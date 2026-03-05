/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) Rockchip Electronics Co.Ltd
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 *
 * Memory allocator supporting two paths:
 * - DMA-BUF import (handle > 0): SDK allocates from /dev/dma_heap, passes fd
 * - dma_alloc_coherent (handle = 0): NIF uses kernel-allocated coherent memory
 */

#ifndef __LINUX_RKNPU_MEM_H
#define __LINUX_RKNPU_MEM_H

#include <linux/mm_types.h>
#include <linux/dma-buf.h>

struct rknpu_device;

/*
 * rknpu DMA buffer structure.
 *
 * @size: allocated size (page-aligned).
 * @kv_addr: kernel virtual address.
 * @dma_addr: IOVA / bus address for NPU access.
 * @head: list entry for session tracking.
 * @dmabuf: DMA-BUF reference (import path only).
 * @attachment: DMA-BUF attachment (import path only).
 * @sgt: scatter-gather table (import path only).
 * @owner: 1 = driver allocated (dma_alloc_coherent), 0 = imported DMA-BUF.
 */
struct rknpu_mem_object {
	unsigned long size;
	void *kv_addr;
	dma_addr_t dma_addr;
	struct list_head head;
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attachment;
	struct sg_table *sgt;
	int owner;
};

int rknpu_mem_create_ioctl(struct rknpu_device *rknpu_dev, struct file *file,
			   unsigned int cmd, unsigned long data);
int rknpu_mem_destroy_ioctl(struct rknpu_device *rknpu_dev, struct file *file,
			    unsigned long data);
int rknpu_mem_sync_ioctl(struct rknpu_device *rknpu_dev, unsigned long data);

#endif
