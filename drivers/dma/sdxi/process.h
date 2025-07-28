/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * User space process management
 *
 * Copyright (C) 2022 AMD, Inc. All rights reserved.
 *
 * Author: Wei Huang <wei.huang2@amd.com>
 *
 */

#ifndef __SDXI_PROCESS_H
#define __SDXI_PROCESS_H

#include <linux/mmu_notifier.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/workqueue.h>

#include "sdxi.h"

struct iommu_sva;

/* User Space Process Info */
struct sdxi_process {
	struct hlist_node list;

	struct mutex mutex;
	struct task_struct *lead_thread;
	void *mm;				/* pointer to mm_struct */
	struct mmu_notifier mmu_notifier;	/* mm_struct notifier */

	struct sdxi_cxt *cxt;
	struct iommu_sva *sva;
	u32 pasid;				/* no meaning if !cxt */
};

/* User Process Management */
struct sdxi_process *sdxi_create_process(struct file *filep);
void sdxi_destroy_process(struct sdxi_process *p);
int sdxi_bind_process_to_device(struct sdxi_process *p);
void sdxi_unbind_process_to_device(struct sdxi_process *p);
void sdxi_unref_process(struct sdxi_process *p);
struct sdxi_process *sdxi_get_process(struct task_struct *thread);

#endif /* __SDXI_PROCESS_H */
