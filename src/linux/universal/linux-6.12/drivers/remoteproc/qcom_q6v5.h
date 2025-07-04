/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __QCOM_Q6V5_H__
#define __QCOM_Q6V5_H__

#include <linux/kernel.h>
#include <linux/completion.h>
#include <linux/soc/qcom/qcom_aoss.h>

struct icc_path;
struct rproc;
struct qcom_smem_state;
struct qcom_sysmon;

struct qcom_q6v5 {
	struct device *dev;
	struct rproc *rproc;

	struct qcom_smem_state *state;
	struct qmp *qmp;
	struct qcom_smem_state *shutdown_state;
	struct qcom_smem_state *spawn_state;

	struct icc_path *path;

	unsigned stop_bit;
	unsigned shutdown_bit;
	unsigned spawn_bit;

	int wdog_irq;
	int fatal_irq;
	int ready_irq;
	int handover_irq;
	int stop_irq;
	int spawn_irq;

	bool handover_issued;

	struct completion start_done;
	struct completion stop_done;
	struct completion spawn_done;

	int crash_reason;

	bool running;

	const char *load_state;
	void (*handover)(struct qcom_q6v5 *q6v5);
};

int qcom_q6v5_init(struct qcom_q6v5 *q6v5, struct platform_device *pdev,
		   struct rproc *rproc, int crash_reason, const char *load_state,
		   void (*handover)(struct qcom_q6v5 *q6v5));
void qcom_q6v5_deinit(struct qcom_q6v5 *q6v5);

int qcom_q6v5_prepare(struct qcom_q6v5 *q6v5);
int qcom_q6v5_unprepare(struct qcom_q6v5 *q6v5);
int qcom_q6v5_request_stop(struct qcom_q6v5 *q6v5, struct qcom_sysmon *sysmon);
int qcom_q6v5_request_spawn(struct qcom_q6v5 *q6v5);
int qcom_q6v5_wait_for_start(struct qcom_q6v5 *q6v5, int timeout);
unsigned long qcom_q6v5_panic(struct qcom_q6v5 *q6v5);
irqreturn_t q6v5_fatal_interrupt(int irq, void *data);
irqreturn_t q6v5_ready_interrupt(int irq, void *data);
irqreturn_t q6v5_spawn_interrupt(int irq, void *data);
irqreturn_t q6v5_stop_interrupt(int irq, void *data);

#endif
