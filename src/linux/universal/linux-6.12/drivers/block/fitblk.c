// SPDX-License-Identifier: GPL-2.0-only
/*
 * uImage.FIT virtual block device driver.
 *
 * Copyright (C) 2023 Daniel Golle
 * Copyright (C) 2007 Nick Piggin
 * Copyright (C) 2007 Novell Inc.
 *
 * Initially derived from drivers/block/brd.c which is in parts derived from
 * drivers/block/rd.c, and drivers/block/loop.c, copyright of their respective
 * owners.
 *
 * uImage.FIT headers extracted from Das U-Boot
 *  (C) Copyright 2008 Semihalf
 *  (C) Copyright 2000-2005
 *  Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 */

#include <linux/init.h>
#include <linux/initrd.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/major.h>
#include <linux/blkdev.h>
#include <linux/blkpg.h>
#include <linux/blk-mq.h>
#include <linux/ctype.h>
#include <linux/hdreg.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_fdt.h>
#include <linux/pagemap.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/refcount.h>
#include <linux/task_work.h>
#include <linux/types.h>
#include <linux/libfdt.h>
#include <linux/mtd/mtd.h>
#include <linux/root_dev.h>
#include <uapi/linux/fitblk.h>

#define FIT_DEVICE_PREFIX	"fit"

/* maximum number of pages used for the uImage.FIT index structure */
#define FIT_MAX_PAGES		1024

/* minimum free sectors to map as read-write "remainder" volume */
#define MIN_FREE_SECT		16

/* maximum number of mapped loadables */
#define MAX_FIT_LOADABLES	16

/* constants for uImage.FIT structrure traversal */
#define FIT_IMAGES_PATH		"/images"
#define FIT_CONFS_PATH		"/configurations"

/* hash/signature/key node */
#define FIT_HASH_NODENAME	"hash"
#define FIT_ALGO_PROP		"algo"
#define FIT_VALUE_PROP		"value"
#define FIT_IGNORE_PROP		"uboot-ignore"
#define FIT_SIG_NODENAME	"signature"
#define FIT_KEY_REQUIRED	"required"
#define FIT_KEY_HINT		"key-name-hint"

/* cipher node */
#define FIT_CIPHER_NODENAME	"cipher"
#define FIT_ALGO_PROP		"algo"

/* image node */
#define FIT_DATA_PROP		"data"
#define FIT_DATA_POSITION_PROP	"data-position"
#define FIT_DATA_OFFSET_PROP	"data-offset"
#define FIT_DATA_SIZE_PROP	"data-size"
#define FIT_TIMESTAMP_PROP	"timestamp"
#define FIT_DESC_PROP		"description"
#define FIT_ARCH_PROP		"arch"
#define FIT_TYPE_PROP		"type"
#define FIT_OS_PROP		"os"
#define FIT_COMP_PROP		"compression"
#define FIT_ENTRY_PROP		"entry"
#define FIT_LOAD_PROP		"load"

/* configuration node */
#define FIT_KERNEL_PROP		"kernel"
#define FIT_FILESYSTEM_PROP	"filesystem"
#define FIT_RAMDISK_PROP	"ramdisk"
#define FIT_FDT_PROP		"fdt"
#define FIT_LOADABLE_PROP	"loadables"
#define FIT_DEFAULT_PROP	"default"
#define FIT_SETUP_PROP		"setup"
#define FIT_FPGA_PROP		"fpga"
#define FIT_FIRMWARE_PROP	"firmware"
#define FIT_STANDALONE_PROP	"standalone"

/* fitblk driver data */
static const char *_fitblk_claim_ptr = "I belong to fitblk";
static const char *ubootver;
struct device_node *rootdisk;
static struct platform_device *pdev;
static LIST_HEAD(fitblk_devices);
static DEFINE_MUTEX(devices_mutex);
refcount_t num_devs;

struct fitblk {
	struct platform_device	*pdev;
	struct file		*bdev_file;
	sector_t		start_sect;
	struct gendisk		*disk;
	struct work_struct	remove_work;
	struct list_head	list;
	bool			dead;
};

static int fitblk_open(struct gendisk *disk, fmode_t mode)
{
	struct fitblk *fitblk = disk->private_data;

	if (fitblk->dead)
		return -ENOENT;

	return 0;
}

static void fitblk_release(struct gendisk *disk)
{
	return;
}

static void fitblk_submit_bio(struct bio *orig_bio)
{
	struct bio *bio = orig_bio;
	struct fitblk *fitblk = bio->bi_bdev->bd_disk->private_data;

	if (fitblk->dead)
		return;

	/* mangle bio and re-submit */
	while (bio) {
		bio->bi_iter.bi_sector += fitblk->start_sect;
		bio->bi_bdev = file_bdev(fitblk->bdev_file);
		bio = bio->bi_next;
	}
	submit_bio(orig_bio);
}

static void fitblk_remove(struct fitblk *fitblk)
{
	blk_mark_disk_dead(fitblk->disk);
	mutex_lock(&devices_mutex);
	fitblk->dead = true;
	list_del(&fitblk->list);
	mutex_unlock(&devices_mutex);

	schedule_work(&fitblk->remove_work);
}

static int fitblk_ioctl(struct block_device *bdev, fmode_t mode,
			unsigned int cmd, unsigned long arg)
{
	struct fitblk *fitblk = bdev->bd_disk->private_data;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	if (fitblk->dead)
		return -ENOENT;

	switch (cmd) {
	case FITBLK_RELEASE:
		fitblk_remove(fitblk);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct block_device_operations fitblk_fops = {
	.owner		= THIS_MODULE,
	.ioctl		= fitblk_ioctl,
	.open		= fitblk_open,
	.release	= fitblk_release,
	.submit_bio	= fitblk_submit_bio,
};

static void fitblk_purge(struct work_struct *work)
{
	struct fitblk *fitblk = container_of(work, struct fitblk, remove_work);

	del_gendisk(fitblk->disk);
	refcount_dec(&num_devs);
	platform_device_del(fitblk->pdev);
	platform_device_put(fitblk->pdev);

	if (refcount_dec_if_one(&num_devs)) {
		sysfs_remove_link(&pdev->dev.kobj, "lower_dev");
		fput(fitblk->bdev_file);
	}

	kfree(fitblk);
}

static int add_fit_subimage_device(struct file *bdev_file,
				   unsigned int slot, sector_t start_sect,
				   sector_t nr_sect, bool readonly)
{
	struct block_device *bdev = file_bdev(bdev_file);
	struct fitblk *fitblk;
	struct gendisk *disk;
	int err;

	mutex_lock(&devices_mutex);
	if (!refcount_inc_not_zero(&num_devs))
		return -EBADF;

	fitblk = kzalloc(sizeof(struct fitblk), GFP_KERNEL);
	if (!fitblk) {
		err = -ENOMEM;
		goto out_unlock;
	}

	fitblk->bdev_file = bdev_file;
	fitblk->start_sect = start_sect;
	INIT_WORK(&fitblk->remove_work, fitblk_purge);

	disk = blk_alloc_disk(&bdev->bd_disk->queue->limits, NUMA_NO_NODE);
	if (!disk) {
		err = -ENOMEM;
		goto out_free_fitblk;
	}

	disk->first_minor = 0;
	disk->flags = bdev->bd_disk->flags | GENHD_FL_NO_PART;
	disk->fops = &fitblk_fops;
	disk->private_data = fitblk;
	if (readonly) {
		set_disk_ro(disk, 1);
		snprintf(disk->disk_name, sizeof(disk->disk_name), FIT_DEVICE_PREFIX "%u", slot);
	} else {
		strcpy(disk->disk_name, FIT_DEVICE_PREFIX "rw");
	}

	set_capacity(disk, nr_sect);
	disk->queue->queue_flags = bdev->bd_disk->queue->queue_flags;

	fitblk->disk = disk;
	fitblk->pdev = platform_device_alloc(disk->disk_name, PLATFORM_DEVID_NONE);
	if (!fitblk->pdev) {
		err = -ENOMEM;
		goto out_cleanup_disk;
	}

	fitblk->pdev->dev.parent = &pdev->dev;
	err = platform_device_add(fitblk->pdev);
	if (err)
		goto out_put_pdev;

	err = device_add_disk(&fitblk->pdev->dev, disk, NULL);
	if (err)
		goto out_del_pdev;

	if (!ROOT_DEV)
		ROOT_DEV = disk->part0->bd_dev;

	list_add_tail(&fitblk->list, &fitblk_devices);

	mutex_unlock(&devices_mutex);

	return 0;

out_del_pdev:
	platform_device_del(fitblk->pdev);
out_put_pdev:
	platform_device_put(fitblk->pdev);
out_cleanup_disk:
	put_disk(disk);
out_free_fitblk:
	kfree(fitblk);
out_unlock:
	refcount_dec(&num_devs);
	mutex_unlock(&devices_mutex);
	return err;
}

static void fitblk_mark_dead(struct block_device *bdev, bool surprise)
{
	struct list_head *n, *tmp;
	struct fitblk *fitblk;

	mutex_lock(&devices_mutex);
	list_for_each_safe(n, tmp, &fitblk_devices) {
		fitblk = list_entry(n, struct fitblk, list);
		if (file_bdev(fitblk->bdev_file) != bdev)
			continue;

		fitblk->dead = true;
		list_del(&fitblk->list);
		/* removal needs to be deferred to avoid deadlock */
		schedule_work(&fitblk->remove_work);
	}
	mutex_unlock(&devices_mutex);
}

static const struct blk_holder_ops fitblk_hops = {
	.mark_dead = fitblk_mark_dead,
};

static int parse_fit_on_dev(struct device *dev)
{
	struct file *bdev_file;
	struct block_device *bdev;
	struct address_space *mapping;
	struct folio *folio;
	pgoff_t f_index = 0;
	size_t bytes_left, bytes_to_copy;
	void *pre_fit, *fit, *fit_c;
	u64 dsize, dsectors, imgmaxsect = 0;
	u32 size, image_pos, image_len;
	const __be32 *image_offset_be, *image_len_be, *image_pos_be;
	int ret = 0, node, images, config;
	const char *image_name, *image_type, *image_description,
		*config_default, *config_description, *config_loadables;
	u32 image_name_len, image_type_len, image_description_len,
		bootconf_len, config_default_len, config_description_len,
		config_loadables_len;
	sector_t start_sect, nr_sects;
	struct device_node *np = NULL;
	const char *bootconf_c;
	const char *loadable;
	char *bootconf = NULL, *bootconf_term;
	bool found;
	int loadables_rem_len, loadable_len;
	u16 loadcnt;
	unsigned int slot = 0;

	/* Exclusive open the block device to receive holder notifications */
	bdev_file = bdev_file_open_by_dev(dev->devt,
					  BLK_OPEN_READ | BLK_OPEN_RESTRICT_WRITES,
					  &_fitblk_claim_ptr, &fitblk_hops);
	if (!bdev_file)
		return -ENODEV;

	if (IS_ERR(bdev_file))
		return PTR_ERR(bdev_file);

	bdev = file_bdev(bdev_file);
	mapping = bdev_file->f_mapping;

	/* map first page */
	folio = read_mapping_folio(mapping, f_index++, NULL);
	if (IS_ERR(folio)) {
		ret = PTR_ERR(folio);
		goto out_blkdev;
	}
	pre_fit = folio_address(folio) + offset_in_folio(folio, 0);

	/* uImage.FIT is based on flattened device tree structure */
	if (fdt_check_header(pre_fit)) {
		ret = -EINVAL;
		folio_put(folio);
		goto out_blkdev;
	}

	size = fdt_totalsize(pre_fit);

	if (size > PAGE_SIZE * FIT_MAX_PAGES) {
		ret = -EOPNOTSUPP;
		folio_put(folio);
		goto out_blkdev;
	}

	/* acquire disk size */
	dsectors = bdev_nr_sectors(bdev);
	dsize = dsectors << SECTOR_SHIFT;

	/* abort if FIT structure is larger than disk or partition size */
	if (size >= dsize) {
		ret = -EFBIG;
		folio_put(folio);
		goto out_blkdev;
	}

	fit = kmalloc(size, GFP_KERNEL);
	if (!fit) {
		ret = -ENOMEM;
		folio_put(folio);
		goto out_blkdev;
	}

	bytes_left = size;
	fit_c = fit;
	while (bytes_left > 0) {
		bytes_to_copy = min_t(size_t, bytes_left,
				      folio_size(folio) - offset_in_folio(folio, 0));
		memcpy(fit_c, pre_fit, bytes_to_copy);
		fit_c += bytes_to_copy;
		bytes_left -= bytes_to_copy;
		if (bytes_left) {
			folio_put(folio);
			folio = read_mapping_folio(mapping, f_index++, NULL);
			if (IS_ERR(folio)) {
				ret = PTR_ERR(folio);
				goto out_blkdev;
			};
			pre_fit = folio_address(folio) + offset_in_folio(folio, 0);
		}
	}
	folio_put(folio);

	/* set boot config node name U-Boot may have added to the device tree */
	np = of_find_node_by_path("/chosen");
	if (np) {
		bootconf_c = of_get_property(np, "u-boot,bootconf", &bootconf_len);
		if (bootconf_c && bootconf_len)
			bootconf = kmemdup_nul(bootconf_c, bootconf_len, GFP_KERNEL);
	}

	if (bootconf) {
		bootconf_term = strchr(bootconf, '#');
		if (bootconf_term)
			*bootconf_term = '\0';
	}

	/* find configuration path in uImage.FIT */
	config = fdt_path_offset(fit, FIT_CONFS_PATH);
	if (config < 0) {
		pr_err("FIT: Cannot find %s node: %d\n",
			FIT_CONFS_PATH, config);
		ret = -ENOENT;
		goto out_bootconf;
	}

	/* get default configuration node name */
	config_default =
		fdt_getprop(fit, config, FIT_DEFAULT_PROP, &config_default_len);

	/* make sure we got either default or selected boot config node name */
	if (!config_default && !bootconf) {
		pr_err("FIT: Cannot find default configuration\n");
		ret = -ENOENT;
		goto out_bootconf;
	}

	/* find selected boot config node, fallback on default config node */
	node = fdt_subnode_offset(fit, config, bootconf ?: config_default);
	if (node < 0) {
		pr_err("FIT: Cannot find %s node: %d\n",
			bootconf ?: config_default, node);
		ret = -ENOENT;
		goto out_bootconf;
	}

	pr_info("FIT: Detected U-Boot %s\n", ubootver);

	/* get selected configuration data */
	config_description =
		fdt_getprop(fit, node, FIT_DESC_PROP, &config_description_len);
	config_loadables = fdt_getprop(fit, node, FIT_LOADABLE_PROP,
				       &config_loadables_len);

	pr_info("FIT: %s configuration: \"%.*s\"%s%.*s%s\n",
		bootconf ? "Selected" : "Default",
		bootconf ? bootconf_len : config_default_len,
		bootconf ?: config_default,
		config_description ? " (" : "",
		config_description ? config_description_len : 0,
		config_description ?: "",
		config_description ? ")" : "");

	if (!config_loadables || !config_loadables_len) {
		pr_err("FIT: No loadables configured in \"%s\"\n",
			bootconf ?: config_default);
		ret = -ENOENT;
		goto out_bootconf;
	}

	/* get images path in uImage.FIT */
	images = fdt_path_offset(fit, FIT_IMAGES_PATH);
	if (images < 0) {
		pr_err("FIT: Cannot find %s node: %d\n", FIT_IMAGES_PATH, images);
		ret = -EINVAL;
		goto out_bootconf;
	}

	/* iterate over images in uImage.FIT */
	fdt_for_each_subnode(node, fit, images) {
		image_name = fdt_get_name(fit, node, &image_name_len);
		image_type = fdt_getprop(fit, node, FIT_TYPE_PROP, &image_type_len);
		image_offset_be = fdt_getprop(fit, node, FIT_DATA_OFFSET_PROP, NULL);
		image_pos_be = fdt_getprop(fit, node, FIT_DATA_POSITION_PROP, NULL);
		image_len_be = fdt_getprop(fit, node, FIT_DATA_SIZE_PROP, NULL);

		if (!image_name || !image_type || !image_len_be ||
		    !image_name_len || !image_type_len)
			continue;

		image_len = be32_to_cpu(*image_len_be);
		if (!image_len)
			continue;

		if (image_offset_be)
			image_pos = be32_to_cpu(*image_offset_be) + size;
		else if (image_pos_be)
			image_pos = be32_to_cpu(*image_pos_be);
		else
			continue;

		image_description = fdt_getprop(fit, node, FIT_DESC_PROP,
						&image_description_len);

		pr_info("FIT: %16s sub-image 0x%08x..0x%08x \"%.*s\"%s%.*s%s\n",
			image_type, image_pos, image_pos + image_len - 1,
			image_name_len, image_name, image_description ? " (" : "",
			image_description ? image_description_len : 0,
			image_description ?: "", image_description ? ") " : "");

		/* only 'filesystem' images should be mapped as partitions */
		if (strncmp(image_type, FIT_FILESYSTEM_PROP, image_type_len))
			continue;

		/* check if sub-image is part of configured loadables */
		found = false;
		loadable = config_loadables;
		loadables_rem_len = config_loadables_len;
		for (loadcnt = 0; loadables_rem_len > 1 &&
				  loadcnt < MAX_FIT_LOADABLES; ++loadcnt) {
			loadable_len =
				strnlen(loadable, loadables_rem_len - 1) + 1;
			loadables_rem_len -= loadable_len;
			if (!strncmp(image_name, loadable, loadable_len)) {
				found = true;
				break;
			}
			loadable += loadable_len;
		}
		if (!found)
			continue;

		if (image_pos % (1 << PAGE_SHIFT)) {
			dev_err(dev, "FIT: image %.*s start not aligned to page boundaries, skipping\n",
				image_name_len, image_name);
			continue;
		}

		if (image_len % (1 << PAGE_SHIFT)) {
			dev_err(dev, "FIT: sub-image %.*s end not aligned to page boundaries, skipping\n",
				image_name_len, image_name);
			continue;
		}

		start_sect = image_pos >> SECTOR_SHIFT;
		nr_sects = image_len >> SECTOR_SHIFT;
		imgmaxsect = max_t(sector_t, imgmaxsect, start_sect + nr_sects);

		if (start_sect + nr_sects > dsectors) {
			dev_err(dev, "FIT: sub-image %.*s disk access beyond EOD\n",
				image_name_len, image_name);
			continue;
		}

		if (!slot) {
			ret = sysfs_create_link_nowarn(&pdev->dev.kobj, bdev_kobj(bdev), "lower_dev");
			if (ret && ret != -EEXIST)
				goto out_bootconf;

			ret = 0;
		}

		add_fit_subimage_device(bdev_file, slot++, start_sect, nr_sects, true);
	}

	if (!slot)
		goto out_bootconf;

	dev_info(dev, "mapped %u uImage.FIT filesystem sub-image%s as /dev/fit%s%u%s\n",
		 slot, (slot > 1)?"s":"", (slot > 1)?"[0...":"", slot - 1,
		 (slot > 1)?"]":"");

	/* in case uImage.FIT is stored in a partition, map the remaining space */
	if (!bdev_read_only(bdev) && bdev_is_partition(bdev) &&
	    (imgmaxsect + MIN_FREE_SECT) < dsectors) {
		add_fit_subimage_device(bdev_file, slot++, imgmaxsect,
					dsectors - imgmaxsect, false);
		dev_info(dev, "mapped remaining space as /dev/fitrw\n");
	}

out_bootconf:
	kfree(bootconf);
	kfree(fit);
out_blkdev:
	if (!slot)
		fput(bdev_file);

	return ret;
}

static int fitblk_match_of_node(struct device *dev, const void *np)
{
	int ret;

	ret = device_match_of_node(dev, np);
	if (ret)
		return ret;

	/*
	 * To match ubiblock and mtdblock devices by their parent ubi
	 * or mtd device, also consider block device parent
	 */
	if (!dev->parent)
		return 0;

	return device_match_of_node(dev->parent, np);
}

static int fitblk_probe(struct platform_device *pdev)
{
	struct device *dev;

	dev = class_find_device(&block_class, NULL, rootdisk, fitblk_match_of_node);
	if (!dev)
		return -EPROBE_DEFER;

	return parse_fit_on_dev(dev);
}

static struct platform_driver fitblk_driver = {
	.probe		= fitblk_probe,
	.driver		= {
		.name   = "fitblk",
	},
};

static int __init fitblk_init(void)
{
	/* detect U-Boot firmware */
	ubootver = of_get_property(of_chosen, "u-boot,version", NULL);
	if (!ubootver)
		return 0;

	/* parse 'rootdisk' property phandle */
	rootdisk = of_parse_phandle(of_chosen, "rootdisk", 0);
	if (!rootdisk)
		return 0;

	if (platform_driver_register(&fitblk_driver))
		return -ENODEV;

	refcount_set(&num_devs, 1);
	pdev = platform_device_register_simple("fitblk", -1, NULL, 0);
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);

	return 0;
}
device_initcall(fitblk_init);
