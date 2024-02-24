/*
 * SDXI MMIO registers
 *
 * Copyright (C) 2022 AMD, Inc. All rights reserved.
 *
 * Author: Wei Huang <wei.huang2@amd.com>
 *
 */

#ifndef __SDXI_PCI_H
#define __SDXI_PCI_H

#include <linux/io.h>

/* MMIO BARs */
#define MMIO_CTL_REGS_BAR		0x0
#define MMIO_DOORBELL_BAR		0x2

/* MMIO Control 0 Register */
#define MMIO_CTL0_OFFSET		0x000000
union mmio_ctl0_reg {
	struct {
		u64 fn_gsr		:2;
		u64 fn_pasid_vl		:1;
		u64 rsvd0		:1;
		u64 fn_err_intr_en	:1;
		u64 rsvd1		:3;
		u64 fn_pasid		:20;
		u64 rsvd2		:4;
		u64 fn_grp_id		:32;
	};
	u64 data;
} __packed __aligned(8);

/* function state control (ctl0.fn_gsr) constants */
#define GSRV_RESET			0x0
#define GSRV_STOP_SF			0x1
#define GSRV_STOP_HD			0x2
#define GSRV_ACTIVE			0x3

/* MMIO Group Enum Register */
#define MMIO_GRP_ENUM_OFFSET		0x000008
union mmio_grp_enum_reg {
	struct {
		u64 busy		:1;
		u64 probe		:1;
		u64 rsvd0		:6;
		u64 rsvd1		:56;
	};
	u64 data;
} __packed __aligned(8);

/* MMIO Control 2 Register */
#define MMIO_CTL2_OFFSET		0x000010
union mmio_ctl2_reg {
	struct {
		u64 max_buffer		:4;
		u64 rsvd0		:8;
		u64 max_akey_sz		:4;
		u64 max_cxt		:16;
		u64 opb_000_avl		:32;
	};
	u64 data;
} __packed __aligned(8);

/* MMIO Status 0 Register */
#define MMIO_STS0_OFFSET		0x000100
union mmio_sts0_reg {
	struct {
		u64 fn_gsv		:3;
		u64 rsvd0		:5;
		u64 rsvd1		:56;
	};
	u64 data;
} __packed __aligned(8);

/* function state (sts0.fn_gsv) constants */
#define GSV_STOP			0x0
#define GSV_INIT			0x1
#define GSV_ACTIVE			0x2
#define GSV_STOPG_SF			0x3
#define GSV_STOPG_HD			0x4
#define GSV_ERROR			0x5

/* MMIO Capability 0 Register */
#define MMIO_CAP0_OFFSET		0x000200
union mmio_cap0_reg {
	struct {
		u64 sfunc		:16;
		u64 vf			:1;
		u64 cs_cap		:2;
		u64 rsvd0		:1;
		u64 db_stride		:3;
		u64 rsvd1		:1;
		u64 max_ds_ring_sz	:8;
		u64 max_rkey_sz		:8;
		u64 rsvd2		:24;
	};
	u64 data;
} __packed __aligned(8);

/* MMIO Capability 1 Register */
#define MMIO_CAP1_OFFSET		0x000208
union mmio_cap1_reg {
	struct {
		u64 max_buffer		:4;
		u64 rkey_cap		:1;
		u64 rm			:1;
		u64 mmio64		:1;
		u64 rsvd0		:1;
		u64 max_errlog_sz	:4;
		u64 max_akey_sz		:4;
		u64 max_cxt		:16;
		u64 opb_000_cap		:32;
	};
	u64 data;
} __packed __aligned(8);

/* MMIO Version Register */
#define MMIO_VER_OFFSET			0x000210
union mmio_ver_reg {
	struct {
		u64 minor		:8;
		u64 rsvd0		:8;
		u64 major		:8;
		u64 rsvd1		:40;
	};
	u64 data;
} __packed __aligned(8);

/* MMIO L2 and rkey table register offsets */
#define MMIO_CXT_L2_OFFSET		0x10000
#define MMIO_RKEY_OFFSET		0x10100

/* context table register (CXT_L2) */
#define CXT_L2_PTR_MASK			0xFFFFFFFFFFFFF000

/* rkey table register (RKEY) */
#define RKEY_EN_MASK			0x1
#define RKEY_SZ_SHIFT			0x1
#define RKEY_SZ_MASK			0xF
#define RKEY_PTR_MASK			0xFFFFFFFFFFFFF000

/* MMIO error log control and status register offsets */
#define MMIO_ERR_CTL_OFFSET		0x20000
#define MMIO_ERR_STS_OFFSET		0x20008
#define MMIO_ERR_CFG_OFFSET		0x20010
#define MMIO_ERR_WRT_OFFSET		0x20020
#define MMIO_ERR_RD_OFFSET		0x20028

/* error log control register (ERR_CTL) */
#define ERR_CTL_INTR_EN_MASK		0x1

/* error log status register (ERR_STS) */
#define ERR_STS_REC_MASK		0x1
#define ERR_STS_OVF_SHIFT		1
#define ERR_STS_OVF_MASK		0x1
#define ERR_STS_ERR_SHIFT		3
#define ERR_STS_ERR_MASK		0x1

/* error log config register (ERR_CFG) */
#define ERR_CFG_EN_MASK			0x1
#define ERR_CFG_SZ_SHIFT		1
#define ERR_CFG_SZ_MASK			0x1F
#define ERR_CFG_PTR_MASK		0xFFFFFFFFFFFFF000

#endif /* __SDXI_PCI_H */
