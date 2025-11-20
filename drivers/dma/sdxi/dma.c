// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDXI DMA engine implementation
 *   Derived from ptdma code
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/container_of.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/list.h>
#include <linux/spinlock.h>

#include "../dmaengine.h"
#include "../virt-dma.h"
#include "context.h"
#include "descriptor.h"
#include "dma.h"
#include "ring.h"
#include "sdxi.h"

struct sdxi_dma_chan {
	struct virt_dma_chan vchan;
	struct sdxi_cxt *cxt;
};

/*
 * A virtual descriptor can correspond to a group of SDXI hardware descriptors.
 */
struct sdxi_dma_desc {
	struct virt_dma_desc vdesc;
	struct sdxi_ring_resv resv;
	struct sdxi_cst_blk *cst_blk;
	dma_addr_t cst_blk_dma;
};

static struct sdxi_dma_chan *to_sdxi_dma_chan(const struct dma_chan *dma_chan)
{
	const struct virt_dma_chan *vchan;

	vchan = container_of_const(dma_chan, struct virt_dma_chan, chan);
	return container_of(vchan, struct sdxi_dma_chan, vchan);
}

static struct sdxi_dma_desc *
to_sdxi_dma_desc(const struct virt_dma_desc *vdesc)
{
	return container_of(vdesc, struct sdxi_dma_desc, vdesc);
}

static void sdxi_dma_free_chan_resources(struct dma_chan *dma_chan)
{
	struct sdxi_dma_chan *chan = to_sdxi_dma_chan(dma_chan);

	/*
	 * Dmaengine has drained all pending txds. Stop the context.
	 * sdxi_working_cxt_exit() isn't what we want, it frees the
	 * context.
	 */
	sdxi_working_cxt_exit(chan->cxt);
}

static void sdxi_tx_desc_free(struct virt_dma_desc *vdesc)
{
	kfree(to_sdxi_dma_desc(vdesc));
}

static struct dma_async_tx_descriptor *
sdxi_dma_prep_memcpy(struct dma_chan *dma_chan, dma_addr_t dst,
		     dma_addr_t src, size_t len, unsigned long flags)
{
	struct dma_async_tx_descriptor *txd;
	struct sdxi_dma_desc *sddesc __free(kfree) = NULL;
	struct sdxi_cst_blk *cst_blk;
	dma_addr_t cst_blk_dma;
	struct sdxi_cxt *cxt = to_sdxi_dma_chan(dma_chan)->cxt;
	struct sdxi_copy copy = {
		.src = src,
		.dst = dst,
		.src_akey = 0,
		.dst_akey = 0,
		.len = len,
	};

	/*
	 * Sorry, no interrupt-signaled completion yet.
	 */
	if (WARN_ON_ONCE(flags & DMA_PREP_INTERRUPT))
		return NULL;
	/*
	 * temp hack: perform a trial encode to a dummy descriptor on
	 * the stack so we can reject bad inputs without touching the
	 * ring state.
	 */
	if (sdxi_encode_copy(&(struct sdxi_desc){}, &copy))
		return NULL;

	/* FIXME: use a pool? this is wasteful. */
	/* FIXME also: this leaks when the reservation fails.*/
	cst_blk = dma_alloc_coherent(sdxi_to_dev(cxt->sdxi), sizeof(*cst_blk),
				     &cst_blk_dma, GFP_NOWAIT);
	if (!cst_blk)
		return NULL;

	cst_blk->signal = cpu_to_le64(1);

	sddesc = kmalloc(sizeof(*sddesc), GFP_NOWAIT);
	if (!sddesc)
		return NULL;

	*sddesc = (typeof(*sddesc)) {
		.cst_blk = cst_blk,
		.cst_blk_dma = cst_blk_dma,
	};

	if (sdxi_ring_reserve(cxt->ring_state, 1, &sddesc->resv))
		return NULL;

	struct sdxi_desc *hwdesc = sdxi_ring_resv_next(&sddesc->resv);

	(void)sdxi_encode_copy(hwdesc, &copy);
	sdxi_desc_set_csb(hwdesc, cst_blk_dma);

	txd = vchan_tx_prep(to_virt_chan(dma_chan), &sddesc->vdesc, flags);
	retain_and_null_ptr(sddesc);
	return txd;
}

static bool sdxi_cst_blk_erred(const struct sdxi_cst_blk *cst)
{
	return FIELD_GET(SDXI_CST_BLK_ER_BIT, le32_to_cpu(cst->flags));
}

static bool sdxi_cst_blk_complete(const struct sdxi_cst_blk *cst)
{
	return cst->signal == 0;
}

static enum dma_status sdxi_tx_status(struct dma_chan *chan,
				      dma_cookie_t cookie,
				      struct dma_tx_state *state)
{
	struct sdxi_dma_chan *sdchan = to_sdxi_dma_chan(chan);
	struct sdxi_dma_desc *sddesc;
	enum dma_status status;
	struct virt_dma_desc *vdesc;

	status = dma_cookie_status(chan, cookie, state);
	if (status == DMA_COMPLETE)
		return status;

	guard(spinlock_irqsave)(&sdchan->vchan.lock);

	vdesc = vchan_find_desc(&sdchan->vchan, cookie);
	if (!vdesc)
		return status;

	sddesc = to_sdxi_dma_desc(vdesc);

	if (sdxi_cst_blk_erred(sddesc->cst_blk))
		return DMA_ERROR;

	/* fixme? should this happen? */
	if (sdxi_cst_blk_complete(sddesc->cst_blk))
		return DMA_COMPLETE;

	return DMA_IN_PROGRESS;
}


static void sdxi_dma_issue_pending(struct dma_chan *dma_chan)
{
	struct virt_dma_chan *vchan = to_virt_chan(dma_chan);
	struct virt_dma_desc *vdesc;
	u64 dbval = 0;

	scoped_guard(spinlock_irqsave, &vchan->lock) {
		if (list_empty(&vchan->desc_submitted)) {
			pr_info("submitted list empty?");
			return;
		}

		list_for_each_entry(vdesc, &vchan->desc_submitted, node) {
			struct sdxi_dma_desc *sddesc = to_sdxi_dma_desc(vdesc);
			struct sdxi_desc *hwdesc;

			sdxi_ring_resv_foreach(&sddesc->resv, hwdesc)
				sdxi_desc_make_valid(hwdesc);
			/*
			 * The reservations ought to be ordered
			 * ascending, but use umax() just in case.
			 */
			dbval = umax(sdxi_ring_resv_dbval(&sddesc->resv), dbval);
		}

		vchan_issue_pending(vchan);
	}

	/*
	 * The implementation is required to handle out-of-order
	 * doorbell updates; we can do this after dropping the
	 * lock.
	 */
	sdxi_cxt_push_doorbell(to_sdxi_dma_chan(dma_chan)->cxt, dbval);
}

static int sdxi_dma_terminate_all(struct dma_chan *dma_chan)
{
	/* FIXME: do we really want to stop the context? */
	return sdxi_cxt_initiate_stop(to_sdxi_dma_chan(dma_chan)->cxt);
}

static void sdxi_dma_synchronize(struct dma_chan *dma_chan)
{
	while (!sdxi_cxt_stopped(to_sdxi_dma_chan(dma_chan)->cxt))
		msleep(100);
	vchan_synchronize(to_virt_chan(dma_chan));
}

static struct sdxi_cxt *start_dma_cxt(struct sdxi_dev *sdxi)
{
	struct sdxi_cxt_start params;
	struct sdxi_desc desc;
	struct sdxi_cxt *cxt;
	int err;

	cxt = sdxi_kcxt_new(sdxi);
	if (!cxt)
		return NULL;

	params = (typeof(params)) {
		.range = sdxi_cxt_range(cxt->id),
	};

	err = sdxi_encode_cxt_start(&desc, &params);
	if (err)
		goto cxt_exit;

	err = sdxi_submit_desc(sdxi->admin_cxt, &desc);
	if (err)
		goto cxt_exit;

	return cxt;

cxt_exit:
	sdxi_working_cxt_exit(cxt);
	return NULL;
}

static void add_channel(struct dma_device *dma_dev)
{
	struct sdxi_dma_chan *sdchan;
	struct sdxi_dev *sdxi = dev_get_drvdata(dma_dev->dev);

	sdchan = devm_kzalloc(dma_dev->dev, sizeof(*sdchan), GFP_KERNEL);
	if (!sdchan)
		return;

	sdchan->cxt = start_dma_cxt(sdxi);
	if (!sdchan->cxt) {
		devm_kfree(dma_dev->dev, sdchan);
		return;
	}

	sdchan->vchan.desc_free = sdxi_tx_desc_free;
	vchan_init(&sdchan->vchan, dma_dev);
}

int sdxi_dma_register(struct sdxi_dev *sdxi)
{
	struct device *dev = sdxi_to_dev(sdxi);
	struct dma_device *dma_dev;

	/*
	 * FIXME: This code assumes the device supports the interrupt
	 * operation group. It's probably not a bad assumption, but
	 * IntrGrp is optional in the spec. We should probe the
	 * device's opgroups and bail if IntrGrp isn't implemented.
	 */

	dma_dev = devm_kzalloc(sdxi_to_dev(sdxi), sizeof(*dma_dev), GFP_KERNEL);
	if (!dma_dev)
		return -ENOMEM;

	*dma_dev = (typeof(*dma_dev)) {
		.dev                 = sdxi_to_dev(sdxi),
		.src_addr_widths     = DMA_SLAVE_BUSWIDTH_64_BYTES,
		.dst_addr_widths     = DMA_SLAVE_BUSWIDTH_64_BYTES,
		.directions          = BIT(DMA_MEM_TO_MEM),
		.residue_granularity = DMA_RESIDUE_GRANULARITY_DESCRIPTOR,

		.device_alloc_chan_resources = NULL, /* fixme */
		.device_free_chan_resources  = sdxi_dma_free_chan_resources,

		.device_prep_dma_memcpy = sdxi_dma_prep_memcpy,

		.device_pause = NULL, /* fixme */
		.device_resume = NULL, /* fixme */
		.device_terminate_all = sdxi_dma_terminate_all,
		.device_synchronize = sdxi_dma_synchronize,
		.device_tx_status = sdxi_tx_status,
		.device_issue_pending = sdxi_dma_issue_pending,
		.device_release = NULL, /* fixme */
	};

	dma_cap_set(DMA_MEMCPY, dma_dev->cap_mask);
	dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	INIT_LIST_HEAD(&dma_dev->channels);

	for (size_t i = 0; i < 1; i++)
		add_channel(dma_dev);

	return dmaenginem_async_device_register(dma_dev);
}
