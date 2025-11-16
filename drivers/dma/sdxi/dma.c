// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDXI DMA engine implementation
 *   Derived from ptdma code
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/list.h>
#include <linux/spinlock.h>

#include "../dmaengine.h"
#include "context.h"
#include "descriptor.h"
#include "dma.h"
#include "ring.h"
#include "sdxi.h"

struct sdxi_dma_chan {
	struct dma_chan dma_chan;
	struct sdxi_cxt *cxt;
};

struct sdxi_dma_desc {
	struct dma_async_tx_descriptor txd;
	struct dmaengine_result tx_result;
	struct sdxi_desc *hw;
};

static struct sdxi_dma_chan *
to_sdxi_dma_chan(const struct dma_chan *dma_chan)
{
	return container_of(dma_chan, struct sdxi_dma_chan, dma_chan);
}

#if 0
static struct sdxi_dma_desc *
to_sdxi_dma_desc(const struct dma_async_tx_descriptor *txd)
{
	return container_of(txd, struct sdxi_dma_desc, txd);
}
#endif

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

static dma_cookie_t sdxi_tx_submit(struct dma_async_tx_descriptor *tx)
{
	return -1;
}

static int sdxi_tx_desc_free(struct dma_async_tx_descriptor *tx)
{
	return -1;
}

static struct dma_async_tx_descriptor *
sdxi_dma_prep_memcpy(struct dma_chan *dma_chan, dma_addr_t dst,
		     dma_addr_t src, size_t len, unsigned long flags)
{
	struct sdxi_cxt *cxt = to_sdxi_dma_chan(dma_chan)->cxt;
	struct sdxi_dma_desc *sddesc;
	struct sdxi_desc *desc;
	struct sdxi_ring_resv resv;
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

	if (sdxi_ring_reserve(cxt->ring_state, 1, &resv))
		return NULL;

	desc = sdxi_ring_resv_next(&resv);

	if (sdxi_encode_copy(desc, &copy))
		goto nop_resv;

	sddesc = kzalloc(sizeof(*sddesc), GFP_NOWAIT);
	if (!sddesc)
		goto nop_resv;

	dma_async_tx_descriptor_init(&sddesc->txd, dma_chan);
	sddesc->txd.flags = flags;
	sddesc->txd.tx_submit = sdxi_tx_submit;
	sddesc->txd.desc_free = sdxi_tx_desc_free;

	/* FIXME: fill out sddesc, txd */

	return &sddesc->txd;

nop_resv:
	/*
	 * We've already advanced the write index, so we have to put
	 * valid descriptors in the reserved slots for the
	 * implementation to execute. Use nops, and proceed to ring
	 * the doorbell so they'll get consumed.
	 */
	sdxi_ring_resv_foreach(&resv, desc) {
		sdxi_serialize_nop(desc);
		sdxi_desc_make_valid(desc);
	}

	/* FIXME: ring doorbell */

	return NULL;
}

static void sdxi_dma_issue_pending(struct dma_chan *dma_chan)
{
	/* Flip the vl bits in all pending hwdescs; ring the doorbell. */
	BUG();
}

static int sdxi_dma_terminate_all(struct dma_chan *dma_chan)
{
	/* FIXME: do we really want to stop the context? */
	return sdxi_cxt_initiate_stop(to_sdxi_dma_chan(dma_chan)->cxt);
}

static void sdxi_dma_synchronize(struct dma_chan *c)
{
	BUG();
	/* Submit a nop with fe=1 and poll for completion. Won't work if terminate_all stopped the context though. */
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

int sdxi_dma_register(struct sdxi_dev *sdxi)
{
	struct sdxi_dma_chan *sdchan;
	struct device *dev = sdxi_to_dev(sdxi);
	struct dma_device *dma_dev = &sdxi->dma_dev;
	int ret = 0;

	sdxi->dma_cxt = start_dma_cxt(sdxi);
	if (!sdxi->dma_cxt)
		return -ENOMEM;

	sdxi->sdxi_dma_chan = devm_kzalloc(dev, sizeof(*sdxi->sdxi_dma_chan),
					   GFP_KERNEL);
	if (!sdxi->sdxi_dma_chan)
		return -ENOMEM;

	sdxi->sdxi_dma_chan->cxt = sdxi->dma_cxt;

	dma_dev->dev = dev;
	dma_dev->src_addr_widths = DMA_SLAVE_BUSWIDTH_64_BYTES;
	dma_dev->dst_addr_widths = DMA_SLAVE_BUSWIDTH_64_BYTES;
	dma_dev->directions = BIT(DMA_MEM_TO_MEM);
	dma_dev->residue_granularity = DMA_RESIDUE_GRANULARITY_DESCRIPTOR;
	dma_cap_set(DMA_MEMCPY, dma_dev->cap_mask);

	dma_cap_set(DMA_PRIVATE, dma_dev->cap_mask);

	INIT_LIST_HEAD(&dma_dev->channels);
	/* FIXME add a channel */

	sdchan = sdxi->sdxi_dma_chan;
	sdchan->dma_chan.device = dma_dev;
	dma_cookie_init(&sdchan->dma_chan);

	list_add_tail(&sdchan->dma_chan.device_node, &dma_dev->channels);

	/* Set base and prep routines */
	dma_dev->device_free_chan_resources = sdxi_dma_free_chan_resources;
	dma_dev->device_prep_dma_memcpy = sdxi_dma_prep_memcpy;
	dma_dev->device_issue_pending = sdxi_dma_issue_pending;
	dma_dev->device_tx_status = dma_cookie_status;
	dma_dev->device_terminate_all = sdxi_dma_terminate_all;
	dma_dev->device_synchronize = sdxi_dma_synchronize;

	dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));

	ret = dma_async_device_register(dma_dev);
	if (ret)
		goto err_reg;

	return 0;

err_reg:
	return ret;
}

void sdxi_dma_unregister(struct sdxi_dev *sdxi)
{
	if (sdxi->dma_cxt)
		sdxi_working_cxt_exit(sdxi->dma_cxt);
	dma_async_device_unregister(&sdxi->dma_dev);
}
