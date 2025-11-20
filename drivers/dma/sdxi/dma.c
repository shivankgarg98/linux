// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDXI DMA engine implementation
 *   Derived from ptdma code
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#include <linux/cleanup.h>
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
	struct sdxi_cxt *cxt = to_sdxi_dma_chan(dma_chan)->cxt;
	struct sdxi_desc check;
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
	if (sdxi_encode_copy(&check, &copy))
		return NULL;

	sddesc = kzalloc(sizeof(*sddesc), GFP_NOWAIT);
	if (!sddesc)
		return NULL;

	if (sdxi_ring_reserve(cxt->ring_state, 1, &sddesc->resv))
		return NULL;

	(void)sdxi_encode_copy(sdxi_ring_resv_next(&sddesc->resv), &copy);

	txd = vchan_tx_prep(to_virt_chan(dma_chan), &sddesc->vdesc, flags);
	retain_and_null_ptr(sddesc);
	return txd;
}

static void sdxi_dma_issue_pending(struct dma_chan *dma_chan)
{
	struct virt_dma_chan *vchan = to_virt_chan(dma_chan);
	struct virt_dma_desc *vdesc;
	u64 dbval = 0;

	scoped_guard(spinlock_irq, &vchan->lock) {
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

	sdxi_cxt_push_doorbell(to_sdxi_dma_chan(dma_chan)->cxt, dbval);
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
		.device_tx_status = dma_cookie_status,
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
