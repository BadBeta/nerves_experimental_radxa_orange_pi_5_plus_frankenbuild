/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) Rockchip Electronics Co.Ltd
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 *
 * Adapted for mainline Linux 6.18 by removing vendor-specific dependencies.
 */

#ifndef __LINUX_RKNPU_IOCTL_H
#define __LINUX_RKNPU_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#if !defined(__KERNEL__)
#define __user
#endif

#ifndef __packed
#define __packed __attribute__((packed))
#endif

/* PC register offsets */
#define RKNPU_OFFSET_VERSION 0x0
#define RKNPU_OFFSET_VERSION_NUM 0x4
#define RKNPU_OFFSET_PC_OP_EN 0x8
#define RKNPU_OFFSET_PC_DATA_ADDR 0x10
#define RKNPU_OFFSET_PC_DATA_AMOUNT 0x14
#define RKNPU_OFFSET_PC_TASK_CONTROL 0x30
#define RKNPU_OFFSET_PC_DMA_BASE_ADDR 0x34

/* Interrupt register offsets */
#define RKNPU_OFFSET_INT_MASK 0x20
#define RKNPU_OFFSET_INT_CLEAR 0x24
#define RKNPU_OFFSET_INT_STATUS 0x28
#define RKNPU_OFFSET_INT_RAW_STATUS 0x2c

#define RKNPU_OFFSET_ENABLE_MASK 0xf008

#define RKNPU_INT_CLEAR 0x1ffff

#define RKNPU_PC_DATA_EXTRA_AMOUNT 4

/* memory type definitions. */
enum e_rknpu_mem_type {
	RKNPU_MEM_CONTIGUOUS = 0 << 0,
	RKNPU_MEM_NON_CONTIGUOUS = 1 << 0,
	RKNPU_MEM_NON_CACHEABLE = 0 << 1,
	RKNPU_MEM_CACHEABLE = 1 << 1,
	RKNPU_MEM_WRITE_COMBINE = 1 << 2,
	RKNPU_MEM_KERNEL_MAPPING = 1 << 3,
	RKNPU_MEM_IOMMU = 1 << 4,
	RKNPU_MEM_ZEROING = 1 << 5,
	RKNPU_MEM_SECURE = 1 << 6,
	RKNPU_MEM_DMA32 = 1 << 7,
	RKNPU_MEM_TRY_ALLOC_SRAM = 1 << 8,
	RKNPU_MEM_TRY_ALLOC_NBUF = 1 << 9,
	RKNPU_MEM_IOMMU_LIMIT_IOVA_ALIGNMENT = 1 << 10,
};

/* sync mode definitions. */
enum e_rknpu_mem_sync_mode {
	RKNPU_MEM_SYNC_TO_DEVICE = 1 << 0,
	RKNPU_MEM_SYNC_FROM_DEVICE = 1 << 1,
};

/* job mode definitions. */
enum e_rknpu_job_mode {
	RKNPU_JOB_SLAVE = 0 << 0,
	RKNPU_JOB_PC = 1 << 0,
	RKNPU_JOB_BLOCK = 0 << 1,
	RKNPU_JOB_NONBLOCK = 1 << 1,
	RKNPU_JOB_PINGPONG = 1 << 2,
	RKNPU_JOB_FENCE_IN = 1 << 3,
	RKNPU_JOB_FENCE_OUT = 1 << 4,
};

/* action definitions */
enum e_rknpu_action {
	RKNPU_GET_HW_VERSION = 0,
	RKNPU_GET_DRV_VERSION = 1,
	RKNPU_GET_FREQ = 2,
	RKNPU_SET_FREQ = 3,
	RKNPU_GET_VOLT = 4,
	RKNPU_SET_VOLT = 5,
	RKNPU_ACT_RESET = 6,
	RKNPU_GET_BW_PRIORITY = 7,
	RKNPU_SET_BW_PRIORITY = 8,
	RKNPU_GET_BW_EXPECT = 9,
	RKNPU_SET_BW_EXPECT = 10,
	RKNPU_GET_BW_TW = 11,
	RKNPU_SET_BW_TW = 12,
	RKNPU_ACT_CLR_TOTAL_RW_AMOUNT = 13,
	RKNPU_GET_DT_WR_AMOUNT = 14,
	RKNPU_GET_DT_RD_AMOUNT = 15,
	RKNPU_GET_WT_RD_AMOUNT = 16,
	RKNPU_GET_TOTAL_RW_AMOUNT = 17,
	RKNPU_GET_IOMMU_EN = 18,
	RKNPU_SET_PROC_NICE = 19,
	RKNPU_POWER_ON = 20,
	RKNPU_POWER_OFF = 21,
	RKNPU_GET_TOTAL_SRAM_SIZE = 22,
	RKNPU_GET_FREE_SRAM_SIZE = 23,
	RKNPU_GET_IOMMU_DOMAIN_ID = 24,
	RKNPU_SET_IOMMU_DOMAIN_ID = 25,
};

/**
 * struct rknpu_mem_create - buffer creation information
 */
struct rknpu_mem_create {
	__u32 handle;
	__u32 flags;
	__u64 size;
	__u64 obj_addr;
	__u64 dma_addr;
	__u64 sram_size;
	__s32 iommu_domain_id;
	__u32 core_mask;
};

/**
 * struct rknpu_mem_map - mmap offset query
 */
struct rknpu_mem_map {
	__u32 handle;
	__u32 reserved;
	__u64 offset;
};

/**
 * struct rknpu_mem_destroy - buffer destruction
 */
struct rknpu_mem_destroy {
	__u32 handle;
	__u32 reserved;
	__u64 obj_addr;
};

/**
 * struct rknpu_mem_sync - buffer synchronization
 */
struct rknpu_mem_sync {
	__u32 flags;
	__u32 reserved;
	__u64 obj_addr;
	__u64 offset;
	__u64 size;
};

/**
 * struct rknpu_task - task information for register commands
 */
struct rknpu_task {
	__u32 flags;
	__u32 op_idx;
	__u32 enable_mask;
	__u32 int_mask;
	__u32 int_clear;
	__u32 int_status;
	__u32 regcfg_amount;
	__u32 regcfg_offset;
	__u64 regcmd_addr;
} __packed;

/**
 * struct rknpu_subcore_task - per-core task index
 */
struct rknpu_subcore_task {
	__u32 task_start;
	__u32 task_number;
};

/**
 * struct rknpu_submit - job submission
 */
struct rknpu_submit {
	__u32 flags;
	__u32 timeout;
	__u32 task_start;
	__u32 task_number;
	__u32 task_counter;
	__s32 priority;
	__u64 task_obj_addr;
	__u32 iommu_domain_id;
	__u32 reserved;
	__u64 task_base_addr;
	__s64 hw_elapse_time;
	__u32 core_mask;
	__s32 fence_fd;
	struct rknpu_subcore_task subcore_task[5];
};

/**
 * struct rknpu_action - action (GET, SET or ACT)
 */
struct rknpu_action {
	__u32 flags;
	__u32 value;
};

/* Ioctl command numbers (misc device, magic 'r') */
#define RKNPU_ACTION 0x00
#define RKNPU_SUBMIT 0x01
#define RKNPU_MEM_CREATE 0x02
#define RKNPU_MEM_MAP 0x03
#define RKNPU_MEM_DESTROY 0x04
#define RKNPU_MEM_SYNC 0x05

#define RKNPU_IOC_MAGIC 'r'
#define RKNPU_IOW(nr, type) _IOW(RKNPU_IOC_MAGIC, nr, type)
#define RKNPU_IOR(nr, type) _IOR(RKNPU_IOC_MAGIC, nr, type)
#define RKNPU_IOWR(nr, type) _IOWR(RKNPU_IOC_MAGIC, nr, type)

#define IOCTL_RKNPU_ACTION RKNPU_IOWR(RKNPU_ACTION, struct rknpu_action)
#define IOCTL_RKNPU_SUBMIT RKNPU_IOWR(RKNPU_SUBMIT, struct rknpu_submit)
#define IOCTL_RKNPU_MEM_CREATE \
	RKNPU_IOWR(RKNPU_MEM_CREATE, struct rknpu_mem_create)
#define IOCTL_RKNPU_MEM_MAP RKNPU_IOWR(RKNPU_MEM_MAP, struct rknpu_mem_map)
#define IOCTL_RKNPU_MEM_DESTROY \
	RKNPU_IOWR(RKNPU_MEM_DESTROY, struct rknpu_mem_destroy)
#define IOCTL_RKNPU_MEM_SYNC RKNPU_IOWR(RKNPU_MEM_SYNC, struct rknpu_mem_sync)

#endif
