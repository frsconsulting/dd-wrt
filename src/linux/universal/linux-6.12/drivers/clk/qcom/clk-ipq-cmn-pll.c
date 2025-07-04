// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

/*
 * CMN PLL block expects the reference clock from on-board Wi-Fi block, and
 * supplies fixed rate clocks as output to the Ethernet hardware blocks.
 * The Ethernet related blocks include PPE (packet process engine) and the
 * external connected PHY (or switch) chip receiving clocks from the CMN PLL.
 *
 * On the IPQ9574 SoC, There are three clocks with 50 MHZ, one clock with
 * 25 MHZ which are output from the CMN PLL to Ethernet PHY (or switch),
 * and one clock with 353 MHZ to PPE.
 *
 *               +---------+
 *               |   GCC   |
 *               +--+---+--+
 *           AHB CLK|   |SYS CLK
 *                  V   V
 *          +-------+---+------+
 *          |                  +-------------> eth0-50mhz
 * REF CLK  |     IPQ9574      |
 * -------->+                  +-------------> eth1-50mhz
 *          |  CMN PLL block   |
 *          |                  +-------------> eth2-50mhz
 *          |                  |
 *          +---------+--------+-------------> eth-25mhz
 *                    |
 *                    V
 *                    ppe-353mhz
 */

#include <dt-bindings/clock/qcom,ipq-cmn-pll.h>
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define TCSR_ETH_CMN				0x0
#define  TCSR_ETH_CMN_ENABLE			BIT(0)

#define CMN_PLL_REFCLK_SRC_SELECTION		0x28
#define CMN_PLL_REFCLK_SRC_DIV			GENMASK(9, 8)

#define CMN_PLL_REFCLK_CONFIG			0x784
#define CMN_PLL_REFCLK_EXTERNAL			BIT(9)
#define CMN_PLL_REFCLK_DIV			GENMASK(8, 4)
#define CMN_PLL_REFCLK_INDEX			GENMASK(3, 0)

#define CMN_PLL_POWER_ON_AND_RESET		0x780
#define CMN_ANA_EN_SW_RSTN			BIT(6)

/**
 * struct cmn_pll_fixed_output_clk - CMN PLL output clocks information
 * @id:	Clock specifier to be supplied
 * @name: Clock name to be registered
 * @rate: Clock rate
 */
struct cmn_pll_fixed_output_clk {
	unsigned int		id;
	const char		*name;
	const unsigned long	rate;
};

#define CLK_PLL_OUTPUT(_id, _name, _rate) {		\
	.id = _id,					\
	.name = _name,					\
	.rate = _rate,					\
}

static const struct cmn_pll_fixed_output_clk ipq9574_output_clks[] = {
	CLK_PLL_OUTPUT(PPE_353MHZ_CLK, "ppe-353mhz", 353000000UL),
	CLK_PLL_OUTPUT(ETH0_50MHZ_CLK, "eth0-50mhz", 50000000UL),
	CLK_PLL_OUTPUT(ETH1_50MHZ_CLK, "eth1-50mhz", 50000000UL),
	CLK_PLL_OUTPUT(ETH2_50MHZ_CLK, "eth2-50mhz", 50000000UL),
	CLK_PLL_OUTPUT(ETH_25MHZ_CLK, "eth-25mhz", 25000000UL),
};

static int ipq_cmn_pll_tcsr_enable(struct platform_device *pdev)
{
	struct resource *res;
	void __iomem *tcsr_base;
	u32 val;

	/* For IPQ50xx, tcsr is necessary to enable cmn block */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "tcsr");
	if (!res)
		return 0;

	tcsr_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR_OR_NULL(tcsr_base))
		return PTR_ERR(tcsr_base);

	val = readl(tcsr_base + TCSR_ETH_CMN);
	val |= TCSR_ETH_CMN_ENABLE;
	writel(val, (tcsr_base + TCSR_ETH_CMN));

	return 0;
}

static int ipq_cmn_pll_config(struct device *dev, unsigned long parent_rate)
{
	void __iomem *base;
	u32 val;

	base = devm_of_iomap(dev, dev->of_node, 0, NULL);
	if (IS_ERR(base))
		return PTR_ERR(base);

	val = readl(base + CMN_PLL_REFCLK_CONFIG);
	val &= ~(CMN_PLL_REFCLK_EXTERNAL | CMN_PLL_REFCLK_INDEX);

	/*
	 * Configure the reference input clock selection as per the given rate.
	 * The output clock rates are always of fixed value.
	 */
	switch (parent_rate) {
	case 25000000:
		val |= FIELD_PREP(CMN_PLL_REFCLK_INDEX, 3);
		break;
	case 31250000:
		val |= FIELD_PREP(CMN_PLL_REFCLK_INDEX, 4);
		break;
	case 40000000:
		val |= FIELD_PREP(CMN_PLL_REFCLK_INDEX, 6);
		break;
	case 48000000:
		val |= FIELD_PREP(CMN_PLL_REFCLK_INDEX, 7);
		break;
	case 50000000:
		val |= FIELD_PREP(CMN_PLL_REFCLK_INDEX, 8);
		break;
	case 96000000:
		val |= FIELD_PREP(CMN_PLL_REFCLK_INDEX, 7);
		val &= ~CMN_PLL_REFCLK_DIV;
		val |= FIELD_PREP(CMN_PLL_REFCLK_DIV, 2);
		break;
	default:
		return -EINVAL;
	}

	writel(val, base + CMN_PLL_REFCLK_CONFIG);

	/* Update the source clock rate selection. Only 96 MHZ uses 0. */
	val = readl(base + CMN_PLL_REFCLK_SRC_SELECTION);
	val &= ~CMN_PLL_REFCLK_SRC_DIV;
	if (parent_rate != 96000000)
		val |= FIELD_PREP(CMN_PLL_REFCLK_SRC_DIV, 1);

	writel(val, base + CMN_PLL_REFCLK_SRC_SELECTION);

	/*
	 * Reset the CMN PLL block by asserting/de-asserting for 100 ms
	 * each, to ensure the updated configurations take effect.
	 */
	val = readl(base + CMN_PLL_POWER_ON_AND_RESET);
	val &= ~CMN_ANA_EN_SW_RSTN;
	writel(val, base);
	msleep(100);

	val |= CMN_ANA_EN_SW_RSTN;
	writel(val, base + CMN_PLL_POWER_ON_AND_RESET);
	msleep(100);

	return 0;
}

static int ipq_cmn_pll_clk_register(struct device *dev, const char *parent)
{
	const struct cmn_pll_fixed_output_clk *fixed_clk;
	struct clk_hw_onecell_data *data;
	unsigned int num_clks;
	struct clk_hw *hw;
	int i;

	num_clks = ARRAY_SIZE(ipq9574_output_clks);
	fixed_clk = ipq9574_output_clks;

	data = devm_kzalloc(dev, struct_size(data, hws, num_clks), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	for (i = 0; i < num_clks; i++) {
		hw = devm_clk_hw_register_fixed_rate(dev, fixed_clk[i].name,
						     parent, 0,
						     fixed_clk[i].rate);
		if (IS_ERR(hw))
			return PTR_ERR(hw);

		data->hws[fixed_clk[i].id] = hw;
	}
	data->num = num_clks;

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, data);
}

static int ipq_cmn_pll_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk *clk;
	int ret;

	ret = ipq_cmn_pll_tcsr_enable(pdev);
	if (ret)
		return dev_err_probe(dev, ret, "Enable CMN PLL failed\n");

	/*
	 * To access the CMN PLL registers, the GCC AHB & SYSY clocks
	 * for CMN PLL block need to be enabled.
	 */
	clk = devm_clk_get_enabled(dev, "ahb");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk),
				     "Enable AHB clock failed\n");

	clk = devm_clk_get_enabled(dev, "sys");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk),
				     "Enable SYS clock failed\n");

	clk = devm_clk_get(dev, "ref");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk),
				     "Get reference clock failed\n");

	/* Configure CMN PLL to apply the reference clock. */
	ret = ipq_cmn_pll_config(dev, clk_get_rate(clk));
	if (ret)
		return dev_err_probe(dev, ret, "Configure CMN PLL failed\n");

	return ipq_cmn_pll_clk_register(dev, __clk_get_name(clk));
}

static const struct of_device_id ipq_cmn_pll_clk_ids[] = {
	{ .compatible = "qcom,ipq9574-cmn-pll", },
	{ }
};

static struct platform_driver ipq_cmn_pll_clk_driver = {
	.probe = ipq_cmn_pll_clk_probe,
	.driver = {
		.name = "ipq_cmn_pll",
		.of_match_table = ipq_cmn_pll_clk_ids,
	},
};

module_platform_driver(ipq_cmn_pll_clk_driver);

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. IPQ CMN PLL Driver");
MODULE_LICENSE("GPL");
