// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDXI submission queue (sq) and descriptor management
 *
 * Copyright (C) 2022 AMD, Inc. All rights reserved.
 *
 * Author: Wei Huang <wei.huang2@amd.com>
 *
 */

#define pr_fmt(fmt)     "SDXI: " fmt

#include <linux/delay.h>
#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/types.h>
#include <linux/wordpart.h>

#include "context.h"
#include "descriptor.h"
#include "enqueue.h"
#include "hw.h"
#include "sdxi.h"
#include "trace.h"

/* Alloc sdxi_sq in kernel space */
struct sdxi_sq *sdxi_sq_alloc(struct sdxi_cxt *cxt, int ring_entries)
{
	struct sdxi_dev *sdxi = cxt->sdxi;
	struct device *dev = sdxi_to_dev(sdxi);
	u64 write_index_ptr;
	struct sdxi_sq *sq;
	u64 ds_ring_ptr;
	u64 cxt_sts_ptr;
	u32 ds_ring_sz;

	/* alloc desc_ring */
	if (ring_entries > sdxi->max_ring_entries) {
		sdxi_err(sdxi, "%d ring entries requested, max is %llu\n",
			ring_entries, sdxi->max_ring_entries);
		return NULL;
	}

	sq = kzalloc(sizeof(*sq), GFP_KERNEL);
	if (!sq)
		return NULL;

	sq->ring_entries = ring_entries;
	sq->ring_size = sizeof(sq->desc_ring[0]) * sq->ring_entries;
	sq->desc_ring = dma_alloc_coherent(dev, sq->ring_size, &sq->ring_dma,
					   GFP_KERNEL);
	if (!sq->desc_ring)
		goto free_sq;

	sq->cxt_sts = dma_pool_zalloc(sdxi->cxt_sts_pool, GFP_KERNEL, &sq->cxt_sts_dma);
	if (!sq->cxt_sts)
		goto free_desc_ring;

	sq->write_index = dma_pool_zalloc(sdxi->write_index_pool, GFP_KERNEL,
					  &sq->write_index_dma);
	if (!sq->write_index)
		goto free_cxt_sts;

	/* final setup */
	if (cxt->id == SDXI_ADMIN_CXT_ID || cxt->id == SDXI_DMA_CXT_ID)
		sq->cxt_sts->state = FIELD_PREP(SDXI_CXT_STS_STATE, CXTV_RUN);

	write_index_ptr = FIELD_PREP(SDXI_CXT_CTL_WRITE_INDEX_PTR,
				     sq->write_index_dma >> 3);
	cxt_sts_ptr = FIELD_PREP(SDXI_CXT_CTL_CXT_STS_PTR,
				 sq->cxt_sts_dma >> 4);
	ds_ring_sz = sq->ring_size >> 6;

	cxt->cxt_ctl->write_index_ptr = cpu_to_le64(write_index_ptr);
	cxt->cxt_ctl->cxt_sts_ptr     = cpu_to_le64(cxt_sts_ptr);
	cxt->cxt_ctl->ds_ring_sz      = cpu_to_le32(ds_ring_sz);

	/* turn it on now */
	sq->cxt = cxt;
	cxt->sq = sq;
	ds_ring_ptr = (FIELD_PREP(SDXI_CXT_CTL_DS_RING_PTR, sq->ring_dma >> 6) |
		       FIELD_PREP(SDXI_CXT_CTL_VL, 1));
	dma_wmb();
	WRITE_ONCE(cxt->cxt_ctl->ds_ring_ptr, cpu_to_le64(ds_ring_ptr));

	sdxi_dbg(sdxi, "sq created, id=%d, cxt_ctl=%p\n"
		 "  desc ring addr:   v=0x%p:d=%pad\n"
		 "  write index addr: v=0x%p:d=%pad\n"
		 "  cxt status addr: v=0x%p:d=%pad\n",
		 cxt->id, cxt->cxt_ctl,
		 sq->desc_ring, &sq->ring_dma,
		 sq->write_index, &sq->write_index_dma,
		 sq->cxt_sts, &sq->cxt_sts_dma);

	/* dump SQ info */
	trace_sdxi_create_sq(cxt, sq);

	return sq;

free_cxt_sts:
	dma_pool_free(sdxi->cxt_sts_pool, sq->cxt_sts, sq->cxt_sts_dma);
free_desc_ring:
	dma_free_coherent(dev, sq->ring_size, sq->desc_ring, sq->ring_dma);
free_sq:
	kfree(sq);
	return NULL;
}

void sdxi_sq_free(struct sdxi_sq *sq)
{
	struct sdxi_cxt *cxt = sq->cxt;
	struct sdxi_dev *sdxi = cxt->sdxi;
	struct device *dev = sdxi_to_dev(sdxi);

	if (!cxt)
		return;

	trace_sdxi_free_sq(cxt, sq);

	dma_pool_free(sdxi->write_index_pool, sq->write_index, sq->write_index_dma);
	dma_pool_free(sdxi->cxt_sts_pool, sq->cxt_sts, sq->cxt_sts_dma);
	dma_free_coherent(dev, sq->ring_size, sq->desc_ring, sq->ring_dma);

	cxt->sq = NULL;
	kfree(sq);
}

/* Default size 1024 ==> 64KB descriptor ring, guaranteed */
#define DEFAULT_DESC_RING_ENTRIES	1024
struct sdxi_sq *sdxi_sq_alloc_default(struct sdxi_cxt *cxt)
{
	return sdxi_sq_alloc(cxt, DEFAULT_DESC_RING_ENTRIES);
}

static const char *cxt_sts_state_str(enum cxt_sts_state state)
{
	static const char *const context_states[] = {
		[CXTV_STOP_SW]  = "stopped (software)",
		[CXTV_RUN]      = "running",
		[CXTV_STOPG_SW] = "stopping (software)",
		[CXTV_STOP_FN]  = "stopped (function)",
		[CXTV_STOPG_FN] = "stopping (function)",
		[CXTV_ERR_FN]   = "error",
	};
	const char *str = "unknown";

	switch (state) {
	case CXTV_STOP_SW:
	case CXTV_RUN:
	case CXTV_STOPG_SW:
	case CXTV_STOP_FN:
	case CXTV_STOPG_FN:
	case CXTV_ERR_FN:
		str = context_states[state];
	}

	return str;
}

static void sdxi_cxt_shutdown(struct sdxi_cxt *target_cxt)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(1000);
	struct sdxi_cxt *admin_cxt = target_cxt->sdxi->admin_cxt;
	struct sdxi_dev *sdxi = target_cxt->sdxi;
	struct sdxi_cxt_sts *sts = target_cxt->sq->cxt_sts;
	struct sdxi_desc desc;
	u16 cxtid = target_cxt->id;
	struct sdxi_cxt_stop params = {
		.range = sdxi_cxt_range(cxtid),
	};
	enum cxt_sts_state state = sdxi_cxt_sts_state(sts);
	int err;

	sdxi_dbg(sdxi, "%s entry: context state: %s",
		 __func__, cxt_sts_state_str(state));

	err = sdxi_encode_cxt_stop(&desc, &params);
	err = sdxi_enqueue(&desc.qw[0], 1,
			   (__le64 *)admin_cxt->sq->desc_ring,
			   admin_cxt->sq->ring_entries,
			   &admin_cxt->sq->cxt_sts->read_index,
			   admin_cxt->sq->write_index, admin_cxt->db);
	if (err) {
		sdxi_err(sdxi, "error %d shutting down context %u\n",
			 err, cxtid);
		return;
	}

	sdxi_dbg(sdxi, "shutting down context %u\n", cxtid);

	do {
		enum cxt_sts_state state = sdxi_cxt_sts_state(sts);

		sdxi_dbg(sdxi, "context %u state: %s", cxtid,
			 cxt_sts_state_str(state));

		switch (state) {
		case CXT_STATE_ERR:
			sdxi_err(sdxi, "context %u went into error state while stopping\n",
				cxtid);
			fallthrough;
		case CXTV_STOP_SW:
		case CXTV_STOP_FN:
			return;
		case CXTV_RUN:
		case CXTV_STOPG_SW:
		case CXTV_STOPG_FN:
			// transitional states
			fsleep(1000);
			break;
		default:
			sdxi_err(sdxi, "context %u in unknown state %u\n",
				 cxtid, state);
			return;
		}
	} while (time_before(jiffies, deadline));

	sdxi_err(sdxi, "stopping context %u timed out (state = %u)\n",
		cxtid, sdxi_cxt_sts_state(sts));
}

void sdxi_working_cxt_exit(struct sdxi_cxt *cxt)
{
	struct sdxi_sq *sq;

	if (!cxt)
		return;

	sq = cxt->sq;
	if (!sq)
		return;

	sdxi_cxt_shutdown(cxt);

	sdxi_sq_free(sq);

	sdxi_cxt_free(cxt);
}
