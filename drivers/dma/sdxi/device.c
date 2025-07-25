// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDXI hardware device driver
 *
 * Copyright (C) 2022 AMD, Inc. All rights reserved.
 *
 * Author: Wei Huang <wei.huang2@amd.com>
 */

#include <asm/mmu.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/packing.h>
#include <linux/pci.h>
#include <linux/ptrace.h>

#include "context.h"
#include "hw.h"
#include "sdxi.h"
#define CREATE_TRACE_POINTS
#include "trace.h"

static bool dma_engine = false;
module_param(dma_engine, bool, 0644);
MODULE_PARM_DESC(dma_engine, "Enable DMA engine interface (default: false)");

static bool set_pr_bits = false;
module_param(set_pr_bits, bool, 0644);
MODULE_PARM_DESC(set_pr_bits,
		 "Set the 'pr' bits on kernel-private SDXI "
		 "control structures when the underlying bus supports privileged "
		 "address space and the function has been configured to use it "
		 "(e.g. PCIe PASID Privileged Mode) "
		 "(default: false)");

static bool force_pr_for_user_contexts = false;
module_param(force_pr_for_user_contexts, bool, 0644);
MODULE_PARM_DESC(force_pr_for_user_contexts,
		 "Force-enable the 'pr' bit for user contexts. "
		 "Not useful without set_pr_bits=1. "
		 "This is a security hole and is intended for hardware "
		 "validation only. "
		 "(default: false)");

static void set_cxt_l2_entry(struct sdxi_dev *sdxi,
			     struct sdxi_cxt_l2_ent *l2_entry,
			     struct cxt_l1_entry *l1_table)
{
	struct device *dev = sdxi_to_dev(sdxi);
	dma_addr_t l1_addr;

	if (l1_table) {
		/* already set, nothing to do. NB: maybe do checking */
		if (sdxi_cxt_l2_ent_vl(l2_entry))
			return;

		l1_addr = dma_map_single(dev, l1_table, L1_TABLE_SIZE,
					 DMA_TO_DEVICE);
		if (dma_mapping_error(dev, l1_addr)) {
			sdxi_err(sdxi, "dma_map_single for L1 table failed\n");
			return;
		}

		sdxi_cxt_l2_ent_set(l2_entry, l1_addr, true);
	} else {
		memset(l2_entry, 0, sizeof(*l2_entry));
	}
}

static void set_cxt_l1_entry(struct sdxi_dev *sdxi,
			     struct cxt_l1_entry *l1_entry,
			     struct sdxi_cxt *cxt)
{
	struct device *dev = sdxi_to_dev(sdxi);

	if (cxt) {
		/* NB: More need to be done */
		cxt->cce_addr = dma_map_single(dev, &cxt->cce, sizeof(cxt->cce),
					       DMA_TO_DEVICE);
		if (dma_mapping_error(dev, cxt->cce_addr)) {
			sdxi_err(sdxi, "dma_map for cxt ctrl addr failed\n");
			return;
		}

		/* akey handling */
		cxt->akey_addr = dma_map_single(dev, cxt->akey,
						cxt->akey_entries * sizeof(struct akey_entry),
						DMA_TO_DEVICE);
		if (dma_mapping_error(dev, cxt->akey_addr))  {
			sdxi_err(sdxi, "dma_map for akey table failed\n");
			dma_unmap_single(dev, cxt->cce_addr, sizeof(cxt->cce),
					 DMA_TO_DEVICE);
			return;
		}

		l1_entry->cxt_ctrl_ptr = cxt->cce_addr >> L1_CXT_CTRL_PTR_SHIFT;
		l1_entry->akey_tbl_ptr = cxt->akey_addr >> L1_CXT_AKEY_PTR_SHIFT;
		l1_entry->akey_tbl_size = (cxt->akey_entries * sizeof(struct akey_entry) >> 12) - 1;
		l1_entry->opb_000_enb = sdxi->op_grp_cap;
		l1_entry->vl = 1;
		l1_entry->ka = 1;
		l1_entry->pr = sdxi->use_privileged_bits ? cxt->privileged : 0;
		l1_entry->max_buf = 11;

		cxt->akey[0].vl = 1;
	} else {
		memset(l1_entry, 0, sizeof(*l1_entry));
	}
}

static void config_cxt_table_entries(struct sdxi_cxt_l2_table *l2_table,
				     struct cxt_l1_entry *l1_table,
				     struct sdxi_cxt *cxt,
				     bool clear)
{
	u16 id;
	struct sdxi_cxt_l2_ent *l2_entry;
	struct cxt_l1_entry *l1_entry;
	struct sdxi_dev *sdxi = cxt->sdxi;

	if (!cxt || !l1_table)
		return;

	id = cxt->id;
	l2_entry = &l2_table->entry[ID_TO_L2_INDEX(id)];
	l1_entry = l1_table + ID_TO_L1_INDEX(id);

	if (!clear) {
		set_cxt_l2_entry(sdxi, l2_entry, l1_table);
		set_cxt_l1_entry(sdxi, l1_entry, cxt);
	} else {
		memset(l1_entry, 0, sizeof(*l1_entry));
		// If this table has been completely zeroed then free it and unmap it
		if (!memchr_inv(l1_table, 0, L1_TABLE_SIZE)) {
			dma_addr_t l1_dma = sdxi_cxt_l2_ent_lv01_ptr(l2_entry);

			// assumes we're freeing a single page
			static_assert(L1_TABLE_SIZE == PAGE_SIZE);
			dma_unmap_single(sdxi_to_dev(sdxi), l1_dma,
					 L1_TABLE_SIZE, DMA_TO_DEVICE);
			free_page((unsigned long)l1_table);
		}
	}
}

static int config_cxt_tables(struct sdxi_dev *sdxi,
			     struct sdxi_cxt *cxt)
{
	u16 l2_idx;
	struct cxt_l1_entry *l1_table;

	if (!cxt)
		return -EINVAL;

	l2_idx = ID_TO_L2_INDEX(cxt->id);

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
	config_cxt_table_entries(sdxi->l2_table, l1_table, cxt, false);

	return 0;
}

static void cleanup_cxt_tables(struct sdxi_dev *sdxi,
			       struct sdxi_cxt *cxt)
{
	u16 l2_idx;
	struct cxt_l1_entry *l1_table;

	if (!cxt)
		return;

	l2_idx = ID_TO_L2_INDEX(cxt->id);

	l1_table = sdxi->l1_table_array[l2_idx];
	/* clear l1 entry */
	config_cxt_table_entries(sdxi->l2_table, l1_table, cxt, true);
}

static struct sdxi_cxt *alloc_cxt(struct sdxi_dev *sdxi, bool privileged)
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
	cxt->privileged = privileged;

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
static struct sdxi_cxt *sdxi_cxt_alloc(struct sdxi_dev *sdxi, bool privileged)
{
	struct device *dev = sdxi_to_dev(sdxi);
	struct sdxi_cxt *cxt;

	mutex_lock(&sdxi->cxt_lock);

	cxt = alloc_cxt(sdxi, privileged);
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
	struct device *dev = sdxi_to_dev(sdxi);

	trace_sdxi_free_cxt(sdxi, cxt);

	mutex_lock(&sdxi->cxt_lock);

	cleanup_cxt_tables(sdxi, cxt);
	sdxi_cxt_release_dummy_buffer(cxt, sdxi_to_dev(sdxi));
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
	struct sdxi_cxt *cxt;
	struct sdxi_sq *sq;
	bool privileged;

	switch (id) {
	case SDXI_ANY_CXT_ID: // User context
		privileged = false;
		if (force_pr_for_user_contexts)
			privileged = true;
		break;
	default: // kernel context
		privileged = true;
		break;
	}

	cxt = sdxi_cxt_alloc(sdxi, privileged);
	if (!cxt) {
		sdxi_err(sdxi, "failed to alloc a new context\n");
		return NULL;
	}

	/* check if context ID matches */
	if (id < SDXI_ANY_CXT_ID && cxt->id != id) {
		sdxi_err(sdxi, "failed to alloc a context with id=%d\n", id);
		goto err_cxt_id;
	}

	sq = sdxi_sq_alloc_default(cxt);
	if (!sq) {
		sdxi_err(sdxi, "failed to alloc a submission queue (sq)\n");
		goto err_sq_alloc;
	}

	return cxt;

err_sq_alloc:
err_cxt_id:
	sdxi_cxt_free(cxt);

	return NULL;
}

static void sdxi_cxt_shutdown(struct sdxi_cxt *target_cxt)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(1000);
	struct sdxi_cxt *admin_cxt = target_cxt->sdxi->admin_cxt;
	struct sdxi_dev *sdxi = target_cxt->sdxi;
	struct sdxi_cxt_sts *sts = target_cxt->sq->cxt_status;
	struct sdxi_desc desc;
	u16 cxtid = target_cxt->id;
	cxt_sts_state_t state = sdxi_cxt_sts_state(sts);

	sdxi_dbg(sdxi, "%s entry: context state: %s",
		 __func__, cxt_sts_state_str(state));

	build_admin_stop_new(&desc, 0, 0, cxtid, cxtid, 0);
	mb();
	sdxi_sq_submit_desc(admin_cxt->sq, &desc, false, 0);

	sdxi_dbg(sdxi, "shutting down context %u\n", cxtid);

	do {
		cxt_sts_state_t state = sdxi_cxt_sts_state(sts);

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
			break;
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
			break;
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

typedef enum sdxi_fn_gsv {
	SDXI_GSV_STOP,
	SDXI_GSV_INIT,
	SDXI_GSV_ACTIVE,
	SDXI_GSV_STOPG_SF,
	SDXI_GSV_STOPG_HD,
	SDXI_GSV_ERROR,
} sdxi_fn_gsv_t;

static const char *const gsv_strings[] = {
	[SDXI_GSV_STOP]     = "stopped",
	[SDXI_GSV_INIT]     = "initializing",
	[SDXI_GSV_ACTIVE]   = "active",
	[SDXI_GSV_STOPG_SF] = "soft stopping",
	[SDXI_GSV_STOPG_HD] = "hard stopping",
	[SDXI_GSV_ERROR]    = "error",
};

static const char *gsv_str(sdxi_fn_gsv_t gsv)
{
	if ((size_t)gsv < ARRAY_SIZE(gsv_strings))
		return gsv_strings[(size_t)gsv];

	WARN_ONCE(1, "unexpected gsv %u\n", gsv);

	return "unknown";
}

typedef enum sdxi_fn_gsr {
	SDXI_GSRV_RESET,
	SDXI_GSRV_STOP_SF,
	SDXI_GSRV_STOP_HD,
	SDXI_GSRV_ACTIVE,
} sdxi_fn_gsr_t;

static sdxi_fn_gsv_t sdxi_dev_gsv(const struct sdxi_dev *sdxi)
{
	return (sdxi_fn_gsv_t)FIELD_GET(SDXI_MMIO_STS0_FN_GSV,
					sdxi_read64(sdxi, SDXI_MMIO_STS0));
}

static int sdxi_dev_start(struct sdxi_dev *sdxi)
{
	struct sdxi_mmio_ctl0 ctl0;
	unsigned long deadline;
	sdxi_fn_gsv_t status;

	status = sdxi_dev_gsv(sdxi);
	if (status != SDXI_GSV_STOP) {
		sdxi_err(sdxi,
			 "can't activate busy device (unexpected gsv: %s)\n",
			 gsv_str(status));
		return -EIO;
	}

	ctl0 = sdxi_get_ctl0(sdxi);
	ctl0.fn_gsr = SDXI_GSRV_ACTIVE;
	sdxi_set_ctl0(sdxi, ctl0);

	deadline = jiffies + msecs_to_jiffies(1000);
	do {
		status = sdxi_dev_gsv(sdxi);
		sdxi_dbg(sdxi, "%s: function state: %s\n", __func__, gsv_str(status));

		switch (status) {
		case SDXI_GSV_ACTIVE:
			sdxi_dbg(sdxi, "activated\n");
			return 0;
			break;
		case SDXI_GSV_ERROR:
			sdxi_err(sdxi, "went to error state\n");
			return -EIO;
			break;
		case SDXI_GSV_INIT:
		case SDXI_GSV_STOP:
			// transitional states, wait
			fsleep(1000);
			break;
		default:
			sdxi_err(sdxi, "unexpected gsv %u, giving up\n", status);
			return -EIO;
			break;
		}
	} while (time_before(jiffies, deadline));

	sdxi_err(sdxi, "activation timed out, current status %u\n",
		sdxi_dev_gsv(sdxi));
	return -ETIMEDOUT;
	
}

// Get the device to the GSV_STOP state.
static int sdxi_dev_stop(struct sdxi_dev *sdxi)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(1000);
	bool reset_issued = false;

	do {
		struct sdxi_mmio_ctl0 ctl0 = sdxi_get_ctl0(sdxi);
		sdxi_fn_gsv_t status = sdxi_dev_gsv(sdxi);

		sdxi_dbg(sdxi, "%s: function state: %s\n", __func__, gsv_str(status));

		switch (status) {
		case SDXI_GSV_ACTIVE:
			ctl0.fn_gsr = SDXI_GSRV_STOP_SF;
			sdxi_set_ctl0(sdxi, ctl0);
			break;
		case SDXI_GSV_ERROR:
			if (!reset_issued) {
				sdxi_info(sdxi,
					  "function in error state, issuing reset\n");
				ctl0.fn_gsr = SDXI_GSRV_RESET;
				sdxi_set_ctl0(sdxi, ctl0);
				reset_issued = true;
			} else {
				fsleep(1000);
			}
			break;
		case SDXI_GSV_STOP:
			return 0;
			break;
		case SDXI_GSV_INIT:
		case SDXI_GSV_STOPG_SF:
		case SDXI_GSV_STOPG_HD:
			// transitional states, wait
			sdxi_dbg(sdxi, "waiting for stop (gsv = %u)\n",
				 status);
			fsleep(1000);
			break;
		default:
			sdxi_err(sdxi, "unknown gsv %u, giving up\n", status);
			return -EIO;
			break;
		}
	} while (time_before(jiffies, deadline));

	sdxi_err(sdxi, "stop attempt timed out, current status %u\n",
		sdxi_dev_gsv(sdxi));
	return -ETIMEDOUT;
}

static void sdxi_parse_capabilities(struct sdxi_dev *sdxi)
{
	union mmio_cap1_reg cap1;
	u64 cap0;

	cap0 = sdxi_read64(sdxi, SDXI_MMIO_CAP0);
	sdxi->max_ring_entries = 1ULL << (FIELD_GET(SDXI_MMIO_CAP0_MAX_DS_RING_SZ, cap0) + 10);
	sdxi->db_stride = 1UL << (FIELD_GET(SDXI_MMIO_CAP0_DB_STRIDE, cap0) + 12);
	sdxi->sfunc = FIELD_GET(SDXI_MMIO_CAP0_SFUNC, cap0);

	/* CAP1 */
	cap1.data = sdxi_read64(sdxi, SDXI_MMIO_CAP1);

	sdxi->max_akeys = 1 << (cap1.max_akey_sz + 8);
	sdxi->max_cxts = cap1.max_cxt + 1;
	sdxi->op_grp_cap = cap1.opb_000_cap;

	dev_info(sdxi_to_dev(sdxi),
		 "sfunc:%#hx descmax:%llu dbstride:%#x akeymax:%u cxtmax:%u opgrps:%#x\n",
		 sdxi->sfunc, sdxi->max_ring_entries, sdxi->db_stride,
		 sdxi->max_akeys, sdxi->max_cxts, sdxi->op_grp_cap);
}

static void sdxi_parse_version(struct sdxi_dev *sdxi)
{
	static const struct packed_field_u8 version_fields[] = {
		PACKED_FIELD(23, 16, typeof(sdxi->sdxi_version), major),
		PACKED_FIELD(7, 0, typeof(sdxi->sdxi_version), minor),
	};
	u64 reg = sdxi_read64(sdxi, SDXI_MMIO_VER);

	unpack_fields(&reg, sizeof(reg), &sdxi->sdxi_version, version_fields,
		      QUIRK_LITTLE_ENDIAN | QUIRK_LSW32_IS_FIRST);
}

// Refer to "Activation of the SDXI Function by Software"
static int sdxi_activate(struct sdxi_dev *sdxi)
{
	struct device *dev = sdxi_to_dev(sdxi);
	u64 ctrl2;
	int err;

	if ((err = sdxi_dev_stop(sdxi)))
		return err;

	sdxi_parse_capabilities(sdxi);
	sdxi_parse_version(sdxi);

	if (sdxi_dev_supports_privileged_address_space(sdxi) && set_pr_bits) {
		struct sdxi_mmio_ctl0 ctl0 = sdxi_get_ctl0(sdxi);
		ctl0.fn_pr = true;
		sdxi_set_ctl0(sdxi, ctl0);
		sdxi->use_privileged_bits = true;
		sdxi_dbg(sdxi,
			 "Setting 'pr' bit on kernel-private control structures\n");
	}

	dev_info(dev, "SDXI %u.%u device found\n",
		 sdxi->sdxi_version.major, sdxi->sdxi_version.minor);

	/* err log */
	sdxi->err_log_num = DEFAULT_ERR_LOG_NUM;
	sdxi->err_log = dma_alloc_coherent(dev, sdxi->err_log_num * sizeof(sdxi->err_log[0]),
					   &sdxi->err_log_dma, GFP_KERNEL);
	if (!sdxi->err_log)
		return -ENOMEM;

	sdxi_write64(sdxi, SDXI_MMIO_ERR_CFG,
		     FIELD_PREP(SDXI_MMIO_ERR_CFG_PTR, sdxi->err_log_dma >> 12) |
		     FIELD_PREP(SDXI_MMIO_ERR_CFG_SZ, sdxi->err_log_num >> 6) |
		     FIELD_PREP(SDXI_MMIO_ERR_CFG_EN, 1));

	/* Signal interrupt on new error log entry */
	sdxi_write64(sdxi, SDXI_MMIO_ERR_CTL,
		     FIELD_PREP(SDXI_MMIO_ERR_CTL_EN, 1));

	// FIXME: clean this up
	ctrl2 = sdxi_read64(sdxi, SDXI_MMIO_CTL2);
	ctrl2 &= 0xFFFFFFFF0000FFFFULL;
	ctrl2 |= (sdxi->max_cxts << 16) & 0x00000000FFFF0000ULL;
	ctrl2 &= 0x00000000FFFFFFFFULL;
	ctrl2 |= (uint64_t)sdxi->op_grp_cap << 32;
	sdxi_write64(sdxi, SDXI_MMIO_CTL2, ctrl2);

	sdxi_write64(sdxi, SDXI_MMIO_CXT_L2,
		     FIELD_PREP(SDXI_MMIO_CXT_L2_PTR, sdxi->l2_dma >> 12));

	err = sdxi_dev_start(sdxi);
	if (err)
		goto unmap_errlog;

	return 0;
unmap_errlog:
	dma_free_coherent(dev, sdxi->err_log_num * sizeof(sdxi->err_log[0]),
			  sdxi->err_log, sdxi->err_log_dma);
	return -ENOMEM;
}

static void sdxi_stop(struct sdxi_dev *sdxi)
{
	struct device *dev = sdxi_to_dev(sdxi);

	sdxi_dev_stop(sdxi);

	dma_free_coherent(dev, sdxi->err_log_num * sizeof(sdxi->err_log[0]),
			  sdxi->err_log, sdxi->err_log_dma);
}

static void init_ctrl_regs(struct sdxi_dev *sdxi)
{
	// Don't assume that the control registers have their defined
	// reset values; set them explicitly. Updating the registers
	// caches the committed values in sdxi_dev.
	sdxi_set_ctl0(sdxi, (struct sdxi_mmio_ctl0){});
}

/* Main entry point for SDXI device initial configuration */
int sdxi_device_init(struct sdxi_dev *sdxi, const struct sdxi_dev_ops *ops)
{
	struct sdxi_cxt *admin_cxt, *dma_cxt;
	struct sdxi_desc desc;
	int err;

	sdxi->dev_ops = ops;

	init_ctrl_regs(sdxi);

	sdxi->l2_table = dmam_alloc_coherent(sdxi_to_dev(sdxi), L2_TABLE_SIZE,
					     &sdxi->l2_dma, GFP_KERNEL);
	if (!sdxi->l2_table)
		return -ENOMEM;

	err = sdxi_activate(sdxi);
	if (err)
		return err;

	err = (ops && ops->irq_init) ? ops->irq_init(sdxi) : 0;
	if (err)
		goto pci_disable;

	/* init admin context */
	admin_cxt = sdxi_working_cxt_init(sdxi, SDXI_ADMIN_CXT_ID);
	if (!admin_cxt) {
		err = -EINVAL;
		goto irq_exit;
	}

	/* init DMA context */
	dma_cxt = sdxi_working_cxt_init(sdxi, SDXI_DMA_CXT_ID);
	if (!dma_cxt) {
		err = -EINVAL;
		goto admin_cxt_exit;
	}

	sdxi->admin_cxt = admin_cxt;
	sdxi->dma_cxt = dma_cxt;

	build_admin_start_new(&desc, 0, 0, SDXI_DMA_CXT_ID, SDXI_DMA_CXT_ID, 0);
	sdxi_sq_submit_desc(admin_cxt->sq, &desc, false, 0);

	/* register with DMA engine */
	if (dma_engine)
		sdxi_dma_register(sdxi->dma_cxt);

	return 0;
admin_cxt_exit:
	sdxi_working_cxt_exit(admin_cxt);
irq_exit:
	if (ops && ops->irq_exit)
		ops->irq_exit(sdxi);
pci_disable:
	sdxi_stop(sdxi);

	return err;
}

void sdxi_device_exit(struct sdxi_dev *sdxi)
{
	if (dma_engine)
		sdxi_dma_unregister(sdxi->dma_cxt);

	sdxi_working_cxt_exit(sdxi->dma_cxt);

	// Walk sdxi->cxt_array freeing any allocated rows.
	for (size_t i = 0; i < L2_TABLE_ENTRIES; ++i) {
		if (!sdxi->cxt_array[i])
			continue;
		// When a context is released its entry in the table should be NULL.
		for (size_t j = 0; j < L1_TABLE_ENTRIES; ++j) {
			struct sdxi_cxt *cxt = sdxi->cxt_array[i][j];
			if (!cxt)
				continue;
			if (cxt->id != 0) // admin context shutdown is last
				sdxi_working_cxt_exit(cxt);
			sdxi->cxt_array[i][j] = NULL;
		}
		if (i != 0) // another special case for admin cxt
			kfree(sdxi->cxt_array[i]);
	}

	sdxi_working_cxt_exit(sdxi->admin_cxt);
	kfree(sdxi->cxt_array[0]); // ugh

	if (sdxi->dev_ops && sdxi->dev_ops->irq_exit)
		sdxi->dev_ops->irq_exit(sdxi);
	sdxi_stop(sdxi);
}
