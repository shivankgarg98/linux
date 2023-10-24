/*
 * SDXI submission queue (sq) and descriptor management
 *
 * Copyright (C) 2022 AMD, Inc. All rights reserved.
 *
 * Author: Wei Huang <wei.huang2@amd.com>
 *
 */

#define pr_fmt(fmt)     "SDXI: " fmt
#define dev_fmt(fmt)    pr_fmt(fmt)

#include "sdxi.h"
#include "pci.h"
#include "sq.h"
#include "trace.h"

/* NB: take care of completion pointer */
void build_admin_update_func(struct sdxi_desc *desc, bool vf, u16 vf_num)
{
	memset(desc, 0, sizeof(*desc));

	DESC_ADM_BUILD_VF(desc, vf, vf_num);
	DESC_BUILD_TYPE(desc, OP_TYPE_ADMIN, OP_ADMIN_UPDATE_FUNC);
}

void build_admin_update_ctxt(struct sdxi_desc *desc, bool vf, u16 vf_num,
			     bool v2, bool v1, bool ct, u16 ctxt_num,
			     u16 ctxt_mask)
{
	memset(desc, 0, sizeof(*desc));

	desc->body[0] |= (v2 << 0);
	desc->body[0] |= (v1 << 1);
	desc->body[0] |= (ct << 2);
	DESC_ADM_BUILD_VF(desc, vf, vf_num);
	DESC_ADM_BUILD_CTXT(desc, ctxt_num, ctxt_mask);
	DESC_BUILD_TYPE(desc, OP_TYPE_ADMIN, OP_ADMIN_UPDATE_CTXT);
}

void build_admin_start(struct sdxi_desc *desc, bool dr, bool vf,
		       u16 vf_num, u16 ctxt_num, u16 ctxt_mask,
		       u64 doorbell)
{
	memset(desc, 0, sizeof(*desc));

	desc->fe = 1;
	desc->body[0] |= (dr << 14);
	DESC_ADM_BUILD_VF(desc, vf, vf_num);
	DESC_ADM_BUILD_CTXT(desc, ctxt_num, ctxt_mask);
	desc->body[3] |= (doorbell & 0xFFFFFFFF);
	desc->body[4] |= ((doorbell >> 32) & 0xFFFFFFFF);
	DESC_BUILD_TYPE(desc, OP_TYPE_ADMIN, OP_ADMIN_START);
}

void build_admin_start_new(struct sdxi_desc *desc, bool vf, u16 vf_num,
			   u16 ctxt_start, u16 ctxt_end, u64 doorbell)
{
	memset(desc, 0, sizeof(*desc));

	desc->fe = 1;
	DESC_ADM_BUILD_VF(desc, vf, vf_num);
	DESC_ADM_BUILD_CTXT(desc, ctxt_start, ctxt_end);
	desc->body[3] |= (doorbell & 0xFFFFFFFF);
	desc->body[4] |= ((doorbell >> 32) & 0xFFFFFFFF);
	DESC_BUILD_TYPE(desc, OP_TYPE_ADMIN, OP_ADMIN_START);
	desc->csb_ptr = 0x1;
}

void build_admin_stop(struct sdxi_desc *desc, bool hs, bool vf,
		      u16 vf_num, u16 ctxt_num, u16 ctxt_mask)
{
	memset(desc, 0, sizeof(*desc));

	desc->fe = 1;
	desc->body[0] |= (hs << 13);
	DESC_ADM_BUILD_VF(desc, vf, vf_num);
	DESC_ADM_BUILD_CTXT(desc, ctxt_num, ctxt_mask);
	DESC_BUILD_TYPE(desc, OP_TYPE_ADMIN, OP_ADMIN_STOP);
}

void build_admin_sync(struct sdxi_desc *desc, bool vf, u16 vf_num,
		      u16 ctxt_num, u16 ctxt_mask, u16 akey_num,
		      u16 akey_mask)
{
	memset(desc, 0, sizeof(*desc));

	desc->fe = 1;
	DESC_ADM_BUILD_VF(desc, vf, vf_num);
	DESC_ADM_BUILD_CTXT(desc, ctxt_num, ctxt_mask);
	DESC_ADM_BUILD_AKEY(desc, akey_num, akey_mask);
	DESC_BUILD_TYPE(desc, OP_TYPE_ADMIN, OP_ADMIN_SYNC);
}

void build_dma_nop(struct sdxi_desc *desc)
{
	memset(desc, 0, sizeof(*desc));

	DESC_BUILD_TYPE(desc, OP_TYPE_DMA, OP_DMA_NOP);
}

void build_dma_copy(struct sdxi_desc *desc, u32 size, u8 src_attr,
		    u8 dst_attr, u16 src_akey, u16 dst_akey,
		    u64 src_addr, u64 dst_addr, u64 csb_ptr)
{
	memset(desc, 0, sizeof(*desc));

	desc->fe = 1;
	desc->body[0] |= size;
	desc->body[1] |= (src_attr & 0xF);
	desc->body[1] |= ((dst_attr & 0xF) << 4);
	desc->body[2] |= (src_akey & 0xFFFF);
	desc->body[2] |= ((dst_akey & 0xFFFF) << 16);
	desc->body[3] = (src_addr & 0xFFFFFFFF);
	desc->body[4] = ((src_addr >> 32) & 0xFFFFFFFF);
	desc->body[5] = (dst_addr & 0xFFFFFFFF);
	desc->body[6] = ((dst_addr >> 32) & 0xFFFFFFFF);
	desc->csb_ptr = csb_ptr ? csb_ptr : 0x1;
	DESC_BUILD_TYPE(desc, OP_TYPE_DMA, OP_DMA_COPY);
}

void build_dma_write_imm(struct sdxi_desc *desc, u32 size, u64 dst_addr,
			u32 data)
{
	memset(desc, 0, sizeof(*desc));

	desc->body[0] |= size-1;
	desc->body[3] = (dst_addr & 0xFFFFFFFF);
	desc->body[4] = ((dst_addr >> 32) & 0xFFFFFFFF);
	desc->body[5] |= data;
	DESC_BUILD_TYPE(desc, OP_TYPE_DMA, OP_DMA_WRT_IMM);
}

static inline void sdxi_sq_ring_doorbell(struct sdxi_sq *sq, u64 value)
{
	struct sdxi_ctxt *ctxt = sq->ctxt;

	reg_write64(ctxt->db, value);
}

u64 sdxi_sq_submit_desc(struct sdxi_sq *sq, struct sdxi_desc *desc,
			bool csb, u64 init_signal)
{
	struct device *dev = &sq->ctxt->sdxi->pdev->dev;
	u64 dest;

	/* check context status */
	if (sq->ctxt_status->state != CTXT_STATE_RUNNING) {
		dev_err(dev, "Context is not running\n");
		return -EINVAL;
	}

	/* no more room for any descriptor */
	if (*sq->write_index + 1 - sq->ctxt_status->read_idx > sq->ring_entries) {
		dev_err(dev, "desc ring is full\n");
		return -EINVAL;
	}

	/* NB: Atomic_INC */
	desc->vl = 0;
	dest = *sq->write_index;
	dest %= sq->ring_entries;
	memcpy(&sq->desc_ring[dest], desc, sizeof(struct sdxi_desc));
	if (csb) {
		memset(&sq->csb[dest], 0, sizeof(struct csb));
		sq->csb[dest].signal = init_signal;
		sq->desc_ring[dest].csb_ptr = sq->csb_dma + dest * sizeof(struct csb);
	}
	sq->desc_ring[dest].vl = 1;
	/* make sure the update of valid bit is visible */
	wmb();
	*sq->write_index += 1;

	/* ring the door bell */
	sdxi_sq_ring_doorbell(sq, *sq->write_index);

	return dest;
}

/* Alloc sdxi_sq in kernel space */
struct sdxi_sq *sdxi_sq_alloc(struct sdxi_ctxt *ctxt, int ring_entries)
{
	struct sdxi_dev *sdxi = ctxt->sdxi;
	struct device *dev = &sdxi->pdev->dev;
	struct sdxi_sq *sq;

	sq = kzalloc(sizeof(*sq), GFP_KERNEL);
	if (!sq)
		return NULL;

	/* alloc desc_ring */
	if (ring_entries > sdxi->max_ring_entries) {
		dev_err(dev, "Invalid descriptor ring entries\n");
		goto err_ring_entries;
	}

	sq->ring_entries = ring_entries;
	sq->ring_size = sizeof(struct sdxi_desc) * sq->ring_entries;
	sq->desc_ring = kzalloc(sq->ring_size, GFP_KERNEL);
	if (!sq->desc_ring)
		goto err_desc_ring;
	sq->ring_dma = dma_map_single(dev, sq->desc_ring, sq->ring_size,
				      DMA_BIDIRECTIONAL);

	/* alloc completion status block */
	sq->csb = kzalloc(ring_entries * sizeof(struct csb), GFP_KERNEL);
	if (!sq->csb)
		goto err_csb;
	sq->csb_dma = dma_map_single(dev, sq->csb, ring_entries * sizeof(struct csb),
					 DMA_FROM_DEVICE);

	/* alloc ctxt status (NB: use page size) */
	sq->ctxt_status_size = PAGE_SIZE;
	sq->ctxt_status = kzalloc(sq->ctxt_status_size, GFP_KERNEL);
	if (!sq->ctxt_status)
		goto err_ctxt_status;
	sq->ctxt_status_dma = dma_map_single(dev, sq->ctxt_status, sq->ctxt_status_size,
					     DMA_FROM_DEVICE);

	/* alloc write index (NB: use page size) */
	sq->write_index_size = PAGE_SIZE;
	sq->write_index = kzalloc(sq->write_index_size, GFP_KERNEL);
	if (!sq->write_index)
		goto err_write_index;
	sq->write_index_dma = dma_map_single(dev, sq->write_index, sq->write_index_size,
					     DMA_TO_DEVICE);

	/* final setup */
	if (ctxt->id == SDXI_ADMIN_CTXT_ID)
		sq->ctxt_status->state = CTXT_STATE_RUNNING;
	else if (ctxt->id == SDXI_DMA_CTXT_ID)
		sq->ctxt_status->state = CTXT_STATE_RUNNING;

	ctxt->cce.desc_ring_size = sq->ring_size >> 6;
	ctxt->cce.desc_ring_base = sq->ring_dma >> DESC_RING_BASE_PTR_SHIFT;
	ctxt->cce.ctxt_status_ptr = sq->ctxt_status_dma >> CTXT_STATUS_PTR_SHIFT;
	ctxt->cce.wrt_index_ptr = sq->write_index_dma >> WRT_INDEX_PTR_SHIFT;

	/* turn it on now */
	sq->ctxt = ctxt;
	ctxt->sq = sq;
	ctxt->cce.vl = 1;

	pr_debug("sq created, id=%d, cce=%p\n"
		 "  desc ring addr:   v=0x%p:d=0x%llx\n"
		 "  write index addr: v=0x%p:d=0x%llx\n"
		 "  ctxt status addr: v=0x%p:d=0x%llx\n",
		 ctxt->id, &(ctxt->cce),
		 sq->desc_ring, virt_to_phys(sq->desc_ring),
		 sq->write_index, virt_to_phys(sq->write_index),
		 sq->ctxt_status, virt_to_phys(sq->ctxt_status));

	/* dump SQ info */
	trace_sdxi_create_sq(ctxt, sq);

	return sq;

err_write_index:
	kfree(sq->ctxt_status);
err_ctxt_status:
	kfree(sq->csb);
err_csb:
	kfree(sq->desc_ring);
err_desc_ring:
err_ring_entries:
	kfree(sq);
	return NULL;
}

void sdxi_sq_free(struct sdxi_sq *sq)
{
	struct sdxi_ctxt *ctxt = sq->ctxt;
	struct device *dev;

	if (!ctxt)
		return;

	trace_sdxi_free_sq(ctxt, sq);

	dev = &ctxt->sdxi->pdev->dev;
	memset(&ctxt->cce, 0, sizeof(ctxt->cce));

	kfree(sq->write_index);
	kfree(sq->ctxt_status);
	kfree(sq->csb);
	kfree(sq->desc_ring);

	ctxt->sq = NULL;
	kfree(sq);
}

/* Default size 1024 ==> 64KB descriptor ring, guaranteed */
#define DEFAULT_DESC_RING_ENTRIES	1024
struct sdxi_sq *sdxi_sq_alloc_default(struct sdxi_ctxt *ctxt)
{
	return sdxi_sq_alloc(ctxt, DEFAULT_DESC_RING_ENTRIES);
}
