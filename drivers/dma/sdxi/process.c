// SPDX-License-Identifier: GPL-2.0-only
/*
 * User space process management (IOMMU, PASID, etc.)
 *
 * Copyright (C) 2022 AMD, Inc. All rights reserved.
 *
 * Author: Wei Huang <wei.huang2@amd.com>
 */

#include <linux/device.h>
#include <linux/iommu.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/idr.h>
#include <linux/dma-fence-array.h>
#include <linux/amd-iommu.h>
#include <linux/hashtable.h>
#include <asm/mmu.h>

#include "process.h"
#include "trace.h"

#define SDXI_PROCESS_LIST_SIZE	16
static DEFINE_MUTEX(process_list_mutex);
static DEFINE_HASHTABLE(process_list, SDXI_PROCESS_LIST_SIZE);
DEFINE_STATIC_SRCU(process_list_srcu);

/**********************/
/* PROCESS MANAGEMENT */
/**********************/
static struct sdxi_process *find_process_by_mm(const struct mm_struct *mm)
{
	struct sdxi_process *process;

	hash_for_each_possible_rcu(process_list, process, list,
				   (uintptr_t)mm) {
		if (process->mm == mm)
			return process;
	}

	return NULL;
}

static struct sdxi_process *find_process(const struct task_struct *thread)
{
	struct sdxi_process *p;
	int idx;

	idx = srcu_read_lock(&process_list_srcu);
	p = find_process_by_mm(thread->mm);
	srcu_read_unlock(&process_list_srcu, idx);

	return p;
}

void sdxi_unref_process(struct sdxi_process *p)
{
	/* NB: more checking to be done here based on kref count */
}

struct sdxi_process *sdxi_get_process(struct task_struct *thread)
{
	struct sdxi_process *process;

	if (!thread->mm)
		return ERR_PTR(-EINVAL);

	process = find_process(thread);
	if (!process)
		return ERR_PTR(-EINVAL);

	return process;
}

int sdxi_bind_process_to_device(struct sdxi_process *process)
{
	struct sdxi_cxt *cxt = process->cxt;
	struct iommu_sva *sva;
	struct sdxi_dev *sdxi;
	struct device *dev;
	u32 pasid;
	int err;

	if (!cxt)
		return -EINVAL;

	sdxi = cxt->sdxi;
	dev = &sdxi->pdev->dev;
	sva = iommu_sva_bind_device(dev, process->mm);
	if (IS_ERR(sva))
		return PTR_ERR(sva);

	pasid = iommu_sva_get_pasid(sva);
	if (pasid == IOMMU_PASID_INVALID) {
		err = -EINVAL;
		goto unbind;
	}

	process->pasid = pasid;
	process->sva = sva;

	trace_sdxi_bind_process(sdxi, process->pasid);

	return 0;
unbind:
	iommu_sva_unbind_device(sva);
	return err;
}

void sdxi_unbind_process_to_device(struct sdxi_process *process)
{
	struct sdxi_cxt *cxt = process->cxt;

	// FIXME: Can either of these really happen? Are they
	// WARN_ON()-worthy?
	if (!cxt || !process->sva)
		return;

	iommu_sva_unbind_device(process->sva);

	trace_sdxi_unbind_process(cxt->sdxi, process->pasid);
}

struct sdxi_process *sdxi_create_process(struct file *filep)
{
	struct task_struct *thread = current;
	struct sdxi_process *process;
	int err = -ENOMEM;

	mutex_lock(&process_list_mutex);

	process = kzalloc(sizeof(*process), GFP_KERNEL);
	if (!process)
		return ERR_PTR(err);

	mutex_init(&process->mutex);
	process->mm = thread->mm;
	process->lead_thread = thread->group_leader;
	hash_add_rcu(process_list, &process->list, (uintptr_t)process->mm);
	get_task_struct(process->lead_thread);
	mutex_unlock(&process_list_mutex);

	return process;
}

void sdxi_destroy_process(struct sdxi_process *process)
{
	if (!process)
		return;

	mutex_lock(&process_list_mutex);

	put_task_struct(process->lead_thread);
	hash_del_rcu(&process->list);
	mutex_destroy(&process->mutex);
	kfree(process);

	mutex_unlock(&process_list_mutex);
}
