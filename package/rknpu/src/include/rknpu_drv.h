/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) Rockchip Electronics Co.Ltd
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 *
 * Simplified for mainline Linux 6.18 — no vendor dependencies.
 */

#ifndef __LINUX_RKNPU_DRV_H_
#define __LINUX_RKNPU_DRV_H_

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/kref.h>
#include <linux/irq.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/version.h>
#include <linux/hrtimer.h>
#include <linux/miscdevice.h>

#include "rknpu_job.h"

#define DRIVER_NAME "rknpu"
#define DRIVER_DESC "RKNPU driver"
#define DRIVER_DATE "20240828"
#define DRIVER_MAJOR 0
#define DRIVER_MINOR 9
#define DRIVER_PATCHLEVEL 8

#define LOG_TAG "RKNPU"

#define LOG_INFO(fmt, args...) pr_info(LOG_TAG ": " fmt, ##args)
#define LOG_WARN(fmt, args...) pr_warn(LOG_TAG ": " fmt, ##args)
#define LOG_DEBUG(fmt, args...) pr_devel(LOG_TAG ": " fmt, ##args)
#define LOG_ERROR(fmt, args...) pr_err(LOG_TAG ": " fmt, ##args)

#define LOG_DEV_INFO(dev, fmt, args...) dev_info(dev, LOG_TAG ": " fmt, ##args)
#define LOG_DEV_WARN(dev, fmt, args...) dev_warn(dev, LOG_TAG ": " fmt, ##args)
#define LOG_DEV_DEBUG(dev, fmt, args...) dev_dbg(dev, LOG_TAG ": " fmt, ##args)
#define LOG_DEV_ERROR(dev, fmt, args...) dev_err(dev, LOG_TAG ": " fmt, ##args)

struct rknpu_irqs_data {
	const char *name;
	irqreturn_t (*irq_hdl)(int irq, void *ctx);
};

struct rknpu_config {
	__u64 dma_mask;
	__u32 pc_data_amount_scale;
	__u32 pc_task_number_bits;
	__u32 pc_task_number_mask;
	__u32 pc_task_status_offset;
	__u32 pc_dma_ctrl;
	const struct rknpu_irqs_data *irqs;
	int num_irqs;
	__u64 max_submit_number;
	__u32 core_mask;
};

struct rknpu_timer {
	ktime_t busy_time;
	ktime_t total_busy_time;
};

struct rknpu_subcore_data {
	struct list_head todo_list;
	wait_queue_head_t job_done_wq;
	struct rknpu_job *job;
	int64_t task_num;
	struct rknpu_timer timer;
};

/**
 * RKNPU device — simplified for mainline
 */
struct rknpu_device {
	void __iomem *base[RKNPU_MAX_CORES];
	struct device *dev;
	struct miscdevice miscdev;
	atomic_t sequence;
	spinlock_t lock;
	spinlock_t irq_lock;
	struct mutex power_lock;
	struct mutex reset_lock;
	struct rknpu_subcore_data subcore_datas[RKNPU_MAX_CORES];
	const struct rknpu_config *config;
	bool iommu_en;
	struct reset_control **srsts;
	int num_srsts;
	struct clk_bulk_data *clks;
	int num_clks;
	int bypass_irq_handler;
	int bypass_soft_reset;
	bool soft_reseting;
	struct device *genpd_dev_npu0;
	struct device *genpd_dev_npu1;
	struct device *genpd_dev_npu2;
	bool multiple_domains;
	atomic_t power_refcount;
	atomic_t cmdline_power_refcount;
	struct delayed_work power_off_work;
	struct workqueue_struct *power_off_wq;
	struct hrtimer timer;
	ktime_t kt;
	unsigned long power_put_delay;
	struct dentry *debugfs_dir;
};

struct rknpu_session {
	struct rknpu_device *rknpu_dev;
	struct list_head list;
};

int rknpu_power_get(struct rknpu_device *rknpu_dev);
int rknpu_power_put(struct rknpu_device *rknpu_dev);
int rknpu_power_put_delay(struct rknpu_device *rknpu_dev);

#endif /* __LINUX_RKNPU_DRV_H_ */
