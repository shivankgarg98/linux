/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Header for sq and descriptor management
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#ifndef __SDXI_SQ_H
#define __SDXI_SQ_H

#include <linux/io-64-nonatomic-lo-hi.h>
#include <asm/barrier.h>

#include "sdxi.h"

struct sdxi_cxt {
	struct sdxi_dev *sdxi;	/* owner */
	unsigned int id;
	bool privileged;

	resource_size_t db_base;	/* doorbell MMIO base addr */
	__le64 __iomem *db;		/* doorbell virt addr */

	struct sdxi_cxt_ctl *cxt_ctl;
	dma_addr_t cxt_ctl_dma;

	struct sdxi_akey_table *akey_table;
	dma_addr_t akey_table_dma;

	struct sdxi_sq *sq;

	struct sdxi_process *process;	/* process reprsentation */

	struct sdxi_ring_state *ring_state;
};

struct sdxi_desc;

enum sdxi_cxt_id {
	SDXI_ADMIN_CXT_ID = 0,
	SDXI_ANY_CXT_ID,
};

/* Context Control */
struct sdxi_cxt *sdxi_working_cxt_init(struct sdxi_dev *sdxi,
				       enum sdxi_cxt_id);
void sdxi_working_cxt_exit(struct sdxi_cxt *cxt);
struct sdxi_cxt *sdxi_kcxt_new(struct sdxi_dev *sdxi);

int sdxi_submit_desc(struct sdxi_cxt *cxt, const struct sdxi_desc *desc);

int sdxi_cxt_initiate_stop(struct sdxi_cxt *cxt);

static inline void sdxi_cxt_push_doorbell(struct sdxi_cxt *cxt, u64 index)
{
	/* Ensure write index is visible. */
	dma_wmb();
	sdxi_dbg(cxt->sdxi, "Ringing context %u doorbell: %llu\n",
		 cxt->id, index);
	iowrite64(index, cxt->db);
}

static inline bool sdxi_cxt_stopped(const struct sdxi_cxt *cxt)
{
	switch (sdxi_cxt_sts_state(cxt->sq->cxt_sts)) {
	case CXTV_STOP_SW:
	case CXTV_STOP_FN:
	case CXTV_ERR_FN:
		return true;
	default:
		return false;
	}
}



#endif /* __SDXI_SQ_H */
