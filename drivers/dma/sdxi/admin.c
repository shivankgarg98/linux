#include <linux/cleanup.h>
#include <linux/kernel.h>
#include <asm/processor.h>
#include <asm/rwonce.h>

#include "admin.h"
#include "context.h"
#include "descriptor.h"
#include "ring.h"
#include "sdxi.h"

/*
 * Polled completion status block that can be attached to a
 * descriptor.
 */
struct sdxi_completion {
	struct sdxi_dev *sdxi;
	struct sdxi_cst_blk *cst_blk;
	dma_addr_t cst_blk_dma;
};

static struct sdxi_completion *sdxi_completion_alloc(struct sdxi_dev *sdxi)
{
	struct sdxi_completion *sc __free(kfree);
	struct sdxi_cst_blk *cst_blk;
	dma_addr_t cst_blk_dma;

	sc = kmalloc(sizeof(*sc), GFP_KERNEL);
	if (!sc)
		return NULL;

	/* Should use a dma_pool. */
	cst_blk = dma_alloc_coherent(sdxi_to_dev(sdxi), sizeof(*cst_blk),
				     &cst_blk_dma, GFP_KERNEL);
	if (!cst_blk)
		return NULL;

	*sc = (typeof(*sc)) {
		.sdxi        = sdxi,
		.cst_blk     = cst_blk,
		.cst_blk_dma = cst_blk_dma,
	};

	return_ptr(sc);
}

static void sdxi_completion_free(struct sdxi_completion *sc)
{
	dma_free_coherent(sdxi_to_dev(sc->sdxi), sizeof(*sc->cst_blk),
			  sc->cst_blk, sc->cst_blk_dma);
	kfree(sc);
}

DEFINE_FREE(sdxi_completion, struct sdxi_completion *, if (_T) sdxi_completion_free(_T))

static void sdxi_completion_poll(const struct sdxi_completion *sc)
{
	while (READ_ONCE(sc->cst_blk->signal) != 0)
		cpu_relax();
}

static struct sdxi_cxt *to_admin_cxt(const struct sdxi_cxt *cxt)
{
	return cxt->sdxi->admin_cxt;
}

int sdxi_adm_start_cxt(struct sdxi_cxt *cxt)
{
	struct sdxi_cxt *adm = to_admin_cxt(cxt);
	struct sdxi_desc *desc;
	struct sdxi_ring_resv resv;
	int err;

	might_sleep();

	struct sdxi_completion *sc __free(sdxi_completion) =
		sdxi_completion_alloc(cxt->sdxi);

	/* This is not how to start the admin context. */
	if (WARN_ON(adm == cxt))
		return -EINVAL;
	/*
	 * FIXME: For admin tasks, we should block until reservation
	 * succeeds.
	 */
	err = sdxi_ring_reserve(adm->ring_state, 1, &resv);
	if (err)
		return err;

	desc = sdxi_ring_resv_next(&resv);
	sdxi_encode_cxt_start(desc, &(const struct sdxi_cxt_start) {
			.range = sdxi_cxt_range(cxt->id),
		});
	sdxi_desc_set_csb(desc, sc->cst_blk_dma);
	sdxi_desc_make_valid(desc);
	sdxi_cxt_push_doorbell(adm, sdxi_ring_resv_dbval(&resv));
	sdxi_completion_poll(sc);

	return 0;
}

void sdxi_adm_stop_cxt(struct sdxi_cxt *cxt)
{
	struct sdxi_cxt *adm = to_admin_cxt(cxt);
	struct sdxi_desc *stop, *sync;
	struct sdxi_ring_resv resv;
	int err;

	might_sleep();

	struct sdxi_completion *sc __free(sdxi_completion) =
		sdxi_completion_alloc(cxt->sdxi);

	/* This is not how to stop the admin context. */
	if (WARN_ON(adm == cxt))
		return;
	/*
	 * FIXME: For admin tasks, we should block until reservation
	 * succeeds.
	 */
	err = sdxi_ring_reserve(adm->ring_state, 2, &resv);
	if (err)
		return;

	stop = sdxi_ring_resv_next(&resv);
	sync = sdxi_ring_resv_next(&resv);

	sdxi_encode_cxt_stop(stop, &(const struct sdxi_cxt_stop) {
			.range = sdxi_cxt_range(cxt->id),
		});
	sdxi_encode_sync(sync, &(const struct sdxi_sync) {
			.filter = SDXI_SYNC_FLT_STOP,
			.range = sdxi_cxt_range(cxt->id),
		});
	sdxi_desc_set_csb(sync, sc->cst_blk_dma);
	sdxi_desc_make_valid(stop);
	sdxi_desc_make_valid(sync);
	sdxi_cxt_push_doorbell(adm, sdxi_ring_resv_dbval(&resv));
	sdxi_completion_poll(sc);
}
