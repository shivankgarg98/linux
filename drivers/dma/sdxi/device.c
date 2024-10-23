// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDXI hardware device driver
 *
 * Copyright (C) 2022 AMD, Inc. All rights reserved.
 *
 * Author: Wei Huang <wei.huang2@amd.com>
 */

#define dev_fmt(fmt)    "SDXI: " fmt

#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <asm/mmu.h>
#include <linux/ptrace.h>

#include "sdxi.h"
#include "pci.h"
#include "context.h"
#include "process.h"

#define CREATE_TRACE_POINTS
#include "trace.h"

static void set_cxt_l2_entry(struct sdxi_dev *sdxi,
			     struct cxt_l2_entry *l2_entry,
			     struct cxt_l1_entry *l1_table)
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

		l2_entry->l1_ptr = l1_addr >> L2_CXT_L1_BASE_SHIFT;
		l2_entry->vl = 1;
	} else {
		memset(l2_entry, 0, sizeof(*l2_entry));
	}
}

static void set_cxt_l1_entry(struct sdxi_dev *sdxi,
			     struct cxt_l1_entry *l1_entry,
			     struct sdxi_cxt *cxt)
{
	struct device *dev = &sdxi->pdev->dev;

	if (cxt) {
		/* NB: More need to be done */
		cxt->cce_addr = dma_map_single(dev, &cxt->cce,
					       sizeof(struct cxt_ctrl_entry),
					       DMA_TO_DEVICE);
		if (dma_mapping_error(dev, cxt->cce_addr)) {
			dev_err(dev, "dma_map for cxt ctrl addr failed\n");
			return;
		}

		/* akey handling */
		cxt->akey_addr = dma_map_single(dev, cxt->akey,
						cxt->akey_entries * sizeof(struct akey_entry),
						DMA_TO_DEVICE);
		if (dma_mapping_error(dev, cxt->akey_addr))  {
			dev_err(dev, "dma_map for akey table failed\n");
			dma_unmap_single(dev, cxt->cce_addr, sizeof(struct cxt_ctrl_entry),
					 DMA_TO_DEVICE);
			return;
		}

		l1_entry->cxt_ctrl_ptr = cxt->cce_addr >> L1_CXT_CTRL_PTR_SHIFT;
		l1_entry->akey_tbl_ptr = cxt->akey_addr >> L1_CXT_AKEY_PTR_SHIFT;
		l1_entry->akey_tbl_size = (cxt->akey_entries * sizeof(struct akey_entry) >> 12) - 1;
		l1_entry->opb_000_enb = sdxi->op_grp_cap;
		l1_entry->vl = 1;
		l1_entry->ka = 1;
		l1_entry->max_buf = 11;

		cxt->akey[0].vl = 1;
	} else {
		memset(l1_entry, 0, sizeof(*l1_entry));
	}
}

static void config_cxt_table_entries(struct cxt_l2_entry *l2_table,
				     struct cxt_l1_entry *l1_table,
				     struct sdxi_cxt *cxt,
				     bool clear)
{
	u16 id;
	struct cxt_l2_entry *l2_entry;
	struct cxt_l1_entry *l1_entry;
	struct sdxi_dev *sdxi = cxt->sdxi;

	if (!cxt || !l1_table || !l2_table)
		return;

	id = cxt->id;
	l2_entry = l2_table + ID_TO_L2_INDEX(id);
	l1_entry = l1_table + ID_TO_L1_INDEX(id);

	if (!clear) {
		set_cxt_l2_entry(sdxi, l2_entry, l1_table);
		set_cxt_l1_entry(sdxi, l1_entry, cxt);
	} else {
		memset(l1_entry, 0, sizeof(*l1_entry));
		// If this table has been completely zeroed then free it and unmap it
		if (!memchr_inv(l1_table, 0, L1_TABLE_SIZE)) {
			dma_addr_t l1_dma = l2_entry->l1_ptr << L2_CXT_L1_BASE_SHIFT;

			// assumes we're freeing a single page
			static_assert(L1_TABLE_SIZE == PAGE_SIZE);
			dma_unmap_single(&sdxi->pdev->dev, l1_dma,
					 L1_TABLE_SIZE, DMA_TO_DEVICE);
			free_page((unsigned long)l1_table);
		}
	}
}

static int config_cxt_tables(struct sdxi_dev *sdxi,
			     struct sdxi_cxt *cxt)
{
	u16 id, l2_idx, l1_idx;
	struct cxt_l2_entry *l2_table = sdxi->l2_table;
	struct cxt_l1_entry *l1_table;

	if (!cxt)
		return -EINVAL;

	id = cxt->id;
	l2_idx = ID_TO_L2_INDEX(id);
	l1_idx = ID_TO_L1_INDEX(id);

	/* allocate l1 table if needed */
	l1_table = sdxi->l1_table_array[l2_idx];
	if (!l1_table) {
		gfp_t gfp_flags;
		unsigned long order;

		gfp_flags = GFP_KERNEL | __GFP_ZERO;
		order = get_order(L1_TABLE_SIZE);
		l1_table = (struct cxt_l1_entry *)__get_free_pages(gfp_flags,
								   order);

		if (!l1_table)
			return -ENOMEM;

		sdxi->l1_table_array[l2_idx] = l1_table;
	}

	/* configure l2 and l1 entries */
	config_cxt_table_entries(l2_table, l1_table, cxt, false);

	return 0;
}

static void cleanup_cxt_tables(struct sdxi_dev *sdxi,
			       struct sdxi_cxt *cxt)
{
	u16 id, l2_idx, l1_idx;
	struct cxt_l2_entry *l2_table = sdxi->l2_table;
	struct cxt_l1_entry *l1_table;

	if (!cxt)
		return;

	id = cxt->id;
	l2_idx = ID_TO_L2_INDEX(id);
	l1_idx = ID_TO_L1_INDEX(id);

	l1_table = sdxi->l1_table_array[l2_idx];
	/* clear l1 entry */
	config_cxt_table_entries(l2_table, l1_table, cxt, true);
}

static struct sdxi_cxt *alloc_cxt(struct sdxi_dev *sdxi)
{
	struct sdxi_cxt *cxt;
	u16 id, l2_idx, l1_idx;
	struct akey_entry *akey;
	int entries = DEFAULT_AKEY_NUM;

	if (sdxi->cxt_count >= sdxi->max_cxts)
		return NULL;

	if (entries > sdxi->max_akeys)
		return NULL;

	/* search for an empty context slot */
	for (id = 0; id < sdxi->max_cxts; id++) {
		l2_idx = ID_TO_L2_INDEX(id);
		l1_idx = ID_TO_L1_INDEX(id);

		if (sdxi->cxt_array[l2_idx] == NULL) {
			int sz = sizeof(struct sdxi_cxt *) * L1_TABLE_ENTRIES;
			struct sdxi_cxt **ptr = kzalloc(sz, GFP_KERNEL);

			sdxi->cxt_array[l2_idx] = ptr;
			if (!(sdxi->cxt_array[l2_idx]))
				return NULL;
		}

		cxt = (sdxi->cxt_array)[l2_idx][l1_idx];
		/* found one empty slot */
		if (!cxt)
			break;
	}

	/* nothing found, bail... */
	if (id == sdxi->max_cxts)
		return NULL;

	/* alloc context and initialize it */
	cxt = kzalloc(sizeof(struct sdxi_cxt), GFP_KERNEL);
	if (!cxt)
		return NULL;

	akey = kcalloc(entries, sizeof(struct akey_entry), GFP_KERNEL);
	if (!akey) {
		kfree(cxt);
		return NULL;
	}

	INIT_LIST_HEAD(&cxt->list);
	cxt->sdxi = sdxi;
	cxt->id = id;
	cxt->akey_entries = entries;
	cxt->akey = akey;
	cxt->db_base = sdxi->dbs_bar + id * sdxi->db_stride;
	cxt->db = sdxi->dbs + id * sdxi->db_stride;

	sdxi->cxt_array[l2_idx][l1_idx] = cxt;
	list_add(&cxt->list, &sdxi->cxt_list);
	sdxi->cxt_count++;

	return cxt;
}

static void free_cxt(struct sdxi_cxt *cxt)
{
	struct sdxi_dev *sdxi = cxt->sdxi;
	u16 l2_idx, l1_idx;

	l2_idx = ID_TO_L2_INDEX(cxt->id);
	l1_idx = ID_TO_L1_INDEX(cxt->id);

	sdxi->cxt_count--;
	list_del(&cxt->list);
	kfree(cxt->akey);
	kfree(cxt);

	(sdxi->cxt_array)[l2_idx][l1_idx] = NULL;
}

static int sdxi_cxt_setup_dummy_buffer(struct sdxi_cxt *cxt, struct device *dev)
{
	unsigned long *buf;
	dma_addr_t addr;

	buf = (unsigned long *)get_zeroed_page(GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	addr = dma_map_single(dev, buf, PAGE_SIZE, DMA_FROM_DEVICE);
	if (dma_mapping_error(dev, addr))
		goto free_buf;

	cxt->dummy_buffer = buf;
	cxt->dummy_buffer_addr = addr;
	return 0;

free_buf:
	free_page((unsigned long)buf);
	return -ENOMEM;
}

static void sdxi_cxt_release_dummy_buffer(struct sdxi_cxt *cxt, struct device *dev)
{
	dma_unmap_single(dev, cxt->dummy_buffer_addr, PAGE_SIZE, DMA_FROM_DEVICE);
	free_page((unsigned long)cxt->dummy_buffer);
}

/* alloc context resources and populate context table */
struct sdxi_cxt *sdxi_cxt_alloc(struct sdxi_dev *sdxi)
{
	struct device *dev = &sdxi->pdev->dev;
	struct sdxi_cxt *cxt;

	mutex_lock(&sdxi->cxt_lock);

	cxt = alloc_cxt(sdxi);
	if (!cxt)
		goto drop_cxt_lock;

	if (sdxi_cxt_setup_dummy_buffer(cxt, dev))
		goto release_cxt;

	if (config_cxt_tables(sdxi, cxt))
		goto release_dummy;

	trace_sdxi_create_cxt(sdxi, cxt);
	mutex_unlock(&sdxi->cxt_lock);
	return cxt;

release_dummy:
	sdxi_cxt_release_dummy_buffer(cxt, dev);
release_cxt:
	free_cxt(cxt);
drop_cxt_lock:
	mutex_unlock(&sdxi->cxt_lock);
	return NULL;
}

/* clear context table and free context resources */
void sdxi_cxt_free(struct sdxi_cxt *cxt)
{
	struct sdxi_dev *sdxi = cxt->sdxi;
	struct device *dev = &sdxi->pdev->dev;

	trace_sdxi_free_cxt(sdxi, cxt);

	mutex_lock(&sdxi->cxt_lock);

	cleanup_cxt_tables(sdxi, cxt);
	sdxi_cxt_release_dummy_buffer(cxt, &sdxi->pdev->dev);
	dma_unmap_single(dev, cxt->cce_addr, sizeof(cxt->cce), DMA_TO_DEVICE);
	dma_unmap_single(dev, cxt->akey_addr,
			 cxt->akey_entries * sizeof(struct akey_entry),
			 DMA_TO_DEVICE);
	free_cxt(cxt);

	mutex_unlock(&sdxi->cxt_lock);
}

struct sdxi_cxt *sdxi_working_cxt_init(struct sdxi_dev *sdxi,
				       enum sdxi_cxt_id id)
{
	struct device *dev = &sdxi->pdev->dev;
	struct sdxi_cxt *cxt;
	struct sdxi_sq *sq;

	cxt = sdxi_cxt_alloc(sdxi);
	if (!cxt) {
		dev_err(dev, "failed to alloc a new context\n");
		return NULL;
	}

	/* check if context ID matches */
	if (id < SDXI_ANY_CXT_ID && cxt->id != id) {
		dev_err(dev, "failed to alloc a context with id=%d\n", id);
		goto err_cxt_id;
	}

	sq = sdxi_sq_alloc_default(cxt);
	if (!sq) {
		dev_err(dev, "failed to alloc a submission queue (sq)\n");
		goto err_sq_alloc;
	}

	return cxt;

err_sq_alloc:
err_cxt_id:
	sdxi_cxt_free(cxt);

	return NULL;
}

void sdxi_working_cxt_exit(struct sdxi_cxt *cxt)
{
	struct sdxi_sq *sq;

	if (!cxt)
		return;

	sq = cxt->sq;
	if (!sq)
		return;

	sdxi_sq_free(sq);

	sdxi_cxt_free(cxt);
}

/* NB: This might not be the best way of doing things. We want
 * to allocate a new context for user space. However the question
 * is which sdxi_device will host it? Right now this function just
 * pick from the first in sdxi_device_list. But it certainly can
 * be improved. Also move this function to sdxi.c file.
 */
struct sdxi_cxt *sdxi_working_cxt_alloc(void)
{
	struct list_head *curr;
	struct sdxi_dev *sdxi;
	struct sdxi_cxt *cxt;
	struct sdxi_desc desc;

	if (list_empty(&sdxi_device_list))
		return NULL;

	list_for_each(curr, &sdxi_device_list) {
		sdxi = list_entry(curr, struct sdxi_dev, list);

		cxt = sdxi_working_cxt_init(sdxi, SDXI_ANY_CXT_ID);
		if (!cxt)
			return NULL;

		build_admin_start_new(&desc, 0, 0, cxt->id, cxt->id, 0);
		mb();
		sdxi_sq_submit_desc(sdxi->admin_cxt->sq, &desc, false, 0);

		return cxt;
	}

	return NULL;
}

/* Main entry point for SDXI device initial configuration */
int sdxi_device_init(struct sdxi_dev *sdxi)
{
	struct sdxi_cxt *admin_cxt, *dma_cxt, *kern_cxt;
	struct sdxi_desc desc;

	/* init admin context */
	admin_cxt = sdxi_working_cxt_init(sdxi, SDXI_ADMIN_CXT_ID);
	if (!admin_cxt)
		return -EINVAL;

	/* init DMA context */
	dma_cxt = sdxi_working_cxt_init(sdxi, SDXI_DMA_CXT_ID);
	if (!dma_cxt)
		goto err_dma_cxt;

	/* init in kernel API context */
	kern_cxt = sdxi_working_cxt_init(sdxi, SDXI_KERNEL_CXT_ID);
	if (!kern_cxt)
		goto err_kern_cxt;

	sdxi->admin_cxt = admin_cxt;
	sdxi->dma_cxt = dma_cxt;
	sdxi->kern_cxt = kern_cxt;

	build_admin_start_new(&desc, 0, 0, SDXI_DMA_CXT_ID, SDXI_KERNEL_CXT_ID, 0);
	sdxi_sq_submit_desc(admin_cxt->sq, &desc, false, 0);

	sdxi_dma_register(sdxi->dma_cxt);

	return 0;
err_kern_cxt:
	sdxi_working_cxt_exit(dma_cxt);
err_dma_cxt:
	sdxi_working_cxt_exit(admin_cxt);

	return -EINVAL;
}

void sdxi_device_exit(struct sdxi_dev *sdxi)
{
	sdxi_dma_unregister(sdxi->dma_cxt);

	sdxi_working_cxt_exit(sdxi->kern_cxt);
	sdxi_working_cxt_exit(sdxi->dma_cxt);
	sdxi_working_cxt_exit(sdxi->admin_cxt);

	// Walk sdxi->cxt_array freeing any allocated rows.
	for (size_t i = 0; i < L2_TABLE_ENTRIES; ++i) {
		if (!sdxi->cxt_array[i])
			continue;
		// When a context is released its entry in the table should be NULL.
		for (size_t j = 0; j < L1_TABLE_ENTRIES; ++j) {
			struct sdxi_cxt *cxt = sdxi->cxt_array[i][j];

			WARN(cxt, "Possible context object leak %p at [%zu][%zu]; cxt_count=%d\n",
			     cxt, i, j, sdxi->cxt_count);
		}
		kfree(sdxi->cxt_array[i]);
	}
}
