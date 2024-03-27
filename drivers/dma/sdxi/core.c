/*
 * SDXI hardware device driver
 *
 * Copyright (C) 2022 AMD, Inc. All rights reserved.
 *
 * Author: Wei Huang <wei.huang2@amd.com>
 */

#define dev_fmt(fmt)    "SDXI: " fmt

#include <linux/module.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <asm/mmu.h>
#include <linux/ptrace.h>

#include "sdxi.h"
#include "pci.h"
#include "sq.h"
#include "process.h"

#define CREATE_TRACE_POINTS
#include "trace.h"

static bool dma_engine;
module_param(dma_engine, bool, 0644);
MODULE_PARM_DESC(dma_engine, "Enable DMA engine interface (default: false)");

static void set_ctxt_l2_entry(struct sdxi_dev *sdxi,
			      struct ctxt_l2_entry *l2_entry,
			      struct ctxt_l1_entry *l1_table)
{
	struct device *dev = &sdxi->pdev->dev;
	dma_addr_t l1_addr;

	if (l1_table) {
		/* already set, nothing to do. NB: maybe do checking */
		if (l2_entry->vl)
			return;

		l1_addr = dma_map_single(dev, l1_table, L1_TABLE_SIZE,
					 DMA_TO_DEVICE);
		if (dma_mapping_error(dev, l1_addr)) {
			dev_err(dev, "dma_map_single for L1 table failed\n");
			return;
		}

		l2_entry->l1_ptr = l1_addr >> L2_CTXT_L1_BASE_SHIFT;
		l2_entry->vl = 1;
	} else {
		memset(l2_entry, 0, sizeof(*l2_entry));
	}
}

static void set_ctxt_l1_entry(struct sdxi_dev *sdxi,
			      struct ctxt_l1_entry *l1_entry,
			      struct sdxi_ctxt *ctxt)
{
	struct device *dev = &sdxi->pdev->dev;

	if (ctxt) {
		/* NB: More need to be done */
		ctxt->cce_addr = dma_map_single(dev, &ctxt->cce,
						sizeof(struct ctxt_ctrl_entry),
						DMA_TO_DEVICE);
		if (dma_mapping_error(dev, ctxt->cce_addr)) {
			dev_err(dev, "dma_map for ctxt ctrl addr failed\n");
			return;
		}

		/* akey handling */
		ctxt->akey_addr = dma_map_single(dev, ctxt->akey,
						 ctxt->akey_entries * sizeof(struct akey_entry),
						 DMA_TO_DEVICE);
		if (dma_mapping_error(dev, ctxt->akey_addr))  {
			dev_err(dev, "dma_map for akey table failed\n");
			dma_unmap_single(dev, ctxt->cce_addr, sizeof(struct ctxt_ctrl_entry),
					 DMA_TO_DEVICE);
			return;
		}

		l1_entry->ctxt_ctrl_ptr = ctxt->cce_addr >> L1_CTXT_CTRL_PTR_SHIFT;
		l1_entry->akey_tbl_ptr = ctxt->akey_addr >> L1_CTXT_AKEY_PTR_SHIFT;
		l1_entry->akey_tbl_size = (ctxt->akey_entries * sizeof(struct akey_entry) >> 12) - 1;
		l1_entry->opb_000_enb = sdxi->op_grp_cap;
		l1_entry->vl = 1;
		l1_entry->ka = 1;

		ctxt->akey[0].vl = 1;
	} else {
		memset(l1_entry, 0, sizeof(*l1_entry));
	}
}

static void config_ctxt_table_entries(struct ctxt_l2_entry *l2_table,
				      struct ctxt_l1_entry *l1_table,
				      struct sdxi_ctxt *ctxt,
				      bool clear)
{
	u16 id;
	struct ctxt_l2_entry *l2_entry;
	struct ctxt_l1_entry *l1_entry;
	struct sdxi_dev *sdxi = ctxt->sdxi;

	if (!ctxt || !l1_table || !l2_table)
		return;

	id = ctxt->id;
	l2_entry = l2_table + ID_TO_L2_INDEX(id);
	l1_entry = l1_table + ID_TO_L1_INDEX(id);

	if (!clear) {
		set_ctxt_l2_entry(sdxi, l2_entry, l1_table);
		set_ctxt_l1_entry(sdxi, l1_entry, ctxt);
	} else {
		memset(l1_entry, 0, sizeof(*l1_entry));
	}
}

static int config_ctxt_tables(struct sdxi_dev *sdxi,
			      struct sdxi_ctxt *ctxt)
{
	u16 id, l2_idx, l1_idx;
	struct ctxt_l2_entry *l2_table = sdxi->l2_table;
	struct ctxt_l1_entry *l1_table;

	if (!ctxt)
		return -EINVAL;

	id = ctxt->id;
	l2_idx = ID_TO_L2_INDEX(id);
	l1_idx = ID_TO_L1_INDEX(id);

	/* allocate l1 table if needed */
	l1_table = sdxi->l1_table_array[l2_idx];
	if (!l1_table) {
		gfp_t gfp_flags;
		unsigned long order;

		gfp_flags = GFP_KERNEL | __GFP_ZERO;
		order = get_order(L1_TABLE_SIZE);
		l1_table = (struct ctxt_l1_entry *)__get_free_pages(gfp_flags,
								    order);

		if (!l1_table)
			return -ENOMEM;

		sdxi->l1_table_array[l2_idx] = l1_table;
	}

	/* configure l2 and l1 entries */
	config_ctxt_table_entries(l2_table, l1_table, ctxt, false);

	return 0;
}

static void cleanup_ctxt_tables(struct sdxi_dev *sdxi,
				struct sdxi_ctxt *ctxt)
{
	u16 id, l2_idx, l1_idx;
	struct ctxt_l2_entry *l2_table = sdxi->l2_table;
	struct ctxt_l1_entry *l1_table;

	if (!ctxt)
		return;

	id = ctxt->id;
	l2_idx = ID_TO_L2_INDEX(id);
	l1_idx = ID_TO_L1_INDEX(id);

	l1_table = sdxi->l1_table_array[l2_idx];
	/* clear l1 entry */
	config_ctxt_table_entries(l2_table, l1_table, ctxt, true);
}

static struct sdxi_ctxt *alloc_ctxt(struct sdxi_dev *sdxi)
{
	struct sdxi_ctxt *ctxt;
	u16 id, l2_idx, l1_idx;
	struct akey_entry *akey;
	int entries = DEFAULT_AKEY_NUM;

	if (sdxi->ctxt_count >= sdxi->max_cxts)
		return NULL;

	if (entries > sdxi->max_akeys)
		return NULL;

	/* search for an empty context slot */
	for (id = 0; id < sdxi->max_cxts; id++) {
		l2_idx = ID_TO_L2_INDEX(id);
		l1_idx = ID_TO_L1_INDEX(id);

		if (sdxi->ctxt_array[l2_idx] == NULL) {
			int sz = sizeof(struct sdxi_ctxt *) * L1_TABLE_ENTRIES;
			struct sdxi_ctxt **ptr = kzalloc(sz, GFP_KERNEL);

			sdxi->ctxt_array[l2_idx] = ptr;
			if (!(sdxi->ctxt_array[l2_idx]))
				return NULL;
		}

		ctxt = (sdxi->ctxt_array)[l2_idx][l1_idx];
		/* found one empty slot */
		if (!ctxt)
			break;
	}

	/* nothing found, bail... */
	if (id == sdxi->max_cxts)
		return NULL;

	/* alloc context and initialize it */
	ctxt = kzalloc(sizeof(struct sdxi_ctxt), GFP_KERNEL);
	if (!ctxt)
		return NULL;

	akey = kzalloc(entries * sizeof(struct akey_entry), GFP_KERNEL);
	if (!akey) {
		kfree(ctxt);
		return NULL;
	}

	INIT_LIST_HEAD(&ctxt->list);
	ctxt->sdxi = sdxi;
	ctxt->id = id;
	ctxt->akey_entries = entries;
	ctxt->akey = akey;
	ctxt->db_base = sdxi->dbs_bar + id * sdxi->db_stride;
	ctxt->db = sdxi->dbs + id * sdxi->db_stride;

	sdxi->ctxt_array[l2_idx][l1_idx] = ctxt;
	list_add(&ctxt->list, &sdxi->ctxt_list);
	sdxi->ctxt_count++;

	return ctxt;
}

static void free_ctxt(struct sdxi_ctxt *ctxt)
{
	struct sdxi_dev *sdxi = ctxt->sdxi;

	trace_sdxi_free_ctxt(sdxi, ctxt);

	sdxi->ctxt_count--;
	list_del(&ctxt->list);
	kfree(ctxt->akey);
	kfree(ctxt);
}

/* alloc context resources and populate context table */
struct sdxi_ctxt *sdxi_ctxt_alloc(struct sdxi_dev *sdxi)
{
	struct sdxi_ctxt *ctxt = NULL;
	unsigned long flags;
	gfp_t gfp_flags;
	struct device *dev = &sdxi->pdev->dev;
	int ret;

	spin_lock_irqsave(&sdxi->ctxt_lock, flags);

	ctxt = alloc_ctxt(sdxi);
	if (!ctxt)
		goto err_out;

	gfp_flags = GFP_KERNEL | __GFP_ZERO;
	ctxt->dummy_buffer = (void *)__get_free_pages(gfp_flags, 0);
	ctxt->dummy_buffer_addr = dma_map_single(dev, ctxt->dummy_buffer, 4096,
                                                DMA_FROM_DEVICE);

       ret = config_ctxt_tables(sdxi, ctxt);
	if (ret) {
		free_ctxt(ctxt);
		ctxt = NULL;
	}

	trace_sdxi_create_ctxt(sdxi, ctxt);

err_out:
	spin_unlock_irqrestore(&sdxi->ctxt_lock, flags);
	return ctxt;
}

/* clear context table and free context resources */
void sdxi_ctxt_free(struct sdxi_ctxt *ctxt)
{
	struct sdxi_dev *sdxi = ctxt->sdxi;
	unsigned long flags;

	trace_sdxi_free_ctxt(sdxi, ctxt);

	spin_lock_irqsave(&sdxi->ctxt_lock, flags);

	cleanup_ctxt_tables(sdxi, ctxt);
	free_pages((unsigned long)ctxt->dummy_buffer, 0);
	free_ctxt(ctxt);

	spin_unlock_irqrestore(&sdxi->ctxt_lock, flags);
}

struct sdxi_ctxt *sdxi_working_ctxt_init(struct sdxi_dev *sdxi,
					 enum sdxi_ctxt_id id)
{
	struct device *dev = &sdxi->pdev->dev;
	struct sdxi_ctxt *ctxt;
	struct sdxi_sq *sq;

	ctxt = sdxi_ctxt_alloc(sdxi);
	if (!ctxt) {
		dev_err(dev, "failed to alloc a new context\n");
		return NULL;
	}

	/* check if context ID matches */
	if (id < SDXI_ANY_CTXT_ID && ctxt->id != id) {
		dev_err(dev, "failed to alloc a context with id=%d\n", id);
		goto err_ctxt_id;
	}

	sq = sdxi_sq_alloc_default(ctxt);
	if (!sq) {
		dev_err(dev, "failed to alloc a submission queue (sq)\n");
		goto err_sq_alloc;
	}

	return ctxt;

err_sq_alloc:
err_ctxt_id:
	sdxi_ctxt_free(ctxt);

	return NULL;
}

void sdxi_working_ctxt_exit(struct sdxi_ctxt *ctxt)
{
	struct sdxi_sq *sq;

	if (!ctxt)
		return;

	sq = ctxt->sq;
	if (!sq)
		return;

	sdxi_sq_free(sq);

	sdxi_ctxt_free(ctxt);
}

/* NB: This might not be the best way of doing things. We want
 * to allocate a new context for user space. However the question
 * is which sdxi_device will host it? Right now this function just
 * pick from the first in sdxi_device_list. But it certainly can
 * be improved. Also move this function to sdxi.c file.
 */
struct sdxi_ctxt *sdxi_working_ctxt_alloc(void)
{
	struct list_head *curr;
	struct sdxi_dev *sdxi;
	struct sdxi_ctxt *ctxt;
	struct sdxi_desc desc;

	if (list_empty(&sdxi_device_list))
		return NULL;

	list_for_each(curr, &sdxi_device_list) {
		sdxi = list_entry(curr, struct sdxi_dev, list);

		ctxt = sdxi_working_ctxt_init(sdxi, SDXI_ANY_CTXT_ID);
		if (!ctxt)
			return NULL;

		build_admin_start_new(&desc, 0, 0, ctxt->id, ctxt->id, 0);
		mb();
		sdxi_sq_submit_desc(sdxi->admin_ctxt->sq, &desc, false, 0);

		return ctxt;
	}

	return NULL;
}

/* Main entry point for SDXI device initial configuration */
int sdxi_device_init(struct sdxi_dev *sdxi)
{
	struct sdxi_ctxt *admin_ctxt, *dma_ctxt, *kern_ctxt;
	struct sdxi_desc desc;

	/* init admin context */
	admin_ctxt = sdxi_working_ctxt_init(sdxi, SDXI_ADMIN_CTXT_ID);
	if (!admin_ctxt)
		return -EINVAL;

	/* init DMA context */
	dma_ctxt = sdxi_working_ctxt_init(sdxi, SDXI_DMA_CTXT_ID);
	if (!dma_ctxt)
		goto err_dma_ctxt;

	/* init in kernel API context */
	kern_ctxt = sdxi_working_ctxt_init(sdxi, SDXI_KERNEL_CTXT_ID);
	if (!kern_ctxt)
		goto err_kern_ctxt;

	sdxi->admin_ctxt = admin_ctxt;
	sdxi->dma_ctxt = dma_ctxt;
	sdxi->kern_ctxt = kern_ctxt;

	build_admin_start_new(&desc, 0, 0, SDXI_DMA_CTXT_ID, SDXI_KERNEL_CTXT_ID, 0);
	sdxi_sq_submit_desc(admin_ctxt->sq, &desc, false, 0);

	/* register with DMA engine */
	if (dma_engine)
		sdxi_dma_register(sdxi->dma_ctxt);

	return 0;
err_kern_ctxt:
	sdxi_working_ctxt_exit(dma_ctxt);
err_dma_ctxt:
	sdxi_working_ctxt_exit(admin_ctxt);

	return -EINVAL;
}

void sdxi_device_exit(struct sdxi_dev *sdxi)
{
	if (dma_engine)
		sdxi_dma_unregister(sdxi->dma_ctxt);

	sdxi_working_ctxt_exit(sdxi->kern_ctxt);
	sdxi_working_ctxt_exit(sdxi->dma_ctxt);
	sdxi_working_ctxt_exit(sdxi->admin_ctxt);
}
