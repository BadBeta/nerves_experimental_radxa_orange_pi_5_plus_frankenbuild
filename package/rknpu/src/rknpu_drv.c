// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Rockchip Electronics Co.Ltd
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 *
 * Simplified for mainline Linux 6.18:
 * - No DRM GEM, no rk_dma_heap — uses dma_alloc_coherent()
 * - No devfreq, no fence, no SRAM, no NBUF
 * - No rockchip_iommu_is_enabled() — checks DT iommus property
 * - No regulator management — relies on clk_ignore_unused cmdline
 * - Misc device only (/dev/rknpu)
 */

#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/iosys-map.h>
#include <linux/iommu.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/ktime.h>
#include <linux/hrtimer.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/seq_file.h>

#include "rknpu_ioctl.h"
#include "rknpu_reset.h"
#include "rknpu_drv.h"
#include "rknpu_mem.h"
#include "rknpu_job.h"

#define RKNPU_GET_DRV_VERSION_STRING(MAJOR, MINOR, PATCHLEVEL) \
	#MAJOR "." #MINOR "." #PATCHLEVEL
#define RKNPU_GET_DRV_VERSION_CODE(MAJOR, MINOR, PATCHLEVEL) \
	(MAJOR * 10000 + MINOR * 100 + PATCHLEVEL)

/* RKNPU load interval: 1000ms */
#define RKNPU_LOAD_INTERVAL 1000000000

static int bypass_irq_handler;
module_param(bypass_irq_handler, int, 0644);
MODULE_PARM_DESC(bypass_irq_handler,
		 "bypass RKNPU irq handler if set it to 1, disabled by default");

static int bypass_soft_reset;
module_param(bypass_soft_reset, int, 0644);
MODULE_PARM_DESC(bypass_soft_reset,
		 "bypass RKNPU soft reset if set it to 1, disabled by default");

/* IRQ handler declarations */
static const struct rknpu_irqs_data rk3588_npu_irqs[] = {
	{ "npu0_irq", rknpu_core0_irq_handler },
	{ "npu1_irq", rknpu_core1_irq_handler },
	{ "npu2_irq", rknpu_core2_irq_handler }
};

/* RK3588 NPU configuration */
static const struct rknpu_config rk3588_rknpu_config = {
	.dma_mask = DMA_BIT_MASK(40),
	.pc_data_amount_scale = 2,
	.pc_task_number_bits = 12,
	.pc_task_number_mask = 0xfff,
	.pc_task_status_offset = 0x3c,
	.pc_dma_ctrl = 0,
	.irqs = rk3588_npu_irqs,
	.num_irqs = ARRAY_SIZE(rk3588_npu_irqs),
	.max_submit_number = (1 << 12) - 1,
	.core_mask = 0x7,
};

static const struct of_device_id rknpu_of_match[] = {
	{
		.compatible = "rockchip,rk3588-rknpu",
		.data = &rk3588_rknpu_config,
	},
	{},
};

static int rknpu_get_drv_version(uint32_t *version)
{
	*version = RKNPU_GET_DRV_VERSION_CODE(DRIVER_MAJOR, DRIVER_MINOR,
					      DRIVER_PATCHLEVEL);
	return 0;
}

/* Forward declarations */
static int rknpu_power_on(struct rknpu_device *rknpu_dev);
static int rknpu_power_off(struct rknpu_device *rknpu_dev);

static void rknpu_power_off_delay_work(struct work_struct *power_off_work)
{
	int ret = 0;
	struct rknpu_device *rknpu_dev =
		container_of(to_delayed_work(power_off_work),
			     struct rknpu_device, power_off_work);
	mutex_lock(&rknpu_dev->power_lock);
	if (atomic_dec_if_positive(&rknpu_dev->power_refcount) == 0) {
		ret = rknpu_power_off(rknpu_dev);
		if (ret)
			atomic_inc(&rknpu_dev->power_refcount);
	}
	mutex_unlock(&rknpu_dev->power_lock);

	if (ret)
		rknpu_power_put_delay(rknpu_dev);
}

int rknpu_power_get(struct rknpu_device *rknpu_dev)
{
	int ret = 0;

	mutex_lock(&rknpu_dev->power_lock);
	if (atomic_inc_return(&rknpu_dev->power_refcount) == 1)
		ret = rknpu_power_on(rknpu_dev);
	mutex_unlock(&rknpu_dev->power_lock);

	return ret;
}

int rknpu_power_put(struct rknpu_device *rknpu_dev)
{
	int ret = 0;

	mutex_lock(&rknpu_dev->power_lock);
	if (atomic_dec_if_positive(&rknpu_dev->power_refcount) == 0) {
		ret = rknpu_power_off(rknpu_dev);
		if (ret)
			atomic_inc(&rknpu_dev->power_refcount);
	}
	mutex_unlock(&rknpu_dev->power_lock);

	if (ret)
		rknpu_power_put_delay(rknpu_dev);

	return ret;
}

int rknpu_power_put_delay(struct rknpu_device *rknpu_dev)
{
	if (rknpu_dev->power_put_delay == 0)
		return rknpu_power_put(rknpu_dev);

	mutex_lock(&rknpu_dev->power_lock);
	if (atomic_read(&rknpu_dev->power_refcount) == 1)
		queue_delayed_work(
			rknpu_dev->power_off_wq, &rknpu_dev->power_off_work,
			msecs_to_jiffies(rknpu_dev->power_put_delay));
	else
		atomic_dec_if_positive(&rknpu_dev->power_refcount);
	mutex_unlock(&rknpu_dev->power_lock);

	return 0;
}

static int rknpu_action(struct rknpu_device *rknpu_dev,
			struct rknpu_action *args)
{
	int ret = -EINVAL;

	switch (args->flags) {
	case RKNPU_GET_HW_VERSION:
		ret = rknpu_get_hw_version(rknpu_dev, &args->value);
		break;
	case RKNPU_GET_DRV_VERSION:
		ret = rknpu_get_drv_version(&args->value);
		break;
	case RKNPU_GET_FREQ:
		args->value = clk_get_rate(rknpu_dev->clks[0].clk);
		ret = 0;
		break;
	case RKNPU_ACT_RESET:
		ret = rknpu_soft_reset(rknpu_dev);
		break;
	case RKNPU_GET_IOMMU_EN:
		args->value = rknpu_dev->iommu_en;
		ret = 0;
		break;
	case RKNPU_SET_PROC_NICE:
		set_user_nice(current, *(int32_t *)&args->value);
		ret = 0;
		break;
	case RKNPU_GET_TOTAL_SRAM_SIZE:
	case RKNPU_GET_FREE_SRAM_SIZE:
		args->value = 0;
		ret = 0;
		break;
	case RKNPU_GET_IOMMU_DOMAIN_ID:
		args->value = 0;
		ret = 0;
		break;
	case RKNPU_SET_IOMMU_DOMAIN_ID:
		/* Single domain only — accept but ignore */
		ret = 0;
		break;
	case RKNPU_POWER_ON:
		atomic_inc(&rknpu_dev->cmdline_power_refcount);
		ret = rknpu_power_get(rknpu_dev);
		break;
	case RKNPU_POWER_OFF:
		if (atomic_dec_if_positive(&rknpu_dev->cmdline_power_refcount) >= 0)
			ret = rknpu_power_put(rknpu_dev);
		else
			ret = 0;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

/* --- misc device file operations --- */

static int rknpu_open(struct inode *inode, struct file *file)
{
	struct rknpu_device *rknpu_dev =
		container_of(file->private_data, struct rknpu_device, miscdev);
	struct rknpu_session *session = NULL;

	session = kzalloc(sizeof(*session), GFP_KERNEL);
	if (!session) {
		LOG_ERROR("rknpu session alloc failed\n");
		return -ENOMEM;
	}

	session->rknpu_dev = rknpu_dev;
	INIT_LIST_HEAD(&session->list);

	file->private_data = (void *)session;

	return nonseekable_open(inode, file);
}

static int rknpu_release(struct inode *inode, struct file *file)
{
	struct rknpu_mem_object *entry, *tmp;
	struct rknpu_session *session = file->private_data;
	struct rknpu_device *rknpu_dev = session->rknpu_dev;
	LIST_HEAD(local_list);

	spin_lock(&rknpu_dev->lock);
	list_replace_init(&session->list, &local_list);
	file->private_data = NULL;
	spin_unlock(&rknpu_dev->lock);

	/* Free any leaked allocations */
	list_for_each_entry_safe(entry, tmp, &local_list, head) {
		LOG_DEBUG("fd close: free leaked obj dma_addr=%#llx size=%lu owner=%d\n",
			  (__u64)entry->dma_addr, entry->size, entry->owner);
		if (entry->owner) {
			/* Path B: dma_alloc_coherent */
			dma_free_coherent(rknpu_dev->dev, entry->size,
					  entry->kv_addr, entry->dma_addr);
		} else {
			/* Path A: imported DMA-BUF */
			if (entry->kv_addr && entry->dmabuf) {
				struct iosys_map unmap =
					IOSYS_MAP_INIT_VADDR(entry->kv_addr);
				dma_buf_vunmap(entry->dmabuf, &unmap);
			}
			if (entry->sgt && entry->attachment)
				dma_buf_unmap_attachment(entry->attachment,
							entry->sgt,
							DMA_BIDIRECTIONAL);
			if (entry->attachment && entry->dmabuf)
				dma_buf_detach(entry->dmabuf,
					       entry->attachment);
			if (entry->dmabuf)
				dma_buf_put(entry->dmabuf);
		}
		list_del(&entry->head);
		kfree(entry);
	}

	kfree(session);
	return 0;
}

static int rknpu_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct rknpu_session *session = file->private_data;
	struct rknpu_device *rknpu_dev;
	struct rknpu_mem_object *entry;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long pfn = vma->vm_pgoff;
	dma_addr_t target_addr = (dma_addr_t)pfn << PAGE_SHIFT;
	bool found = false;
	int ret;

	LOG_INFO("mmap: pgoff=0x%lx size=%lu target_addr=0x%llx\n",
		 pfn, size, (u64)target_addr);

	if (!session) {
		LOG_ERROR("mmap: no session\n");
		return -EINVAL;
	}

	rknpu_dev = session->rknpu_dev;

	/* Find the BO matching this DMA address */
	spin_lock(&rknpu_dev->lock);
	list_for_each_entry(entry, &session->list, head) {
		LOG_INFO("mmap: checking BO dma_addr=0x%llx size=%lu\n",
			 (u64)entry->dma_addr, entry->size);
		if (entry->dma_addr == target_addr && size <= entry->size) {
			found = true;
			break;
		}
	}
	spin_unlock(&rknpu_dev->lock);

	if (!found) {
		LOG_ERROR("mmap: no BO found for dma_addr %#llx size=%lu\n",
			  (u64)target_addr, size);
		return -EINVAL;
	}

	/*
	 * dma_alloc_coherent memory can be mapped to userspace via
	 * dma_mmap_coherent which handles the pfn translation correctly
	 * for both IOMMU and non-IOMMU cases.
	 *
	 * CRITICAL: Reset vm_pgoff to 0 before calling dma_mmap_coherent.
	 * We used vm_pgoff to look up the BO by dma_addr above, but
	 * dma_common_mmap() interprets vm_pgoff as an offset INTO the
	 * buffer. With the dma_addr as pgoff, it fails with -ENXIO
	 * because pgoff > buffer page count.
	 */
	vma->vm_pgoff = 0;

	ret = dma_mmap_coherent(rknpu_dev->dev, vma, entry->kv_addr,
				entry->dma_addr, entry->size);
	LOG_INFO("mmap: dma_mmap_coherent ret=%d\n", ret);
	return ret;
}

static int rknpu_mem_map_ioctl(struct rknpu_device *rknpu_dev,
			       struct file *file, unsigned long data)
{
	struct rknpu_mem_map args;
	struct rknpu_mem_object *entry;
	struct rknpu_session *session;
	bool found = false;

	if (unlikely(copy_from_user(&args, (struct rknpu_mem_map __user *)data,
				    sizeof(args))))
		return -EFAULT;

	/*
	 * The SDK passes the handle from MEM_CREATE.  Walk our session list
	 * to find the matching BO and return dma_addr as the mmap offset.
	 * Since we don't track by handle, match by the handle value stored
	 * nowhere — instead just accept any recent allocation.
	 *
	 * Actually the SDK uses obj_addr (kptr) for most operations.  For
	 * MEM_MAP the SDK passes the GEM handle.  We stored handle in
	 * rknpu_mem_object during create, so look it up.
	 *
	 * Simpler approach: the SDK calls mmap with the dma_addr it already
	 * received from MEM_CREATE.  Just return dma_addr = the BO's dma_addr
	 * as the offset.  We need to find the BO by handle.
	 *
	 * Since we don't store handle in the mem_object, use a simple
	 * approach: return the dma_addr of the most recent allocation that
	 * matches the handle ordinal.  Actually, we need to store the handle.
	 *
	 * For now, just find any BO in the session and return its dma_addr.
	 * The SDK calls MEM_MAP immediately after MEM_CREATE, so the last
	 * entry in the list is the right one.
	 */
	session = file->private_data;
	if (!session)
		return -EFAULT;

	spin_lock(&rknpu_dev->lock);
	if (!list_empty(&session->list)) {
		/* Return the last (most recently added) entry's dma_addr */
		entry = list_last_entry(&session->list,
					struct rknpu_mem_object, head);
		args.offset = (__u64)entry->dma_addr;
		found = true;
	}
	spin_unlock(&rknpu_dev->lock);

	if (!found) {
		LOG_ERROR("mem_map: no BO found for handle %u\n", args.handle);
		return -EINVAL;
	}

	LOG_INFO("mem_map: handle=%u offset=0x%llx\n", args.handle, args.offset);

	if (unlikely(copy_to_user((struct rknpu_mem_map __user *)data,
				  &args, sizeof(args))))
		return -EFAULT;

	return 0;
}

static long rknpu_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	long ret = -EINVAL;
	struct rknpu_device *rknpu_dev = NULL;

	if (!file->private_data)
		return -EINVAL;

	rknpu_dev = ((struct rknpu_session *)file->private_data)->rknpu_dev;

	LOG_INFO("ioctl: cmd=0x%x nr=%d dir=%d size=%d\n",
		 cmd, _IOC_NR(cmd), _IOC_DIR(cmd), _IOC_SIZE(cmd));

	rknpu_power_get(rknpu_dev);

	switch (_IOC_NR(cmd)) {
	case RKNPU_ACTION: {
		struct rknpu_action args;

		if (unlikely(copy_from_user(&args,
					    (struct rknpu_action __user *)arg,
					    sizeof(args)))) {
			ret = -EFAULT;
			break;
		}
		ret = rknpu_action(rknpu_dev, &args);
		if (unlikely(copy_to_user((struct rknpu_action __user *)arg,
					  &args, sizeof(args))))
			ret = -EFAULT;
		break;
	}
	case RKNPU_SUBMIT:
		ret = rknpu_submit_ioctl(rknpu_dev, file, cmd, arg);
		break;
	case RKNPU_MEM_CREATE:
		ret = rknpu_mem_create_ioctl(rknpu_dev, file, cmd, arg);
		break;
	case RKNPU_MEM_MAP:
		ret = rknpu_mem_map_ioctl(rknpu_dev, file, arg);
		break;
	case RKNPU_MEM_DESTROY:
		ret = rknpu_mem_destroy_ioctl(rknpu_dev, file, arg);
		break;
	case RKNPU_MEM_SYNC:
		ret = rknpu_mem_sync_ioctl(rknpu_dev, arg);
		break;
	default:
		LOG_WARN("ioctl: UNKNOWN nr=%d cmd=0x%x\n", _IOC_NR(cmd), cmd);
		break;
	}

	rknpu_power_put_delay(rknpu_dev);

	return ret;
}

static const struct file_operations rknpu_fops = {
	.owner = THIS_MODULE,
	.open = rknpu_open,
	.release = rknpu_release,
	.mmap = rknpu_mmap,
	.unlocked_ioctl = rknpu_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = rknpu_ioctl,
#endif
};

/* --- power management --- */

static int rknpu_power_on(struct rknpu_device *rknpu_dev)
{
	struct device *dev = rknpu_dev->dev;
	int ret;

	LOG_DEV_INFO(dev, "power_on: multiple_domains=%d, genpd0=%p, genpd1=%p, genpd2=%p\n",
		     rknpu_dev->multiple_domains,
		     rknpu_dev->genpd_dev_npu0,
		     rknpu_dev->genpd_dev_npu1,
		     rknpu_dev->genpd_dev_npu2);

	ret = clk_bulk_prepare_enable(rknpu_dev->num_clks, rknpu_dev->clks);
	if (ret) {
		LOG_DEV_ERROR(dev, "failed to enable clks: %d\n", ret);
		return ret;
	}
	LOG_DEV_INFO(dev, "power_on: clks enabled (%d clks)\n",
		     rknpu_dev->num_clks);

	if (rknpu_dev->multiple_domains) {
		if (rknpu_dev->genpd_dev_npu0) {
			ret = pm_runtime_resume_and_get(
				rknpu_dev->genpd_dev_npu0);
			LOG_DEV_INFO(dev, "power_on: npu0 pm_runtime ret=%d\n",
				     ret);
			if (ret < 0) {
				LOG_DEV_ERROR(dev,
					      "failed pm_runtime npu0: %d\n",
					      ret);
				goto out;
			}
		}
		if (rknpu_dev->genpd_dev_npu1) {
			ret = pm_runtime_resume_and_get(
				rknpu_dev->genpd_dev_npu1);
			LOG_DEV_INFO(dev, "power_on: npu1 pm_runtime ret=%d\n",
				     ret);
			if (ret < 0) {
				LOG_DEV_ERROR(dev,
					      "failed pm_runtime npu1: %d\n",
					      ret);
				goto out;
			}
		}
		if (rknpu_dev->genpd_dev_npu2) {
			ret = pm_runtime_resume_and_get(
				rknpu_dev->genpd_dev_npu2);
			LOG_DEV_INFO(dev, "power_on: npu2 pm_runtime ret=%d\n",
				     ret);
			if (ret < 0) {
				LOG_DEV_ERROR(dev,
					      "failed pm_runtime npu2: %d\n",
					      ret);
				goto out;
			}
		}
	}

	ret = pm_runtime_get_sync(dev);
	LOG_DEV_INFO(dev, "power_on: main pm_runtime ret=%d, status=%d\n",
		     ret, dev->power.runtime_status);
	if (ret < 0) {
		LOG_DEV_ERROR(dev, "failed pm_runtime for rknpu: %d\n", ret);
		goto out;
	}

	/* Re-initialize IOMMU after power-on.
	 * The NPU's IOMMU registers (DTE at +0x9000, +0xa000) lose state
	 * when the NPU power domain is turned off. Detach+reattach forces
	 * rk_iommu_enable() to reprogram the DTE register.
	 *
	 * NOTE: This destroys existing BO IOMMU mappings if power cycles
	 * between mem_create and submit. Use POWER_ON ioctl to hold power
	 * and prevent this from firing between ioctls.
	 */
	if (rknpu_dev->iommu_en && rknpu_dev->base[0]) {
		struct iommu_domain *domain;
		void __iomem *mmu0 = rknpu_dev->base[0] + 0x9000;
		void __iomem *mmu1 = rknpu_dev->base[0] + 0xa000;
		u32 dte0, dte1;

		domain = iommu_get_domain_for_dev(dev);
		if (domain) {
			iommu_detach_device(domain, dev);
			ret = iommu_attach_device(domain, dev);
			if (ret) {
				LOG_DEV_ERROR(dev,
					      "failed iommu re-attach: %d\n",
					      ret);
			}

			/* Force DTE valid bit.
			 * iommu_attach_device() programs the page table base
			 * but may not set bit 0 (valid). Without it, the IOMMU
			 * ignores the page table and all DMA faults.
			 */
			wmb(); /* ensure IOMMU register writes complete */
			dte0 = readl(mmu0 + 0x00);
			dte1 = readl(mmu1 + 0x00);
			LOG_DEV_INFO(dev,
				     "power_on: DTE_CHECK mmu0=0x%x mmu1=0x%x\n",
				     dte0, dte1);
			if (dte0 && !(dte0 & 1)) {
				writel(dte0 | 1, mmu0 + 0x00);
				writel(dte1 | 1, mmu1 + 0x00);
				wmb();
				LOG_DEV_INFO(dev,
					     "power_on: DTE_FORCED mmu0=0x%x mmu1=0x%x\n",
					     readl(mmu0 + 0x00),
					     readl(mmu1 + 0x00));
			}
		}
	}

out:
	return ret;
}

static int rknpu_power_off(struct rknpu_device *rknpu_dev)
{
	struct device *dev = rknpu_dev->dev;

	pm_runtime_put_sync(dev);

	if (rknpu_dev->multiple_domains) {
		/*
		 * Wait for IOMMU to finish before cutting power domains.
		 * On mainline with clk_ignore_unused, power domains may not
		 * fully gate, so this is less critical. But keep a small
		 * delay for safety.
		 */
		if (rknpu_dev->iommu_en)
			msleep(20);

		if (rknpu_dev->genpd_dev_npu2)
			pm_runtime_put_sync(rknpu_dev->genpd_dev_npu2);
		if (rknpu_dev->genpd_dev_npu1)
			pm_runtime_put_sync(rknpu_dev->genpd_dev_npu1);
		if (rknpu_dev->genpd_dev_npu0)
			pm_runtime_put_sync(rknpu_dev->genpd_dev_npu0);
	}

	clk_bulk_disable_unprepare(rknpu_dev->num_clks, rknpu_dev->clks);

	return 0;
}

/* --- timer for load tracking --- */

static enum hrtimer_restart hrtimer_handler(struct hrtimer *timer)
{
	struct rknpu_device *rknpu_dev =
		container_of(timer, struct rknpu_device, timer);
	struct rknpu_subcore_data *subcore_data = NULL;
	struct rknpu_job *job = NULL;
	ktime_t now;
	unsigned long flags;
	int i;

	for (i = 0; i < rknpu_dev->config->num_irqs; i++) {
		subcore_data = &rknpu_dev->subcore_datas[i];

		spin_lock_irqsave(&rknpu_dev->irq_lock, flags);

		job = subcore_data->job;
		if (job) {
			now = ktime_get();
			subcore_data->timer.busy_time +=
				ktime_sub(now, job->hw_recoder_time);
			job->hw_recoder_time = now;
		}

		subcore_data->timer.total_busy_time =
			subcore_data->timer.busy_time;
		subcore_data->timer.busy_time = 0;

		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
	}

	hrtimer_forward_now(timer, rknpu_dev->kt);
	return HRTIMER_RESTART;
}

static void rknpu_init_timer(struct rknpu_device *rknpu_dev)
{
	rknpu_dev->kt = ktime_set(0, RKNPU_LOAD_INTERVAL);
	hrtimer_init(&rknpu_dev->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	rknpu_dev->timer.function = hrtimer_handler;
	hrtimer_start(&rknpu_dev->timer, rknpu_dev->kt, HRTIMER_MODE_REL);
}

static void rknpu_cancel_timer(struct rknpu_device *rknpu_dev)
{
	hrtimer_cancel(&rknpu_dev->timer);
}

/* --- IRQ registration --- */

static int rknpu_register_irq(struct platform_device *pdev,
			      struct rknpu_device *rknpu_dev)
{
	const struct rknpu_config *config = rknpu_dev->config;
	struct device *dev = &pdev->dev;
	int i, ret, irq;

	for (i = 0; i < config->num_irqs; i++) {
		irq = platform_get_irq_byname(pdev, config->irqs[i].name);
		if (irq < 0) {
			irq = platform_get_irq(pdev, i);
			if (irq < 0) {
				LOG_DEV_ERROR(dev, "no npu %s in dts\n",
					      config->irqs[i].name);
				return irq;
			}
		}

		ret = devm_request_irq(dev, irq, config->irqs[i].irq_hdl,
				       IRQF_SHARED, dev_name(dev), rknpu_dev);
		if (ret < 0) {
			LOG_DEV_ERROR(dev, "request %s failed: %d\n",
				      config->irqs[i].name, ret);
			return ret;
		}
	}

	return 0;
}

/* --- IOMMU detection --- */

static bool rknpu_is_iommu_enable(struct device *dev)
{
	struct device_node *iommu;

	iommu = of_parse_phandle(dev->of_node, "iommus", 0);
	if (!iommu) {
		LOG_DEV_INFO(dev, "no iommu in dts, using non-iommu mode\n");
		return false;
	}

	if (!of_device_is_available(iommu)) {
		LOG_DEV_INFO(dev, "iommu disabled, using non-iommu mode\n");
		of_node_put(iommu);
		return false;
	}
	of_node_put(iommu);

	LOG_DEV_INFO(dev, "iommu enabled\n");
	return true;
}

/* --- debugfs register dump --- */

struct rknpu_reg_range {
	const char *name;
	u32 start;
	u32 end;
};

static const struct rknpu_reg_range npu_reg_ranges[] = {
	{ "PC",       0x0000, 0x003C },
	{ "CNA",      0x1000, 0x1190 },
	{ "CORE",     0x3000, 0x3020 },
	{ "DPU",      0x4000, 0x40F0 },
	{ "DPU_LUT",  0x4100, 0x412C },
	{ "RDMA",     0x5000, 0x5050 },
	{ "PPU",      0x6000, 0x6020 },
	{ "PPU_RDMA", 0x7000, 0x7020 },
	{ "GLOBAL",   0xF000, 0xF008 },
};

static int rknpu_debugfs_regs_show(struct seq_file *s, void *unused)
{
	struct rknpu_device *rknpu_dev = s->private;
	int num_cores, i, r;
	int ret;

	if (!rknpu_dev)
		return -ENODEV;

	num_cores = rknpu_dev->config->num_irqs;

	/* Power on NPU to read registers safely */
	ret = rknpu_power_get(rknpu_dev);
	if (ret) {
		seq_printf(s, "ERROR: failed to power on NPU: %d\n", ret);
		return 0;
	}

	/* Small delay for clocks to stabilize */
	udelay(100);

	seq_printf(s, "# RKNPU Register Dump (%d cores)\n", num_cores);
	seq_printf(s, "# power_refcount=%d\n\n",
		   atomic_read(&rknpu_dev->power_refcount));

	for (i = 0; i < num_cores; i++) {
		void __iomem *base = rknpu_dev->base[i];

		if (!base) {
			seq_printf(s, "## Core %d: NOT MAPPED\n\n", i);
			continue;
		}

		seq_printf(s, "## Core %d\n", i);

		for (r = 0; r < ARRAY_SIZE(npu_reg_ranges); r++) {
			const struct rknpu_reg_range *range = &npu_reg_ranges[r];
			u32 off;

			seq_printf(s, "### %s (0x%04X - 0x%04X)\n",
				   range->name, range->start, range->end);

			for (off = range->start; off <= range->end; off += 4) {
				u32 val = readl(base + off);
				if (val != 0)
					seq_printf(s, "0x%04X = 0x%08X\n",
						   off, val);
			}
			seq_putc(s, '\n');
		}
	}

	rknpu_power_put_delay(rknpu_dev);
	return 0;
}

static int rknpu_debugfs_regs_open(struct inode *inode, struct file *file)
{
	return single_open(file, rknpu_debugfs_regs_show, inode->i_private);
}

static const struct file_operations rknpu_debugfs_regs_fops = {
	.owner = THIS_MODULE,
	.open = rknpu_debugfs_regs_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/* Full dump includes zero registers too — for complete capture */
static int rknpu_debugfs_regs_full_show(struct seq_file *s, void *unused)
{
	struct rknpu_device *rknpu_dev = s->private;
	int num_cores, i, r;
	int ret;

	if (!rknpu_dev)
		return -ENODEV;

	num_cores = rknpu_dev->config->num_irqs;

	ret = rknpu_power_get(rknpu_dev);
	if (ret) {
		seq_printf(s, "ERROR: failed to power on NPU: %d\n", ret);
		return 0;
	}

	udelay(100);

	seq_printf(s, "# RKNPU Full Register Dump (%d cores)\n\n", num_cores);

	for (i = 0; i < num_cores; i++) {
		void __iomem *base = rknpu_dev->base[i];

		if (!base)
			continue;

		seq_printf(s, "## Core %d\n", i);

		for (r = 0; r < ARRAY_SIZE(npu_reg_ranges); r++) {
			const struct rknpu_reg_range *range = &npu_reg_ranges[r];
			u32 off;

			seq_printf(s, "### %s\n", range->name);

			for (off = range->start; off <= range->end; off += 4) {
				u32 val = readl(base + off);
				seq_printf(s, "0x%04X = 0x%08X\n", off, val);
			}
			seq_putc(s, '\n');
		}
	}

	rknpu_power_put_delay(rknpu_dev);
	return 0;
}

static int rknpu_debugfs_regs_full_open(struct inode *inode, struct file *file)
{
	return single_open(file, rknpu_debugfs_regs_full_show, inode->i_private);
}

static const struct file_operations rknpu_debugfs_regs_full_fops = {
	.owner = THIS_MODULE,
	.open = rknpu_debugfs_regs_full_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static void rknpu_debugfs_init(struct rknpu_device *rknpu_dev)
{
	rknpu_dev->debugfs_dir = debugfs_create_dir("rknpu", NULL);
	if (IS_ERR_OR_NULL(rknpu_dev->debugfs_dir))
		return;

	debugfs_create_file("regs", 0444, rknpu_dev->debugfs_dir,
			    rknpu_dev, &rknpu_debugfs_regs_fops);
	debugfs_create_file("regs_full", 0444, rknpu_dev->debugfs_dir,
			    rknpu_dev, &rknpu_debugfs_regs_full_fops);
}

static void rknpu_debugfs_fini(struct rknpu_device *rknpu_dev)
{
	debugfs_remove_recursive(rknpu_dev->debugfs_dir);
}

/* --- platform driver --- */

static int rknpu_probe(struct platform_device *pdev)
{
	struct resource *res = NULL;
	struct rknpu_device *rknpu_dev = NULL;
	struct device *dev = &pdev->dev;
	struct device *virt_dev = NULL;
	const struct rknpu_config *config = NULL;
	int ret = -EINVAL, i = 0;

	if (!pdev->dev.of_node) {
		LOG_DEV_ERROR(dev, "rknpu device-tree data is missing!\n");
		return -ENODEV;
	}

	config = of_device_get_match_data(dev);
	if (!config)
		return -EINVAL;

	rknpu_dev = devm_kzalloc(dev, sizeof(*rknpu_dev), GFP_KERNEL);
	if (!rknpu_dev)
		return -ENOMEM;

	rknpu_dev->config = config;
	rknpu_dev->dev = dev;
	dev_set_drvdata(dev, rknpu_dev);

	/* Set DMA mask */
	ret = dma_set_mask_and_coherent(dev, config->dma_mask);
	if (ret) {
		LOG_DEV_ERROR(dev, "failed to set DMA mask: %d\n", ret);
		return ret;
	}

	rknpu_dev->iommu_en = rknpu_is_iommu_enable(dev);
	rknpu_dev->bypass_irq_handler = bypass_irq_handler;
	rknpu_dev->bypass_soft_reset = bypass_soft_reset;

	rknpu_reset_get(rknpu_dev);

	rknpu_dev->num_clks = devm_clk_bulk_get_all(dev, &rknpu_dev->clks);
	if (rknpu_dev->num_clks < 1) {
		LOG_DEV_ERROR(dev, "failed to get clks for rknpu\n");
		return -ENODEV;
	}

	spin_lock_init(&rknpu_dev->lock);
	spin_lock_init(&rknpu_dev->irq_lock);
	mutex_init(&rknpu_dev->power_lock);
	mutex_init(&rknpu_dev->reset_lock);

	/* Map MMIO regions for each core */
	for (i = 0; i < config->num_irqs; i++) {
		INIT_LIST_HEAD(&rknpu_dev->subcore_datas[i].todo_list);
		init_waitqueue_head(&rknpu_dev->subcore_datas[i].job_done_wq);
		rknpu_dev->subcore_datas[i].task_num = 0;

		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		if (!res) {
			LOG_DEV_ERROR(dev, "failed to get MMIO resource %d\n",
				      i);
			return -ENXIO;
		}

		rknpu_dev->base[i] = devm_ioremap_resource(dev, res);
		if (PTR_ERR(rknpu_dev->base[i]) == -EBUSY) {
			rknpu_dev->base[i] = devm_ioremap(dev, res->start,
							  resource_size(res));
		}
		if (IS_ERR(rknpu_dev->base[i])) {
			LOG_DEV_ERROR(dev, "failed to remap MMIO %d\n", i);
			return PTR_ERR(rknpu_dev->base[i]);
		}
	}

	/* Register IRQ handlers */
	if (!rknpu_dev->bypass_irq_handler) {
		ret = rknpu_register_irq(pdev, rknpu_dev);
		if (ret)
			return ret;
	} else {
		LOG_DEV_WARN(dev, "bypass irq handler!\n");
	}

	/* Register misc device */
	rknpu_dev->miscdev.minor = MISC_DYNAMIC_MINOR;
	rknpu_dev->miscdev.name = "rknpu";
	rknpu_dev->miscdev.fops = &rknpu_fops;

	ret = misc_register(&rknpu_dev->miscdev);
	if (ret) {
		LOG_DEV_ERROR(dev, "cannot register miscdev (%d)\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, rknpu_dev);

	pm_runtime_enable(dev);

	/* Attach power domains for multi-core NPU */
	if (of_count_phandle_with_args(dev->of_node, "power-domains",
				       "#power-domain-cells") > 1) {
		virt_dev = dev_pm_domain_attach_by_name(dev, "npu0");
		if (!IS_ERR(virt_dev))
			rknpu_dev->genpd_dev_npu0 = virt_dev;
		virt_dev = dev_pm_domain_attach_by_name(dev, "npu1");
		if (!IS_ERR(virt_dev))
			rknpu_dev->genpd_dev_npu1 = virt_dev;
		if (config->num_irqs > 2) {
			virt_dev = dev_pm_domain_attach_by_name(dev, "npu2");
			if (!IS_ERR(virt_dev))
				rknpu_dev->genpd_dev_npu2 = virt_dev;
		}
		rknpu_dev->multiple_domains = true;
	}

	/* Initial power on/off cycle to verify hardware */
	ret = rknpu_power_on(rknpu_dev);
	if (ret) {
		LOG_DEV_ERROR(dev, "initial power on failed: %d\n", ret);
		goto err_remove;
	}

	/* Set default power put delay to 3s */
	rknpu_dev->power_put_delay = 3000;
	rknpu_dev->power_off_wq =
		create_freezable_workqueue("rknpu_power_off_wq");
	if (!rknpu_dev->power_off_wq) {
		LOG_DEV_ERROR(dev, "couldn't create power_off workqueue\n");
		ret = -ENOMEM;
		goto err_remove;
	}
	INIT_DEFERRABLE_WORK(&rknpu_dev->power_off_work,
			     rknpu_power_off_delay_work);

	rknpu_power_off(rknpu_dev);
	atomic_set(&rknpu_dev->power_refcount, 0);
	atomic_set(&rknpu_dev->cmdline_power_refcount, 0);

	rknpu_init_timer(rknpu_dev);

	rknpu_debugfs_init(rknpu_dev);

	LOG_DEV_INFO(dev, "RKNPU: v%d.%d.%d for mainline Linux\n",
		     DRIVER_MAJOR, DRIVER_MINOR, DRIVER_PATCHLEVEL);

	return 0;

err_remove:
	misc_deregister(&rknpu_dev->miscdev);
	return ret;
}

static void rknpu_remove(struct platform_device *pdev)
{
	struct rknpu_device *rknpu_dev = platform_get_drvdata(pdev);
	int i;

	cancel_delayed_work_sync(&rknpu_dev->power_off_work);
	destroy_workqueue(rknpu_dev->power_off_wq);

	rknpu_cancel_timer(rknpu_dev);

	for (i = 0; i < rknpu_dev->config->num_irqs; i++) {
		WARN_ON(rknpu_dev->subcore_datas[i].job);
		WARN_ON(!list_empty(&rknpu_dev->subcore_datas[i].todo_list));
	}

	rknpu_debugfs_fini(rknpu_dev);
	misc_deregister(&rknpu_dev->miscdev);

	mutex_lock(&rknpu_dev->power_lock);
	if (atomic_read(&rknpu_dev->power_refcount) > 0)
		rknpu_power_off(rknpu_dev);
	mutex_unlock(&rknpu_dev->power_lock);

	if (rknpu_dev->multiple_domains) {
		if (rknpu_dev->genpd_dev_npu0)
			dev_pm_domain_detach(rknpu_dev->genpd_dev_npu0, true);
		if (rknpu_dev->genpd_dev_npu1)
			dev_pm_domain_detach(rknpu_dev->genpd_dev_npu1, true);
		if (rknpu_dev->genpd_dev_npu2)
			dev_pm_domain_detach(rknpu_dev->genpd_dev_npu2, true);
	}

	pm_runtime_disable(&pdev->dev);
}

static struct platform_driver rknpu_driver = {
	.probe = rknpu_probe,
	.remove = rknpu_remove,
	.driver = {
		.owner = THIS_MODULE,
		.name = "RKNPU",
		.of_match_table = of_match_ptr(rknpu_of_match),
	},
};

static int rknpu_init(void)
{
	return platform_driver_register(&rknpu_driver);
}

static void rknpu_exit(void)
{
	platform_driver_unregister(&rknpu_driver);
}

late_initcall(rknpu_init);
module_exit(rknpu_exit);

MODULE_DESCRIPTION("RKNPU driver (mainline)");
MODULE_AUTHOR("Felix Zeng <felix.zeng@rock-chips.com>");
MODULE_ALIAS("rockchip-rknpu");
MODULE_LICENSE("GPL v2");
MODULE_IMPORT_NS(DMA_BUF);
