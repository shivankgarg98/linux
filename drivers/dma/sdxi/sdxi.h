/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * SDXI device driver header
 *
 * Copyright (C) 2022 AMD, Inc. All rights reserved.
 *
 * Author: Wei Huang <wei.huang2@amd.com>
 */

#ifndef __SDXI_H
#define __SDXI_H

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/idr.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/types.h>

#include "../virt-dma.h"
#include "hw.h"
#include "mmio.h"

#define SDXI_DRV_NAME		"sdxi"
#define SDXI_DRV_DESC		"SDXI driver"

/***************************/
/*        DEFAULTS         */
/***************************/
#define DEFAULT_ERR_LOG_NUM	64
#define DEFAULT_RKEY_NUM	256
#define DEFAULT_AKEY_NUM	256

/***************************/
/*          MACROS         */
/***************************/
extern struct list_head sdxi_device_list;
#define for_each_sdxi(sdxi)					\
	list_for_each_entry((sdxi), &sdxi_device_list, list)
#define for_each_sdxi_safe(sdxi, next)					\
	list_for_each_entry_safe((sdxi), (next), &sdxi_device_list, list)

#define ID_TO_L2_INDEX(id)	(((id) >> 9) & 0x1FF)
#define ID_TO_L1_INDEX(id)	((id) & 0x7F)
#define IS_VF_DEVICE(sdxi)	((sdxi)->is_vf)

/***************************/
/*          CONSTS         */
/***************************/
#define L2_TABLE_ENTRIES	(1 << 9)
#define L1_TABLE_ENTRIES	(1 << 7)
#define L2_TABLE_SIZE		4096
#define L1_TABLE_SIZE		4096

#define OP_TYPE_DMA             0x001
#define OP_TYPE_ADMIN           0x002
#define OP_TYPE_ATOMIC          0x003
#define OP_TYPE_INTR            0x004

#define OP_DMA_NOP		0x01
#define OP_DMA_WRT_IMM		0x02
#define OP_DMA_COPY		0x03
#define OP_DMA_REP_COPY		0x04
#define OP_ADMIN_UPDATE_FUNC	0x00
#define OP_ADMIN_UPDATE_CXT	0x01
#define OP_ADMIN_UPDATE_AKEY	0x02
#define OP_ADMIN_START		0x03
#define OP_ADMIN_STOP		0x04
#define OP_ADMIN_INTR		0x05
#define OP_ADMIN_SYNC		0x06
#define OP_ATOMIC_SWAP		0x01
#define OP_ATOMIC_UADD		0x02
#define OP_ATOMIC_USUB		0x03
#define OP_ATOMIC_AND		0x05
#define OP_ATOMIC_OR		0x06
#define OP_ATOMIC_XOR		0x07
#define OP_ATOMIC_SMIN		0x08
#define OP_ATOMIC_SMAX		0x09
#define OP_ATOMIC_UMIN		0x0A
#define OP_ATOMIC_UMAX		0x0B
#define OP_ATOMIC_UCLAMPI	0x0C
#define OP_ATOMIC_UCLAMPD	0x0D
#define OP_ATOMIC_CMPSWAP	0x0E
#define OP_INTR_INTERRUPT	0x00

#define CXT_STATE_STOPPED	0x0
#define CXT_STATE_RUNNING	0x1
#define CXT_STATE_STOPPING	0x2
#define CXT_STATE_ERR		0xF

#define DESC_RING_BASE_PTR_SHIFT	6
#define CXT_STATUS_PTR_SHIFT		4
#define WRT_INDEX_PTR_SHIFT		3

#define L2_CXT_L1_BASE_SHIFT		12

#define L1_CXT_CTRL_PTR_SHIFT		6
#define L1_CXT_AKEY_PTR_SHIFT		12

#define MAX_DMA_COPY_BYTES		(1ULL << 32)

/***************************/
/*         STRUCTS         */
/***************************/
enum sdxi_cxt_id {
	SDXI_ADMIN_CXT_ID = 0,
	SDXI_DMA_CXT_ID = 1,
	SDXI_ANY_CXT_ID,
};

struct sdxi_desc {
	u32 vl			: 1;
	u32 se			: 1;
	u32 fe			: 1;
	u32 ch			: 1;
	u32 csr			: 1;
	u32 rsvd1		: 3;
	u32 subtype		: 8;
	u32 type		: 11;
	u32 rsvd2		: 5;
	u32 body[13];
	u64 csb_ptr;
} __packed;

/* Submission Queue */
struct sdxi_sq {
	struct sdxi_cxt *cxt;		/* owner */

	u32 ring_entries;
	u32 ring_size;
	struct sdxi_desc *desc_ring;
	dma_addr_t ring_dma;

	struct sdxi_cst_blk *csb;
	size_t csb_size;
	dma_addr_t csb_dma;

	u32 write_index_size;
	u64 *write_index;
	dma_addr_t write_index_dma;

	u32 cxt_status_size;
	struct sdxi_cxt_sts *cxt_status;
	dma_addr_t cxt_status_dma;

	/* NB: define doorbell here */
};

struct sdxi_tasklet_data {
	struct sdxi_cmd *cmd;
};

struct sdxi_cmd {
	struct list_head entry;
	struct work_struct work;
	struct sdxi_cxt *cxt;
	int ret;
	size_t len;
	u64 src_addr;
	u64 dst_addr;
	u64 index;  //index at descriptor ring
	/* completion callback support */
	void (*sdxi_cmd_callback)(void *data, int err);
	void *data;
};

struct sdxi_dma_desc {
	struct virt_dma_desc vd;
	struct sdxi_cxt *cxt;
	enum dma_status status;
	bool issued_to_hw;
	struct sdxi_cmd sdxi_cmd;
};

struct sdxi_dma_chan {
	struct virt_dma_chan vc;
	struct sdxi_cxt *cxt;
};

struct akey_entry {
	u32 vl			: 1;	/* QW0 */
	u32 iv			: 1;
	u32 pv			: 1;
	u32 ste			: 1;
	u32 intr_num		: 11;
	u32 rsvd1		: 1;
	u32 tgt_sfunc		: 16;
	u32 pasid		: 20;
	u32 rsvd2		: 10;
	u32 ph			: 2;
	u32 stag		: 16;	/* QW1 */
	u32 rsvd3		: 16;
	u32 rkey		: 16;
	u32 rsvd4		: 16;
} __packed;

/* Context */
struct sdxi_cxt {
	struct list_head list;
	struct sdxi_dev *sdxi;	/* owner */
	unsigned int id;

	resource_size_t db_base;	/* doorbell MMIO base addr */
	void __iomem *db;		/* doorbell virt addr */

	struct sdxi_cxt_ctl cce __aligned(64);
	dma_addr_t cce_addr;		/* cce dma addr */

	int akey_entries;
	struct akey_entry *akey;
	dma_addr_t akey_addr;		/* akey dma addr */

	struct sdxi_sq *sq;

	/* NB: might need to move to sdxi_device? */
	struct sdxi_dma_chan sdxi_dma_chan;

	struct sdxi_process *process;	/* process reprsentation */

	/* FOR DEBUG */
	unsigned long *dummy_buffer;
	dma_addr_t dummy_buffer_addr;
};

/* RKey Table Entry */
struct rkey_ent {
	u32 vl			: 1;	/* QW0 */
	u32 iv			: 1;
	u32 pv			: 1;
	u32 ste			: 1;
	u32 intr_num		: 11;
	u32 rsvd1		: 1;
	u32 req_sfunc		: 16;
	u32 pasid		: 20;
	u32 rsvd2		: 10;
	u32 ph			: 2;
	u32 stag		: 16;	/* QW1 */
	u32 rsvd3		: 16;
	u32 rsvd4;
} __packed;

/* Error Log Entry */
struct sdxi_err {
	u32 vl			: 1;	/* QW0 */
	u32 rsvd1		: 7;
	u32 step		: 6;
	u32 rsvd2		: 2;
	u32 type		: 11;
	u32 rsvd3		: 5;
	u32 cv			: 1;
	u32 div			: 1;
	u32 bv			: 1;
	u32 rsvd4		: 1;
	u32 buf			: 3;
	u32 rsvd5		: 1;
	u32 sub_step		: 4;
	u32 re			: 3;
	u32 rsvd6		: 1;
	u32 cxt_num		: 16;
	u64 desc_idx;
	u32 rsvd7[7];
	u32 err_class		: 16;
	u32 rsvd8		: 16;
	u32 vendor[4];
} __packed;

/* L1 Table Entry */
struct cxt_l1_entry {
	u64 vl			: 1;	/* QW0 */
	u64 ka			: 1;
	u64 pv			: 1;
	u64 rsvd1		: 3;
	u64 cxt_ctrl_ptr	: 58;
	u64 akey_tbl_size	: 4;	/* QW1 */
	u64 rsvd2		: 8;
	u64 akey_tbl_ptr	: 52;
	u64 cxt_pasid		: 20;	/* QW2 */
	u64 max_buf		: 4;
	u64 rsvd3		: 8;
	u64 opb_000_enb		: 32;
	u64 rsvd4;			/* QW3 */
} __packed;

struct irq_entry {
	int vector;
};

struct sdxi_dev {
	struct list_head list;

	/* physical device */
	struct pci_dev *pdev;
	resource_size_t ctrl_regs_bar;	/* ctrl registers base (BAR0) */
	resource_size_t dbs_bar;	/* doorbells base (BAR2) */
	void __iomem *ctrl_regs;	/* virt addr of ctrl registers */
	void __iomem *dbs;		/* virt addr of doorbells */

	/* hardware capabilities (from cap0 & cap1) */
	u16 sfunc;			/* function's requester id */
	u32 db_stride;			/* doorbell stride in bytes */
	u64 max_ring_entries;		/* max # of ring entries supported */

	u32 max_akeys;			/* max akey # supported */
	u32 max_cxts;			/* max contexts # supported */
	u32 op_grp_cap;			/* supported operatation group cap */

	/* MSI */
	unsigned int irq_count;
	struct irq_entry err_irq;

	/* context management */
	struct mutex cxt_lock;		/* context protection */
	struct list_head cxt_list;
	int cxt_count;
	/* l2 table, pre-allocated with sdxi_device */
	struct sdxi_cxt_l2_table *l2_table;
	dma_addr_t l2_dma;
	/* list of context l1 tables, on-demand, access with [l2_idx] */
	struct cxt_l1_entry *l1_table_array[L2_TABLE_ENTRIES];
	/* all contexts, on-demand, access with [l2_idx][l1_idx] */
	struct sdxi_cxt **cxt_array[L2_TABLE_ENTRIES];

	/* error log */
	u32 err_log_num;
	struct sdxi_err *err_log;
	dma_addr_t err_log_dma;

	/* DMA engine */
	struct dma_device dma_dev;
	struct sdxi_dma_chan *sdxi_dma_chan;
	struct sdxi_tasklet_data tdata;

	/* special contexts */
	struct sdxi_cxt *admin_cxt;	/* admin context */
	struct sdxi_cxt *dma_cxt;	/* DMA engine context */
};

static inline struct device *sdxi_to_dev(const struct sdxi_dev *sdxi)
{
	return &sdxi->pdev->dev;
}

#define sdxi_dbg(s, fmt, ...) dev_dbg(sdxi_to_dev(s), fmt, ## __VA_ARGS__)
#define sdxi_info(s, fmt, ...) dev_info(sdxi_to_dev(s), fmt, ## __VA_ARGS__)
#define sdxi_err(s, fmt, ...) dev_err(sdxi_to_dev(s), fmt, ## __VA_ARGS__)

/***************************/
/*           API           */
/***************************/
/* Device Control */
int sdxi_device_init(struct sdxi_dev *sdxi);
void sdxi_device_exit(struct sdxi_dev *sdxi);

/* Context Control */
struct sdxi_cxt *sdxi_cxt_alloc(struct sdxi_dev *sdxi);
struct sdxi_cxt *sdxi_working_cxt_random_alloc(void);
void sdxi_cxt_free(struct sdxi_cxt *cxt);
struct sdxi_cxt *sdxi_working_cxt_init(struct sdxi_dev *sdxi,
				       enum sdxi_cxt_id);
void sdxi_working_cxt_exit(struct sdxi_cxt *cxt);
struct sdxi_cxt *sdxi_working_cxt_alloc(void);

/* Submission Queue */
struct sdxi_sq *sdxi_sq_alloc(struct sdxi_cxt *cxt, int ring_size);
struct sdxi_sq *sdxi_sq_alloc_default(struct sdxi_cxt *cxt);
void sdxi_sq_free(struct sdxi_sq *sq);
int sdxi_submit_desc(struct sdxi_sq *sq, struct sdxi_desc *desc);

/* DMA Engine */
int sdxi_dma_register(struct sdxi_cxt *dma_cxt);
void sdxi_dma_unregister(struct sdxi_cxt *dma_cxt);

/* Chardev (IOCTL) */
int sdxi_chardev_init(void);
void sdxi_chardev_exit(void);

void sdxi_delay(void);

static inline u64 sdxi_read64(const struct sdxi_dev *sdxi, enum sdxi_reg reg)
{
	return ioread64(sdxi->ctrl_regs + reg);
}

static inline void sdxi_write64(struct sdxi_dev *sdxi, enum sdxi_reg reg, u64 val)
{
	iowrite64(val, sdxi->ctrl_regs + reg);
	sdxi_delay();
}



#endif /* __SDXI_H */
