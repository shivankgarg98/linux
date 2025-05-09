/* SPDX-License-Identifier: GPL-2.0-only */

// SDXI MMIO register offsets and layouts.

#ifndef DMA_SDXI_MMIO_H
#define DMA_SDXI_MMIO_H

#include <linux/compiler_attributes.h>
#include <linux/types.h>

enum sdxi_reg {
	SDXI_MMIO_CTL0       = 0x00000,
	SDXI_MMIO_CTL2       = 0x00010,
	SDXI_MMIO_STS0       = 0x00100,
	SDXI_MMIO_CAP0       = 0x00200,
	SDXI_MMIO_CAP1       = 0x00208,
	SDXI_MMIO_VER        = 0x00210,
	SDXI_MMIO_CXT_L2     = 0x10000,
	SDXI_MMIO_RKEY       = 0x10100,
	SDXI_MMIO_ERR_CTL    = 0x20000,
	SDXI_MMIO_ERR_STS    = 0x20008,
	SDXI_MMIO_ERR_CFG    = 0x20010,
	SDXI_MMIO_ERR_WRT    = 0x20020,
	SDXI_MMIO_ERR_RD     = 0x20028,
};

/* Capability 1 Register */
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

/* Control 0 Register */
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
#define GSRV_STOP_SF			0x1
#define GSRV_ACTIVE			0x3


#endif // DMA_SDXI_MMIO_H
