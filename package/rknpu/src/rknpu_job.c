// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Rockchip Electronics Co.Ltd
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 *
 * Simplified for mainline Linux 6.18 — no DRM GEM, no fence, no DMA heap.
 * Uses dma_alloc_coherent() memory objects.
 */

#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/io.h>
#include <linux/uaccess.h>

#include "rknpu_ioctl.h"
#include "rknpu_drv.h"
#include "rknpu_reset.h"
#include "rknpu_mem.h"
#include "rknpu_job.h"

#define _REG_READ(base, offset) readl(base + (offset))
#define _REG_WRITE(base, value, offset) writel(value, base + (offset))

#define REG_READ(offset) _REG_READ(rknpu_core_base, offset)
#define REG_WRITE(value, offset) _REG_WRITE(rknpu_core_base, value, offset)

static int rknpu_wait_core_index(int core_mask)
{
	int index = 0;

	switch (core_mask) {
	case RKNPU_CORE0_MASK:
	case RKNPU_CORE0_MASK | RKNPU_CORE1_MASK:
	case RKNPU_CORE0_MASK | RKNPU_CORE1_MASK | RKNPU_CORE2_MASK:
		index = 0;
		break;
	case RKNPU_CORE1_MASK:
		index = 1;
		break;
	case RKNPU_CORE2_MASK:
		index = 2;
		break;
	default:
		break;
	}

	return index;
}

static int rknpu_core_mask(int core_index)
{
	int core_mask = RKNPU_CORE_AUTO_MASK;

	switch (core_index) {
	case 0:
		core_mask = RKNPU_CORE0_MASK;
		break;
	case 1:
		core_mask = RKNPU_CORE1_MASK;
		break;
	case 2:
		core_mask = RKNPU_CORE2_MASK;
		break;
	default:
		break;
	}

	return core_mask;
}

static int rknpu_get_task_number(struct rknpu_job *job, int core_index)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	int task_num = job->args->task_number;

	if (core_index >= RKNPU_MAX_CORES || core_index < 0) {
		LOG_ERROR("invalid rknpu core index: %d", core_index);
		return 0;
	}

	if (rknpu_dev->config->num_irqs > 1) {
		if (job->use_core_num == 1 || job->use_core_num == 2)
			task_num =
				job->args->subcore_task[core_index].task_number;
		else if (job->use_core_num == 3)
			task_num = job->args->subcore_task[core_index + 2]
					   .task_number;
	}

	return task_num;
}

static void rknpu_job_free(struct rknpu_job *job)
{
	if (job->args_owner)
		kfree(job->args);
	kfree(job);
}

static int rknpu_job_cleanup(struct rknpu_job *job)
{
	rknpu_job_free(job);
	return 0;
}

static void rknpu_job_cleanup_work(struct work_struct *work)
{
	struct rknpu_job *job =
		container_of(work, struct rknpu_job, cleanup_work);
	rknpu_job_cleanup(job);
}

static inline struct rknpu_job *rknpu_job_alloc(struct rknpu_device *rknpu_dev,
						struct rknpu_submit *args)
{
	struct rknpu_job *job = NULL;
	int i = 0;

	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job)
		return NULL;

	job->timestamp = ktime_get();
	job->rknpu_dev = rknpu_dev;
	job->use_core_num = (args->core_mask & RKNPU_CORE0_MASK) +
			    ((args->core_mask & RKNPU_CORE1_MASK) >> 1) +
			    ((args->core_mask & RKNPU_CORE2_MASK) >> 2);
	atomic_set(&job->run_count, job->use_core_num);
	atomic_set(&job->interrupt_count, job->use_core_num);
	for (i = 0; i < rknpu_dev->config->num_irqs; i++)
		atomic_set(&job->submit_count[i], 0);

	if (!(args->flags & RKNPU_JOB_NONBLOCK)) {
		job->args = args;
		job->args_owner = false;
		return job;
	}

	job->args = kzalloc(sizeof(*args), GFP_KERNEL);
	if (!job->args) {
		kfree(job);
		return NULL;
	}
	*job->args = *args;
	job->args_owner = true;

	INIT_WORK(&job->cleanup_work, rknpu_job_cleanup_work);

	return job;
}

static inline int rknpu_job_wait(struct rknpu_job *job)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	struct rknpu_submit *args = job->args;
	struct rknpu_task *last_task = NULL;
	struct rknpu_subcore_data *subcore_data = NULL;
	struct rknpu_job *entry, *q;
	void __iomem *rknpu_core_base = NULL;
	int core_index = rknpu_wait_core_index(job->args->core_mask);
	unsigned long flags;
	int wait_count = 0;
	bool continue_wait = false;
	int ret = -EINVAL;
	int i = 0;

	subcore_data = &rknpu_dev->subcore_datas[core_index];

	do {
		ret = wait_event_timeout(subcore_data->job_done_wq,
					 job->flags & RKNPU_JOB_DONE ||
						 rknpu_dev->soft_reseting,
					 msecs_to_jiffies(args->timeout));

		if (++wait_count >= 3)
			break;

		if (ret == 0) {
			int64_t elapse_time_us = 0;
			spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
			elapse_time_us = ktime_us_delta(ktime_get(),
							job->hw_commit_time);
			continue_wait =
				job->hw_commit_time == 0 ?
					true :
					(elapse_time_us < args->timeout * 1000);
			spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

			/* Poll NPU state at each timeout iteration */
			{
				void __iomem *cb = rknpu_dev->base[core_index];
				u32 tc = readl(cb + rknpu_dev->config->pc_task_status_offset) &
					 rknpu_dev->config->pc_task_number_mask;
				u32 rs = readl(cb + RKNPU_OFFSET_INT_RAW_STATUS);
				u32 is = readl(cb + RKNPU_OFFSET_INT_STATUS);
				u32 da = readl(cb + RKNPU_OFFSET_PC_DATA_ADDR);
				u32 dam = readl(cb + RKNPU_OFFSET_PC_DATA_AMOUNT);
				LOG_ERROR(
					"poll[%d]: task_cnt=%u raw=0x%x int=0x%x "
					"pc_addr=0x%x pc_amt=%u elapsed=%lldus\n",
					wait_count, tc, rs, is, da, dam,
					elapse_time_us);
			}

			LOG_ERROR(
				"job: %p, mask: %#x, wait_count: %d, continue wait: %d, commit elapse: %lldus, timeout: %uus\n",
				job, args->core_mask, wait_count,
				continue_wait,
				(job->hw_commit_time == 0 ? 0 : elapse_time_us),
				args->timeout * 1000);
		}
	} while (ret == 0 && continue_wait);

	last_task = job->last_task;
	if (!last_task) {
		spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
		for (i = 0; i < job->use_core_num; i++) {
			subcore_data = &rknpu_dev->subcore_datas[i];
			list_for_each_entry_safe(
				entry, q, &subcore_data->todo_list, head[i]) {
				if (entry == job) {
					list_del(&job->head[i]);
					break;
				}
			}
		}
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

		LOG_ERROR("job commit failed\n");
		return ret < 0 ? ret : -EINVAL;
	}

	last_task->int_status = job->int_status[core_index];

	if (ret <= 0) {
		args->task_counter = 0;
		rknpu_core_base = rknpu_dev->base[core_index];
		if (args->flags & RKNPU_JOB_PC) {
			uint32_t task_status = REG_READ(
				rknpu_dev->config->pc_task_status_offset);
			args->task_counter =
				(task_status &
				 rknpu_dev->config->pc_task_number_mask);
		}

		LOG_ERROR(
			"failed to wait job, task counter: %d, flags: %#x, ret = %d, elapsed: %lldus\n",
			args->task_counter, args->flags, ret,
			ktime_us_delta(ktime_get(), job->timestamp));

		/* Comprehensive timeout diagnostics */
		if (rknpu_core_base) {
			int r;
			LOG_ERROR("TIMEOUT DIAG: PC registers:\n");
			for (r = 0; r <= 0x3c; r += 4)
				LOG_ERROR("  [0x%02x]=0x%08x\n",
					  r, REG_READ(r));
			LOG_ERROR("  [0xf008]=0x%08x\n",
				  REG_READ(0xf008));
			LOG_ERROR("TIMEOUT DIAG: engine status/s_pointer:\n");
			LOG_ERROR("  CNA:  S_STATUS[0x1000]=0x%08x S_POINTER[0x1004]=0x%08x\n",
				  REG_READ(0x1000), REG_READ(0x1004));
			LOG_ERROR("  CORE: S_STATUS[0x3000]=0x%08x S_POINTER[0x3004]=0x%08x\n",
				  REG_READ(0x3000), REG_READ(0x3004));
			LOG_ERROR("  DPU:  S_STATUS[0x4000]=0x%08x S_POINTER[0x4004]=0x%08x\n",
				  REG_READ(0x4000), REG_READ(0x4004));
			LOG_ERROR("  RDMA: S_STATUS[0x5000]=0x%08x S_POINTER[0x5004]=0x%08x\n",
				  REG_READ(0x5000), REG_READ(0x5004));
			LOG_ERROR("  WDMA: S_STATUS[0x6000]=0x%08x S_POINTER[0x6004]=0x%08x\n",
				  REG_READ(0x6000), REG_READ(0x6004));
			LOG_ERROR("  WRDMA:S_STATUS[0x7000]=0x%08x S_POINTER[0x7004]=0x%08x\n",
				  REG_READ(0x7000), REG_READ(0x7004));
			LOG_ERROR("  CNA_CLK_GATE[0x1090]=0x%08x\n",
				  REG_READ(0x1090));
		}

		/* Dump task[0] and task[1] from memory at timeout */
		{
			struct rknpu_mem_object *task_obj =
				(struct rknpu_mem_object *)(uintptr_t)
				args->task_obj_addr;
			if (task_obj && task_obj->kv_addr) {
				struct rknpu_task *tb = task_obj->kv_addr;
				int t;
				for (t = 0; t < 3 && t < args->task_number; t++) {
					struct rknpu_task *tp = &tb[args->task_start + t];
					LOG_ERROR("  task[%d] flags=0x%x op=%u en=0x%x "
						  "imask=0x%x iclr=0x%x ist=0x%x "
						  "amt=%u off=%u cmd=0x%llx\n",
						  t, tp->flags, tp->op_idx,
						  tp->enable_mask, tp->int_mask,
						  tp->int_clear, tp->int_status,
						  tp->regcfg_amount,
						  tp->regcfg_offset,
						  tp->regcmd_addr);
				}
				/* Raw hex of task[0] and task[1] */
				LOG_ERROR("  task[0] raw: %*ph\n",
					  (int)sizeof(struct rknpu_task),
					  (u8 *)&tb[args->task_start]);
				if (args->task_number > 1)
					LOG_ERROR("  task[1] raw: %*ph\n",
						  (int)sizeof(struct rknpu_task),
						  (u8 *)&tb[args->task_start + 1]);
			}
		}

		return ret < 0 ? ret : -ETIMEDOUT;
	}

	if (!(job->flags & RKNPU_JOB_DONE))
		return -EINVAL;

	args->task_counter = args->task_number;
	args->hw_elapse_time = job->hw_elapse_time;

	return 0;
}

static inline int rknpu_job_subcore_commit_pc(struct rknpu_job *job,
					      int core_index)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	struct rknpu_submit *args = job->args;
	struct rknpu_mem_object *task_obj =
		(struct rknpu_mem_object *)(uintptr_t)args->task_obj_addr;
	struct rknpu_task *task_base = NULL;
	struct rknpu_task *first_task = NULL;
	struct rknpu_task *last_task = NULL;
	void __iomem *rknpu_core_base = rknpu_dev->base[core_index];
	int task_start = args->task_start;
	int task_end;
	int task_number = args->task_number;
	int task_pp_en = args->flags & RKNPU_JOB_PINGPONG ? 1 : 0;
	int pc_data_amount_scale = rknpu_dev->config->pc_data_amount_scale;
	int pc_task_number_bits = rknpu_dev->config->pc_task_number_bits;
	int i = 0;
	int submit_index = atomic_read(&job->submit_count[core_index]);
	int max_submit_number = rknpu_dev->config->max_submit_number;

	if (!task_obj) {
		job->ret = -EINVAL;
		return job->ret;
	}

	if (rknpu_dev->config->num_irqs > 1) {
		for (i = 0; i < rknpu_dev->config->num_irqs; i++) {
			if (i == core_index) {
				REG_WRITE((0xe + 0x10000000 * i), 0x1004);
				REG_WRITE((0xe + 0x10000000 * i), 0x3004);
			}
		}

		switch (job->use_core_num) {
		case 1:
		case 2:
			task_start = args->subcore_task[core_index].task_start;
			task_number =
				args->subcore_task[core_index].task_number;
			break;
		case 3:
			task_start =
				args->subcore_task[core_index + 2].task_start;
			task_number =
				args->subcore_task[core_index + 2].task_number;
			break;
		default:
			LOG_ERROR("Unknown use core num %d\n",
				  job->use_core_num);
			break;
		}
	}

	task_start = task_start + submit_index * max_submit_number;
	task_number = task_number - submit_index * max_submit_number;
	task_number = task_number > max_submit_number ? max_submit_number :
							task_number;
	task_end = task_start + task_number - 1;

	task_base = task_obj->kv_addr;

	first_task = &task_base[task_start];
	last_task = &task_base[task_end];

	LOG_INFO("commit_pc: core=%d task_start=%d task_number=%d task_end=%d\n",
		 core_index, task_start, task_number, task_end);
	LOG_INFO("commit_pc: task_obj=%p kv_addr=%p task_base_addr=0x%llx\n",
		 task_obj, task_base, args->task_base_addr);
	LOG_INFO("commit_pc: first_task: regcmd_addr=0x%llx amount=%u enable=0x%x int_mask=0x%x\n",
		 first_task->regcmd_addr, first_task->regcfg_amount,
		 first_task->enable_mask, first_task->int_mask);

	/* Dump first 5 task entries with full struct fields */
	{
		int t;
		for (t = task_start; t <= task_end && t < task_start + 5; t++) {
			struct rknpu_task *tp = &task_base[t];
			LOG_INFO("commit_pc: task[%d] flags=0x%x op_idx=%u enable=0x%x int_mask=0x%x int_clear=0x%x int_status=0x%x amount=%u offset=%u regcmd=0x%llx\n",
				 t, tp->flags, tp->op_idx, tp->enable_mask,
				 tp->int_mask, tp->int_clear, tp->int_status,
				 tp->regcfg_amount, tp->regcfg_offset,
				 tp->regcmd_addr);
		}
		/* Also dump raw bytes of task[0] and task[1] for cache verification */
		{
			u8 *raw = (u8 *)first_task;
			LOG_INFO("commit_pc: task[%d] raw: %*ph\n",
				 task_start, (int)sizeof(struct rknpu_task), raw);
			if (task_number > 1) {
				raw = (u8 *)&task_base[task_start + 1];
				LOG_INFO("commit_pc: task[%d] raw: %*ph\n",
					 task_start + 1,
					 (int)sizeof(struct rknpu_task), raw);
			}
		}
	}

	{
		u32 data_amount = (first_task->regcfg_amount +
			RKNPU_PC_DATA_EXTRA_AMOUNT +
			pc_data_amount_scale - 1) /
			pc_data_amount_scale - 1;
		u32 task_ctrl = ((0x6 | task_pp_en) <<
			pc_task_number_bits) | task_number;

		LOG_INFO("commit_pc: REGS: PC_DATA_ADDR=0x%llx PC_DATA_AMOUNT=%u\n",
			 first_task->regcmd_addr, data_amount);
		LOG_INFO("commit_pc: REGS: INT_MASK=0x%x INT_CLEAR=0x%x TASK_CTRL=0x%x DMA_BASE=0x%llx\n",
			 last_task->int_mask, first_task->int_mask,
			 task_ctrl, args->task_base_addr);
		LOG_INFO("commit_pc: base=%p\n", rknpu_core_base);

		REG_WRITE(first_task->regcmd_addr, RKNPU_OFFSET_PC_DATA_ADDR);
		REG_WRITE(data_amount, RKNPU_OFFSET_PC_DATA_AMOUNT);
		REG_WRITE(last_task->int_mask, RKNPU_OFFSET_INT_MASK);
		REG_WRITE(first_task->int_mask, RKNPU_OFFSET_INT_CLEAR);
		REG_WRITE(task_ctrl, RKNPU_OFFSET_PC_TASK_CONTROL);
		REG_WRITE(args->task_base_addr, RKNPU_OFFSET_PC_DMA_BASE_ADDR);
	}

	job->first_task = first_task;
	job->last_task = last_task;
	job->int_mask[core_index] = last_task->int_mask;

	/* Dump ALL NPU registers 0x00-0x3C before OP_EN to find faulting addresses */
	{
		int r;
		LOG_INFO("commit_pc: NPU register dump AFTER programming:\n");
		for (r = 0; r <= 0x3c; r += 4) {
			LOG_INFO("  [0x%02x]=0x%08x\n", r, REG_READ(r));
		}
		LOG_INFO("  [0xf008]=0x%08x\n", REG_READ(0xf008));
	}

	/* Clear ALL interrupts before starting */
	REG_WRITE(RKNPU_INT_CLEAR, RKNPU_OFFSET_INT_CLEAR);

	LOG_INFO("commit_pc: after clear raw_status=0x%x\n",
		 REG_READ(RKNPU_OFFSET_INT_RAW_STATUS));

	REG_WRITE(0x1, RKNPU_OFFSET_PC_OP_EN);
	REG_WRITE(0x0, RKNPU_OFFSET_PC_OP_EN);

	/* Read status immediately after enable */
	{
		u32 raw_s, int_s;
		udelay(100);
		raw_s = REG_READ(RKNPU_OFFSET_INT_RAW_STATUS);
		int_s = REG_READ(RKNPU_OFFSET_INT_STATUS);
		LOG_INFO("commit_pc: post-enable(100us) raw_status=0x%x int_status=0x%x\n",
			 raw_s, int_s);
	}

	return 0;
}

static inline int rknpu_job_subcore_commit(struct rknpu_job *job,
					   int core_index)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	struct rknpu_submit *args = job->args;
	void __iomem *rknpu_core_base = rknpu_dev->base[core_index];

	/* Switch to slave mode first */
	REG_WRITE(0x1, RKNPU_OFFSET_PC_DATA_ADDR);

	if (!(args->flags & RKNPU_JOB_PC)) {
		job->ret = -EINVAL;
		return job->ret;
	}

	return rknpu_job_subcore_commit_pc(job, core_index);
}

static void rknpu_job_commit(struct rknpu_job *job)
{
	switch (job->args->core_mask) {
	case RKNPU_CORE0_MASK:
		rknpu_job_subcore_commit(job, 0);
		break;
	case RKNPU_CORE1_MASK:
		rknpu_job_subcore_commit(job, 1);
		break;
	case RKNPU_CORE2_MASK:
		rknpu_job_subcore_commit(job, 2);
		break;
	case RKNPU_CORE0_MASK | RKNPU_CORE1_MASK:
		rknpu_job_subcore_commit(job, 0);
		rknpu_job_subcore_commit(job, 1);
		break;
	case RKNPU_CORE0_MASK | RKNPU_CORE1_MASK | RKNPU_CORE2_MASK:
		rknpu_job_subcore_commit(job, 0);
		rknpu_job_subcore_commit(job, 1);
		rknpu_job_subcore_commit(job, 2);
		break;
	default:
		LOG_ERROR("Unknown core mask: %d\n", job->args->core_mask);
		break;
	}
}

static void rknpu_job_next(struct rknpu_device *rknpu_dev, int core_index)
{
	struct rknpu_job *job = NULL;
	struct rknpu_subcore_data *subcore_data = NULL;
	unsigned long flags;

	if (rknpu_dev->soft_reseting)
		return;

	subcore_data = &rknpu_dev->subcore_datas[core_index];

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);

	if (subcore_data->job || list_empty(&subcore_data->todo_list)) {
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
		return;
	}

	job = list_first_entry(&subcore_data->todo_list, struct rknpu_job,
			       head[core_index]);

	list_del_init(&job->head[core_index]);
	subcore_data->job = job;
	job->hw_commit_time = ktime_get();
	job->hw_recoder_time = job->hw_commit_time;
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	if (atomic_dec_and_test(&job->run_count))
		rknpu_job_commit(job);
}

static void rknpu_job_done(struct rknpu_job *job, int ret, int core_index)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	struct rknpu_subcore_data *subcore_data = NULL;
	ktime_t now;
	unsigned long flags;
	int max_submit_number = rknpu_dev->config->max_submit_number;

	if (atomic_inc_return(&job->submit_count[core_index]) <
	    (rknpu_get_task_number(job, core_index) + max_submit_number - 1) /
		    max_submit_number) {
		rknpu_job_subcore_commit(job, core_index);
		return;
	}

	subcore_data = &rknpu_dev->subcore_datas[core_index];

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	subcore_data->job = NULL;
	subcore_data->task_num -= rknpu_get_task_number(job, core_index);
	now = ktime_get();
	job->hw_elapse_time = ktime_sub(now, job->hw_commit_time);
	subcore_data->timer.busy_time += ktime_sub(now, job->hw_recoder_time);
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	if (atomic_dec_and_test(&job->interrupt_count)) {
		int use_core_num = job->use_core_num;

		job->flags |= RKNPU_JOB_DONE;
		job->ret = ret;

		if (job->flags & RKNPU_JOB_ASYNC)
			schedule_work(&job->cleanup_work);

		if (use_core_num > 1)
			wake_up(&(&rknpu_dev->subcore_datas[0])->job_done_wq);
		else
			wake_up(&subcore_data->job_done_wq);
	}

	rknpu_job_next(rknpu_dev, core_index);
}

static int rknpu_schedule_core_index(struct rknpu_device *rknpu_dev)
{
	int core_num = rknpu_dev->config->num_irqs;
	int task_num = rknpu_dev->subcore_datas[0].task_num;
	int core_index = 0;
	int i = 0;

	for (i = 1; i < core_num; i++) {
		if (task_num > rknpu_dev->subcore_datas[i].task_num) {
			core_index = i;
			task_num = rknpu_dev->subcore_datas[i].task_num;
		}
	}

	return core_index;
}

static void rknpu_job_schedule(struct rknpu_job *job)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	struct rknpu_subcore_data *subcore_data = NULL;
	int i = 0, core_index = 0;
	unsigned long flags;

	if (job->args->core_mask == RKNPU_CORE_AUTO_MASK) {
		core_index = rknpu_schedule_core_index(rknpu_dev);
		job->args->core_mask = rknpu_core_mask(core_index);
		job->use_core_num = 1;
		atomic_set(&job->run_count, job->use_core_num);
		atomic_set(&job->interrupt_count, job->use_core_num);
	}

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	for (i = 0; i < rknpu_dev->config->num_irqs; i++) {
		if (job->args->core_mask & rknpu_core_mask(i)) {
			subcore_data = &rknpu_dev->subcore_datas[i];
			list_add_tail(&job->head[i], &subcore_data->todo_list);
			subcore_data->task_num += rknpu_get_task_number(job, i);
		}
	}
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	for (i = 0; i < rknpu_dev->config->num_irqs; i++) {
		if (job->args->core_mask & rknpu_core_mask(i))
			rknpu_job_next(rknpu_dev, i);
	}
}

static void rknpu_job_abort(struct rknpu_job *job)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	struct rknpu_subcore_data *subcore_data = NULL;
	unsigned long flags;
	int i = 0;

	msleep(100);

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	for (i = 0; i < rknpu_dev->config->num_irqs; i++) {
		if (job->args->core_mask & rknpu_core_mask(i)) {
			subcore_data = &rknpu_dev->subcore_datas[i];
			if (job == subcore_data->job && !job->irq_entry[i]) {
				subcore_data->job = NULL;
				subcore_data->task_num -=
					rknpu_get_task_number(job, i);
			}
		}
	}
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	if (job->ret == -ETIMEDOUT) {
		LOG_ERROR("job timeout, flags: %#x:\n", job->flags);
		for (i = 0; i < rknpu_dev->config->num_irqs; i++) {
			if (job->args->core_mask & rknpu_core_mask(i)) {
				void __iomem *rknpu_core_base =
					rknpu_dev->base[i];
				LOG_ERROR(
					"\tcore %d: int=0x%x raw=0x%x mask=0x%x tc=%u pc_addr=0x%x pc_amt=0x%x task_ctrl=0x%x dma_base=0x%x elapsed=%lldus\n",
					i, REG_READ(RKNPU_OFFSET_INT_STATUS),
					REG_READ(RKNPU_OFFSET_INT_RAW_STATUS),
					job->int_mask[i],
					(REG_READ(
						 rknpu_dev->config
							 ->pc_task_status_offset) &
					 rknpu_dev->config->pc_task_number_mask),
					REG_READ(RKNPU_OFFSET_PC_DATA_ADDR),
					REG_READ(RKNPU_OFFSET_PC_DATA_AMOUNT),
					REG_READ(RKNPU_OFFSET_PC_TASK_CONTROL),
					REG_READ(RKNPU_OFFSET_PC_DMA_BASE_ADDR),
					ktime_us_delta(ktime_get(),
						       job->timestamp));
			}
		}
		/* Post-timeout IOMMU diagnostic: core 0 only */
		{
			void __iomem *mmu0 = rknpu_dev->base[0] + 0x9000;
			void __iomem *mmu1 = rknpu_dev->base[0] + 0xa000;
			u32 dte0, sts0, pf0, raw0, msk0;
			u32 dte1, sts1, pf1, raw1, msk1;

			sts0 = readl(mmu0 + 0x04);
			pf0 = readl(mmu0 + 0x0c);
			raw0 = readl(mmu0 + 0x14);
			msk0 = readl(mmu0 + 0x1c);
			dte0 = readl(mmu0 + 0x00);
			LOG_ERROR("\tIOMMU[0x9000]: DTE=0x%x STATUS=0x%x PG_FAULT=0x%x RAW=0x%x MASK=0x%x\n",
				  dte0, sts0, pf0, raw0, msk0);

			sts1 = readl(mmu1 + 0x04);
			pf1 = readl(mmu1 + 0x0c);
			raw1 = readl(mmu1 + 0x14);
			msk1 = readl(mmu1 + 0x1c);
			dte1 = readl(mmu1 + 0x00);
			LOG_ERROR("\tIOMMU[0xa000]: DTE=0x%x STATUS=0x%x PG_FAULT=0x%x RAW=0x%x MASK=0x%x\n",
				  dte1, sts1, pf1, raw1, msk1);
		}
		rknpu_soft_reset(rknpu_dev);
	} else {
		LOG_ERROR(
			"job abort, flags: %#x, ret: %d, elapsed: %lldus\n",
			job->flags, job->ret,
			ktime_us_delta(ktime_get(), job->timestamp));
	}

	rknpu_job_cleanup(job);
}

static inline uint32_t rknpu_fuzz_status(uint32_t status)
{
	uint32_t fuzz_status = 0;

	if ((status & 0x3) != 0)
		fuzz_status |= 0x3;
	if ((status & 0xc) != 0)
		fuzz_status |= 0xc;
	if ((status & 0x30) != 0)
		fuzz_status |= 0x30;
	if ((status & 0xc0) != 0)
		fuzz_status |= 0xc0;
	if ((status & 0x300) != 0)
		fuzz_status |= 0x300;
	if ((status & 0xc00) != 0)
		fuzz_status |= 0xc00;

	return fuzz_status;
}

static inline irqreturn_t rknpu_irq_handler(int irq, void *data, int core_index)
{
	struct rknpu_device *rknpu_dev = data;
	void __iomem *rknpu_core_base = rknpu_dev->base[core_index];
	struct rknpu_subcore_data *subcore_data = NULL;
	struct rknpu_job *job = NULL;
	uint32_t status = 0;
	uint32_t raw_status = 0;
	uint32_t task_cnt = 0;
	unsigned long flags;

	subcore_data = &rknpu_dev->subcore_datas[core_index];

	raw_status = REG_READ(RKNPU_OFFSET_INT_RAW_STATUS);
	status = REG_READ(RKNPU_OFFSET_INT_STATUS);
	task_cnt = REG_READ(rknpu_dev->config->pc_task_status_offset) &
		   rknpu_dev->config->pc_task_number_mask;

	LOG_INFO("irq: core=%d raw=0x%x status=0x%x task_cnt=%u\n",
		 core_index, raw_status, status, task_cnt);

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	job = subcore_data->job;
	if (!job) {
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
		REG_WRITE(RKNPU_INT_CLEAR, RKNPU_OFFSET_INT_CLEAR);
		LOG_INFO("irq: core=%d no job, cleared\n", core_index);
		rknpu_job_next(rknpu_dev, core_index);
		return IRQ_HANDLED;
	}
	job->irq_entry[core_index] = true;
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	job->int_status[core_index] = status;

	if (rknpu_fuzz_status(status) != job->int_mask[core_index]) {
		LOG_ERROR(
			"invalid irq status: %#x, raw status: %#x, require mask: %#x, fuzz: %#x, task counter: %#x\n",
			status, raw_status,
			job->int_mask[core_index],
			rknpu_fuzz_status(status), task_cnt);
		REG_WRITE(RKNPU_INT_CLEAR, RKNPU_OFFSET_INT_CLEAR);
		return IRQ_HANDLED;
	}

	LOG_INFO("irq: core=%d matched, calling job_done (submit_count=%d)\n",
		 core_index, atomic_read(&job->submit_count[core_index]));

	REG_WRITE(RKNPU_INT_CLEAR, RKNPU_OFFSET_INT_CLEAR);
	rknpu_job_done(job, 0, core_index);

	return IRQ_HANDLED;
}

irqreturn_t rknpu_core0_irq_handler(int irq, void *data)
{
	return rknpu_irq_handler(irq, data, 0);
}

irqreturn_t rknpu_core1_irq_handler(int irq, void *data)
{
	return rknpu_irq_handler(irq, data, 1);
}

irqreturn_t rknpu_core2_irq_handler(int irq, void *data)
{
	return rknpu_irq_handler(irq, data, 2);
}

static int rknpu_submit(struct rknpu_device *rknpu_dev,
			struct rknpu_submit *args)
{
	struct rknpu_job *job = NULL;
	int ret = -EINVAL;

	if (args->task_number == 0) {
		LOG_ERROR("invalid rknpu task number!\n");
		return -EINVAL;
	}

	if (args->core_mask > rknpu_dev->config->core_mask) {
		LOG_ERROR("invalid rknpu core mask: %#x", args->core_mask);
		return -EINVAL;
	}

	job = rknpu_job_alloc(rknpu_dev, args);
	if (!job) {
		LOG_ERROR("failed to allocate rknpu job!\n");
		return -ENOMEM;
	}

	if (args->flags & RKNPU_JOB_NONBLOCK) {
		job->flags |= RKNPU_JOB_ASYNC;
		rknpu_job_schedule(job);
		ret = job->ret;
		if (ret) {
			rknpu_job_abort(job);
			return ret;
		}
	} else {
		rknpu_job_schedule(job);
		if (args->flags & RKNPU_JOB_PC)
			job->ret = rknpu_job_wait(job);

		args->task_counter = job->args->task_counter;
		ret = job->ret;
		if (!ret)
			rknpu_job_cleanup(job);
		else
			rknpu_job_abort(job);
	}

	return ret;
}

/*
 * IOVA guard pages for NPU pre-fetch protection.
 *
 * The NPU hardware pre-fetches/reads memory beyond buffer boundaries.
 * On mainline kernels, the IOMMU IOVA allocator assigns virtual addresses
 * top-down, leaving unmapped gaps between allocations. When the NPU
 * accesses these gaps, it triggers IOMMU page faults.
 *
 * Fix: find ALL gaps between session BOs and map guard pages to fill them.
 * Each guard page maps to a single zeroed physical page (reads are harmless).
 *
 * Uses kmalloc for the IOVA tracking array to avoid stack overflow.
 */
#define RKNPU_MAX_GUARD_PAGES	2048  /* max 8MB of guard pages total */
#define RKNPU_GUARD_BELOW	16    /* 64KB guard below lowest BO */

int rknpu_submit_ioctl(struct rknpu_device *rknpu_dev, struct file *file,
		       unsigned int cmd, unsigned long data)
{
	struct rknpu_submit args;
	struct rknpu_mem_object *task_obj;
	struct rknpu_session *session;
	struct iommu_domain *domain = NULL;
	unsigned int in_size = _IOC_SIZE(cmd);
	unsigned int k_size = sizeof(struct rknpu_submit);
	struct page *guard_page = NULL;
	dma_addr_t *guard_iovas = NULL;
	int guard_count = 0;
	int ret = -EINVAL;

	if (in_size > k_size)
		in_size = k_size;

	memset(&args, 0, sizeof(args));

	if (unlikely(copy_from_user(&args, (struct rknpu_submit __user *)data,
				    in_size))) {
		LOG_ERROR("%s: copy_from_user failed\n", __func__);
		return -EFAULT;
	}

	LOG_INFO("submit: flags=0x%x tasks=%u task_base=0x%llx core_mask=0x%x\n",
		 args.flags, args.task_number,
		 args.task_base_addr, args.core_mask);

	/*
	 * If SDK didn't provide task_base_addr (e.g. smaller ioctl struct
	 * or SDK version that leaves it zero), use task_obj->dma_addr.
	 */
	if (args.task_base_addr == 0 && args.task_obj_addr != 0) {
		task_obj = (struct rknpu_mem_object *)(uintptr_t)args.task_obj_addr;
		if (task_obj) {
			args.task_base_addr = task_obj->dma_addr;
			LOG_INFO("submit: task_base_addr fallback to 0x%llx\n",
				 args.task_base_addr);
		}
	}

	/*
	 * Fill IOVA gaps between session BOs with guard pages.
	 * Sort BOs by IOVA, find gaps, fill each gap page-by-page.
	 */
	session = file->private_data;
	if (session && rknpu_dev->iommu_en) {
		struct rknpu_mem_object *bo;
		struct { dma_addr_t start; dma_addr_t end; } ranges[32];
		int n_ranges = 0;
		int i, j;

		spin_lock(&rknpu_dev->lock);
		list_for_each_entry(bo, &session->list, head) {
			if (n_ranges < 32) {
				ranges[n_ranges].start = bo->dma_addr;
				ranges[n_ranges].end = bo->dma_addr + bo->size;
				n_ranges++;
			}
		}
		spin_unlock(&rknpu_dev->lock);

		/* Sort by start address (insertion sort, n is small) */
		for (i = 1; i < n_ranges; i++) {
			dma_addr_t ts = ranges[i].start;
			dma_addr_t te = ranges[i].end;
			j = i - 1;
			while (j >= 0 && ranges[j].start > ts) {
				ranges[j + 1] = ranges[j];
				j--;
			}
			ranges[j + 1].start = ts;
			ranges[j + 1].end = te;
		}

		domain = iommu_get_domain_for_dev(rknpu_dev->dev);
		if (domain && n_ranges > 0) {
			guard_iovas = kmalloc_array(RKNPU_MAX_GUARD_PAGES,
						    sizeof(dma_addr_t),
						    GFP_KERNEL);
			guard_page = alloc_page(GFP_KERNEL | __GFP_ZERO);

			if (guard_page && guard_iovas) {
				phys_addr_t pa = page_to_phys(guard_page);
				int total_gaps = 0;
				dma_addr_t iova;

				/* Map guard pages BELOW lowest BO */
				if (ranges[0].start >= RKNPU_GUARD_BELOW * PAGE_SIZE) {
					dma_addr_t gs = ranges[0].start -
							RKNPU_GUARD_BELOW * PAGE_SIZE;
					for (iova = gs;
					     iova < ranges[0].start &&
					     guard_count < RKNPU_MAX_GUARD_PAGES;
					     iova += PAGE_SIZE) {
						if (iommu_map(domain, iova, pa,
							      PAGE_SIZE,
							      IOMMU_READ) == 0)
							guard_iovas[guard_count++] = iova;
					}
					LOG_INFO("submit: guard below: 0x%llx-0x%llx (%d pages)\n",
						 (u64)gs, (u64)ranges[0].start,
						 guard_count);
				}

				/* Fill gaps BETWEEN consecutive BOs */
				for (i = 0; i < n_ranges - 1; i++) {
					dma_addr_t gap_s = PAGE_ALIGN(ranges[i].end);
					dma_addr_t gap_e = ranges[i + 1].start & PAGE_MASK;
					int gap_pages, mapped = 0;

					if (gap_s >= gap_e)
						continue;

					gap_pages = (gap_e - gap_s) >> PAGE_SHIFT;
					total_gaps++;

					for (iova = gap_s;
					     iova < gap_e &&
					     guard_count < RKNPU_MAX_GUARD_PAGES;
					     iova += PAGE_SIZE) {
						if (iommu_map(domain, iova, pa,
							      PAGE_SIZE,
							      IOMMU_READ) == 0) {
							guard_iovas[guard_count++] = iova;
							mapped++;
						}
					}

					LOG_INFO("submit: gap[%d] 0x%llx-0x%llx (%d/%d pages)\n",
						 i, (u64)gap_s, (u64)gap_e,
						 mapped, gap_pages);
				}

				LOG_INFO("submit: total guard pages=%d across %d gaps\n",
					 guard_count, total_gaps);
			}
		}
	}

	/*
	 * Flush CPU caches for ALL imported DMA-BUF BOs before NPU access.
	 *
	 * The SDK writes task descriptors, regcmds, and input data to
	 * DMA-BUF mapped memory via CPU. On BSP 5.10, the driver's
	 * MEM_SYNC ioctl handled cache maintenance. On mainline 6.18
	 * with system heap, the SDK may not call MEM_SYNC or
	 * DMA_BUF_IOCTL_SYNC. Force-flushing here ensures all
	 * CPU-written data is visible to the NPU's DMA engine.
	 */
	if (session) {
		struct rknpu_mem_object *bo;
		struct sg_table *sync_sgt[32];
		int sync_count = 0;
		int i;

		/* Collect sgt pointers under lock, sync outside */
		spin_lock(&rknpu_dev->lock);
		list_for_each_entry(bo, &session->list, head) {
			if (!bo->owner && bo->sgt && sync_count < 32)
				sync_sgt[sync_count++] = bo->sgt;
		}
		spin_unlock(&rknpu_dev->lock);

		for (i = 0; i < sync_count; i++)
			dma_sync_sgtable_for_device(rknpu_dev->dev,
						    sync_sgt[i],
						    DMA_TO_DEVICE);
		LOG_INFO("submit: synced %d DMA-BUF BOs to device\n",
			 sync_count);
	}

	/*
	 * Dump first regcmds of task[0] to verify IOVA addresses.
	 * Find the BO containing regcmd_addr and dump from kv_addr.
	 */
	if (session && args.task_obj_addr) {
		struct rknpu_mem_object *task_obj =
			(struct rknpu_mem_object *)(uintptr_t)args.task_obj_addr;
		if (task_obj && task_obj->kv_addr) {
			struct rknpu_task *tb = task_obj->kv_addr;
			u64 regcmd_iova = tb[args.task_start].regcmd_addr;
			struct rknpu_mem_object *bo;

			LOG_INFO("submit: task[0] regcmd_iova=0x%llx\n",
				 regcmd_iova);

			spin_lock(&rknpu_dev->lock);
			list_for_each_entry(bo, &session->list, head) {
				dma_addr_t bo_end = bo->dma_addr + bo->size;
				if (regcmd_iova >= bo->dma_addr &&
				    regcmd_iova < bo_end && bo->kv_addr) {
					u64 off = regcmd_iova - bo->dma_addr;
					u32 *rcmd = (u32 *)((u8 *)bo->kv_addr + off);
					int words = min_t(int, 280,
						(bo->size - off) / 4);
					int w;

					spin_unlock(&rknpu_dev->lock);
					LOG_INFO("submit: regcmd in BO dma=0x%llx"
						 " off=0x%llx entries=%d:\n",
						 (u64)bo->dma_addr, off,
						 words / 2);
					for (w = 0; w + 1 < words; w += 2) {
						u32 w0 = rcmd[w];
						u32 w1 = rcmd[w + 1];
						u32 reg = w0 & 0xffff;
						u32 val = ((w1 & 0xffff) << 16)
							  | (w0 >> 16);
						u32 tgt = w1 >> 16;
						if (val >= 0xff000000)
							LOG_INFO("  [%03d] reg=0x%04x tgt=0x%04x val=0x%08x **IOVA**\n",
								 w / 2, reg, tgt, val);
						else if (w / 2 < 20 ||
							 reg == 0x1070 ||
							 reg == 0x4020 ||
							 reg == 0x6020 ||
							 reg == 0x4004 ||
							 reg == 0x5004 ||
							 reg == 0x6004 ||
							 reg == 0x7004)
							LOG_INFO("  [%03d] reg=0x%04x tgt=0x%04x val=0x%08x\n",
								 w / 2, reg, tgt, val);
					}
					goto regcmd_done;
				}
			}
			spin_unlock(&rknpu_dev->lock);
		}
	}
regcmd_done:

	ret = rknpu_submit(rknpu_dev, &args);

	/*
	 * Sync DMA-BUF BOs from device after NPU completes.
	 * This ensures CPU can read NPU output data from DMA-BUFs.
	 */
	if (session) {
		struct rknpu_mem_object *bo;
		struct sg_table *sync_sgt[32];
		int sync_count = 0;
		int i;

		spin_lock(&rknpu_dev->lock);
		list_for_each_entry(bo, &session->list, head) {
			if (!bo->owner && bo->sgt && sync_count < 32)
				sync_sgt[sync_count++] = bo->sgt;
		}
		spin_unlock(&rknpu_dev->lock);

		for (i = 0; i < sync_count; i++)
			dma_sync_sgtable_for_cpu(rknpu_dev->dev,
						 sync_sgt[i],
						 DMA_FROM_DEVICE);
	}

	/* Unmap all guard pages after NPU job completes */
	if (guard_count && domain) {
		int i;
		for (i = 0; i < guard_count; i++)
			iommu_unmap(domain, guard_iovas[i], PAGE_SIZE);
		LOG_INFO("submit: guard unmapped %d pages\n", guard_count);
	}
	if (guard_page)
		__free_page(guard_page);
	kfree(guard_iovas);

	if (unlikely(copy_to_user((struct rknpu_submit __user *)data, &args,
				  in_size))) {
		LOG_ERROR("%s: copy_to_user failed\n", __func__);
		return -EFAULT;
	}

	return ret;
}

int rknpu_get_hw_version(struct rknpu_device *rknpu_dev, uint32_t *version)
{
	void __iomem *rknpu_core_base = rknpu_dev->base[0];

	if (version == NULL)
		return -EINVAL;

	*version = REG_READ(RKNPU_OFFSET_VERSION) +
		   (REG_READ(RKNPU_OFFSET_VERSION_NUM) & 0xffff);

	return 0;
}

int rknpu_clear_rw_amount(struct rknpu_device *rknpu_dev)
{
	/* Not supported on RK3588 (amount_top is NULL) */
	return 0;
}

int rknpu_get_rw_amount(struct rknpu_device *rknpu_dev, uint32_t *dt_wr,
			uint32_t *dt_rd, uint32_t *wd_rd)
{
	/* Not supported on RK3588 */
	return 0;
}

int rknpu_get_total_rw_amount(struct rknpu_device *rknpu_dev, uint32_t *amount)
{
	/* Not supported on RK3588 */
	if (amount)
		*amount = 0;
	return 0;
}
