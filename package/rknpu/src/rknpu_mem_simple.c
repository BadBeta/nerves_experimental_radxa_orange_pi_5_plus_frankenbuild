// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Rockchip Electronics Co.Ltd
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 *
 * Memory allocator for mainline Linux, supporting two allocation paths:
 *
 * Path A (handle > 0): DMA-BUF import
 *   The RKNN SDK allocates from /dev/dma_heap/system, gets a DMA-BUF fd,
 *   then passes it via MEM_CREATE. We import the DMA-BUF, map through
 *   IOMMU to get an IOVA, and keep the mapping alive.
 *
 * Path B (handle <= 0): dma_alloc_coherent
 *   Our Rust NIF uses kernel-allocated coherent memory. The handle returned
 *   is a simple counter (not an fd). Userspace mmaps via /dev/rknpu.
 */

#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/dma-buf.h>
#include <linux/iosys-map.h>
#include <linux/uaccess.h>

#include "rknpu_drv.h"
#include "rknpu_ioctl.h"
#include "rknpu_mem.h"

static atomic_t handle_counter = ATOMIC_INIT(0);

int rknpu_mem_create_ioctl(struct rknpu_device *rknpu_dev, struct file *file,
			   unsigned int cmd, unsigned long data)
{
	struct rknpu_mem_create args;
	struct rknpu_mem_object *rknpu_obj = NULL;
	struct rknpu_session *session = NULL;
	unsigned int in_size = _IOC_SIZE(cmd);
	unsigned int k_size = sizeof(struct rknpu_mem_create);
	int ret;

	if (in_size > k_size)
		in_size = k_size;

	memset(&args, 0, sizeof(args));

	if (unlikely(copy_from_user(&args, (struct rknpu_mem_create __user *)data,
				    in_size))) {
		LOG_ERROR("%s: copy_from_user failed\n", __func__);
		return -EFAULT;
	}

	rknpu_obj = kzalloc(sizeof(*rknpu_obj), GFP_KERNEL);
	if (!rknpu_obj)
		return -ENOMEM;

	if (args.handle > 0) {
		/*
		 * Path A: Import DMA-BUF fd from userspace (SDK path).
		 *
		 * The SDK has already allocated from /dev/dma_heap/system
		 * and passes the DMA-BUF fd in the handle field.
		 */
		int fd = args.handle;
		struct dma_buf *dmabuf;
		struct dma_buf_attachment *attachment;
		struct sg_table *sgt;
		struct scatterlist *sgl;

		dmabuf = dma_buf_get(fd);
		if (IS_ERR(dmabuf)) {
			LOG_ERROR("mem_create: dma_buf_get(fd=%d) failed: %ld\n",
				  fd, PTR_ERR(dmabuf));
			ret = PTR_ERR(dmabuf);
			goto err_free_obj;
		}

		attachment = dma_buf_attach(dmabuf, rknpu_dev->dev);
		if (IS_ERR(attachment)) {
			LOG_ERROR("mem_create: dma_buf_attach failed: %ld\n",
				  PTR_ERR(attachment));
			ret = PTR_ERR(attachment);
			goto err_put_dmabuf;
		}

		sgt = dma_buf_map_attachment(attachment, DMA_BIDIRECTIONAL);
		if (IS_ERR(sgt)) {
			LOG_ERROR("mem_create: dma_buf_map_attachment failed: %ld\n",
				  PTR_ERR(sgt));
			ret = PTR_ERR(sgt);
			goto err_detach;
		}

		sgl = sgt->sgl;
		rknpu_obj->dmabuf = dmabuf;
		rknpu_obj->attachment = attachment;
		rknpu_obj->sgt = sgt;
		rknpu_obj->dma_addr = sg_dma_address(sgl);
		rknpu_obj->size = PAGE_ALIGN(args.size);
		rknpu_obj->owner = 0; /* imported, not owned */

		/*
		 * Kernel virtual mapping needed so SUBMIT handler can read
		 * task descriptors from the BO.
		 */
		{
			struct iosys_map map;
			ret = dma_buf_vmap(dmabuf, &map);
			if (ret) {
				LOG_ERROR("mem_create: dma_buf_vmap failed: %d\n", ret);
				goto err_unmap_attachment;
			}
			rknpu_obj->kv_addr = map.vaddr;
		}

		args.handle = fd; /* return same fd */
		args.size = rknpu_obj->size;
		args.obj_addr = (__u64)(uintptr_t)rknpu_obj;
		args.dma_addr = (__u64)rknpu_obj->dma_addr;
		args.sram_size = 0;

		{
			struct scatterlist *sg_iter;
			int sg_idx;
			dma_addr_t total_dma_len = 0;

			for_each_sgtable_dma_sg(sgt, sg_iter, sg_idx) {
				dma_addr_t a = sg_dma_address(sg_iter);
				unsigned int l = sg_dma_len(sg_iter);
				total_dma_len += l;
				LOG_INFO("mem_create: IMPORT fd=%d sg[%d] dma=%#llx len=%u\n",
					 fd, sg_idx, (u64)a, l);
			}
			LOG_INFO("mem_create: IMPORT fd=%d total_dma_len=%llu requested=%llu dma_base=%#llx nents=%d orig_nents=%d\n",
				 fd, (u64)total_dma_len, args.size,
				 args.dma_addr, sgt->nents,
				 sgt->orig_nents);
		}
	} else {
		/*
		 * Path B: Kernel allocates via dma_alloc_coherent (NIF path).
		 *
		 * Returns a counter as handle (not an fd). Userspace mmaps
		 * via /dev/rknpu using the MEM_MAP ioctl to get the offset.
		 */
		void *kv_addr;
		dma_addr_t dma_addr;
		size_t aligned_size = PAGE_ALIGN(args.size);

		kv_addr = dma_alloc_coherent(rknpu_dev->dev, aligned_size,
					     &dma_addr,
					     GFP_KERNEL | __GFP_ZERO);
		if (!kv_addr) {
			LOG_ERROR("mem_create: dma_alloc_coherent failed for size %zu\n",
				  aligned_size);
			ret = -ENOMEM;
			goto err_free_obj;
		}

		rknpu_obj->kv_addr = kv_addr;
		rknpu_obj->dma_addr = dma_addr;
		rknpu_obj->size = aligned_size;
		rknpu_obj->owner = 1; /* driver owns this allocation */
		rknpu_obj->dmabuf = NULL;
		rknpu_obj->attachment = NULL;
		rknpu_obj->sgt = NULL;

		args.size = aligned_size;
		args.obj_addr = (__u64)(uintptr_t)rknpu_obj;
		args.dma_addr = (__u64)dma_addr;
		args.handle = atomic_inc_return(&handle_counter);
		args.sram_size = 0;

		LOG_DEBUG("mem_create: ALLOC handle=%u size=%llu dma=%#llx\n",
			  args.handle, args.size, args.dma_addr);
	}

	if (unlikely(copy_to_user((struct rknpu_mem_create __user *)data, &args,
				  in_size))) {
		LOG_ERROR("%s: copy_to_user failed\n", __func__);
		ret = -EFAULT;
		goto err_free_alloc;
	}

	/* Track allocation in session for cleanup on fd close */
	spin_lock(&rknpu_dev->lock);
	session = file->private_data;
	if (!session) {
		spin_unlock(&rknpu_dev->lock);
		ret = -EFAULT;
		goto err_free_alloc;
	}
	list_add_tail(&rknpu_obj->head, &session->list);
	spin_unlock(&rknpu_dev->lock);

	return 0;

err_free_alloc:
	if (rknpu_obj->owner) {
		dma_free_coherent(rknpu_dev->dev, rknpu_obj->size,
				  rknpu_obj->kv_addr, rknpu_obj->dma_addr);
	} else if (rknpu_obj->sgt) {
		struct iosys_map unmap = IOSYS_MAP_INIT_VADDR(rknpu_obj->kv_addr);
		if (rknpu_obj->kv_addr)
			dma_buf_vunmap(rknpu_obj->dmabuf, &unmap);
		dma_buf_unmap_attachment(rknpu_obj->attachment,
					rknpu_obj->sgt, DMA_BIDIRECTIONAL);
		dma_buf_detach(rknpu_obj->dmabuf, rknpu_obj->attachment);
		dma_buf_put(rknpu_obj->dmabuf);
	}
	goto err_free_obj;

err_unmap_attachment:
	dma_buf_unmap_attachment(rknpu_obj->attachment,
				rknpu_obj->sgt, DMA_BIDIRECTIONAL);
err_detach:
	dma_buf_detach(rknpu_obj->dmabuf, rknpu_obj->attachment);
err_put_dmabuf:
	dma_buf_put(rknpu_obj->dmabuf);

err_free_obj:
	kfree(rknpu_obj);
	return ret;
}

int rknpu_mem_destroy_ioctl(struct rknpu_device *rknpu_dev, struct file *file,
			    unsigned long data)
{
	struct rknpu_mem_object *rknpu_obj, *entry, *q;
	struct rknpu_session *session = NULL;
	struct rknpu_mem_destroy args;
	bool found = false;

	if (unlikely(copy_from_user(&args, (struct rknpu_mem_destroy __user *)data,
				    sizeof(args)))) {
		LOG_ERROR("%s: copy_from_user failed\n", __func__);
		return -EFAULT;
	}

	rknpu_obj = (struct rknpu_mem_object *)(uintptr_t)args.obj_addr;
	if (!rknpu_obj) {
		LOG_ERROR("%s: invalid obj_addr\n", __func__);
		return -EINVAL;
	}

	LOG_DEBUG("mem_destroy: obj=%#llx dma=%#llx owner=%d\n",
		  args.obj_addr, (__u64)rknpu_obj->dma_addr, rknpu_obj->owner);

	/* Remove from session list */
	spin_lock(&rknpu_dev->lock);
	session = file->private_data;
	if (!session) {
		spin_unlock(&rknpu_dev->lock);
		return -EFAULT;
	}
	list_for_each_entry_safe(entry, q, &session->list, head) {
		if (entry == rknpu_obj) {
			list_del(&entry->head);
			found = true;
			break;
		}
	}
	spin_unlock(&rknpu_dev->lock);

	if (found) {
		if (rknpu_obj->owner) {
			/* Path B: dma_alloc_coherent */
			dma_free_coherent(rknpu_dev->dev, rknpu_obj->size,
					  rknpu_obj->kv_addr,
					  rknpu_obj->dma_addr);
		} else {
			/* Path A: imported DMA-BUF */
			if (rknpu_obj->kv_addr && rknpu_obj->dmabuf) {
				struct iosys_map unmap =
					IOSYS_MAP_INIT_VADDR(rknpu_obj->kv_addr);
				dma_buf_vunmap(rknpu_obj->dmabuf, &unmap);
			}
			if (rknpu_obj->sgt && rknpu_obj->attachment)
				dma_buf_unmap_attachment(rknpu_obj->attachment,
							 rknpu_obj->sgt,
							 DMA_BIDIRECTIONAL);
			if (rknpu_obj->attachment && rknpu_obj->dmabuf)
				dma_buf_detach(rknpu_obj->dmabuf,
					       rknpu_obj->attachment);
			if (rknpu_obj->dmabuf)
				dma_buf_put(rknpu_obj->dmabuf);
		}
		kfree(rknpu_obj);
	}

	return 0;
}

int rknpu_mem_sync_ioctl(struct rknpu_device *rknpu_dev, unsigned long data)
{
	struct rknpu_mem_sync args;
	struct rknpu_mem_object *obj;

	if (unlikely(copy_from_user(&args, (struct rknpu_mem_sync __user *)data,
				    sizeof(args)))) {
		LOG_ERROR("%s: copy_from_user failed\n", __func__);
		return -EFAULT;
	}

	obj = (struct rknpu_mem_object *)(uintptr_t)args.obj_addr;
	if (!obj)
		return -EINVAL;

	/*
	 * For dma_alloc_coherent memory (owner=1): no sync needed
	 * (cache-coherent by definition).
	 *
	 * For imported DMA-BUFs (owner=0): must flush CPU caches to
	 * make writes visible to the NPU via DMA. The SDK calls this
	 * after writing task descriptors, regcmds, and input data.
	 *
	 * On BSP 5.10 the driver called dma_sync_single_for_device()
	 * directly. Here we use dma_sync_sgtable which works on the
	 * DMA-BUF attachment's scatter-gather table.
	 */
	if (!obj->owner && obj->sgt) {
		if (args.flags & RKNPU_MEM_SYNC_TO_DEVICE) {
			dma_sync_sgtable_for_device(rknpu_dev->dev,
						    obj->sgt, DMA_TO_DEVICE);
			LOG_INFO("mem_sync: TO_DEVICE obj=%p dma=0x%llx size=%lu\n",
				 obj, (u64)obj->dma_addr, obj->size);
		}
		if (args.flags & RKNPU_MEM_SYNC_FROM_DEVICE) {
			dma_sync_sgtable_for_cpu(rknpu_dev->dev,
						 obj->sgt, DMA_FROM_DEVICE);
			LOG_INFO("mem_sync: FROM_DEVICE obj=%p dma=0x%llx size=%lu\n",
				 obj, (u64)obj->dma_addr, obj->size);
		}
	}

	return 0;
}
