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

#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/types.h>
#include <linux/wordpart.h>

#include "context.h"
#include "hw.h"
#include "sdxi.h"
#include "trace.h"

void build_dma_copy(struct sdxi_desc *desc, u32 size, u8 src_attr,
		    u8 dst_attr, u16 src_akey, u16 dst_akey,
		    u64 src_addr, u64 dst_addr)
{
	memset(desc, 0, sizeof(*desc));

	desc->fe = 1;
	desc->body[0] |= size - 1; // size is encoded as (actual size - 1)
	desc->body[1] |= (src_attr & 0xF);
	desc->body[1] |= ((dst_attr & 0xF) << 4);
	desc->body[2] |= lower_16_bits(src_akey);
	desc->body[2] |= upper_16_bits(dst_akey);
	desc->body[3] = lower_32_bits(src_addr);
	desc->body[4] = upper_32_bits(src_addr);
	desc->body[5] = lower_32_bits(dst_addr);
	desc->body[6] = upper_32_bits(dst_addr);
	DESC_BUILD_TYPE(desc, OP_TYPE_DMA, OP_DMA_COPY);
}

static u64 sdxi_cxt_sts_read_index(const struct sdxi_cxt_sts *sts)
{
	return le64_to_cpu(READ_ONCE(sts->read_index));
}

static void sdxi_sq_ring_doorbell(struct sdxi_sq *sq, u64 value)
{
	struct sdxi_cxt *cxt = sq->cxt;

	iowrite64(value, cxt->db);
}

u64 sdxi_sq_submit_desc(struct sdxi_sq *sq, struct sdxi_desc *desc,
			bool csb, u64 init_signal)
{
	struct sdxi_dev *sdxi = sq->cxt->sdxi;
	u64 dest;

	if (WARN_ON_ONCE(csb))
		return -EINVAL;

	/* check context status */
	if (sdxi_cxt_sts_state(sq->cxt_sts) != CXTV_RUN) {
		sdxi_err(sdxi, "Context is not running\n");
		return -EINVAL;
	}

	/* no more room for any descriptor */
	if (*sq->write_index + 1 - sdxi_cxt_sts_read_index(sq->cxt_sts) > sq->ring_entries) {
		sdxi_err(sdxi, "desc ring is full\n");
		return -EINVAL;
	}

	/* NB: Atomic_INC */
	desc->vl = 0;
	dest = *sq->write_index;
	dest %= sq->ring_entries;
	memcpy(&sq->desc_ring[dest], desc, sizeof(struct sdxi_desc));
	sq->desc_ring[dest].vl = 1;
	/* make sure the update of valid bit is visible */
	wmb();
	*sq->write_index += 1;

	/* ring the door bell */
	sdxi_sq_ring_doorbell(sq, *sq->write_index);

	return dest;
}

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
	sq->ring_size = sizeof(struct sdxi_desc) * sq->ring_entries;
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
