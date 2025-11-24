#include <linux/slab.h>
#include <linux/dma-mapping.h>

#include "completion.h"
#include "descriptor.h"

struct sdxi_completion {
	struct sdxi_dev *sdxi;
	struct sdxi_cst_blk *cst_blk;
	dma_addr_t cst_blk_dma;
};

struct sdxi_completion *sdxi_completion_alloc(struct sdxi_dev *sdxi)
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

	cst_blk->signal = cpu_to_le64(1);

	*sc = (typeof(*sc)) {
		.sdxi        = sdxi,
		.cst_blk     = cst_blk,
		.cst_blk_dma = cst_blk_dma,
	};

	return_ptr(sc);
}

void sdxi_completion_free(struct sdxi_completion *sc)
{
	dma_free_coherent(sdxi_to_dev(sc->sdxi), sizeof(*sc->cst_blk),
			  sc->cst_blk, sc->cst_blk_dma);
	kfree(sc);
}

void sdxi_completion_poll(const struct sdxi_completion *sc)
{
	while (READ_ONCE(sc->cst_blk->signal) != 0)
		cpu_relax();
}

void sdxi_completion_attach(struct sdxi_desc *desc,
			    const struct sdxi_completion *cs)
{
	sdxi_desc_set_csb(desc, cs->cst_blk_dma);
}
