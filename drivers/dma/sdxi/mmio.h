/* SPDX-License-Identifier: GPL-2.0-only */

// SDXI MMIO register offsets and layouts.

#ifndef DMA_SDXI_MMIO_H
#define DMA_SDXI_MMIO_H

#include <linux/bits.h>
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

enum {
	//// SDXI_MMIO_CTL0 bit definitions

	// Initiate function state transitions
	SDXI_MMIO_CTL0_FN_GSR = GENMASK_ULL(1, 0),

	//// SDXI_MMIO_STS0 bit definitions

	// Overall function state.
	SDXI_MMIO_STS0_FN_GSV = GENMASK_ULL(2, 0),

	//// SDXI_MMIO_CAP0 bit definitions

	// SDXI function identifier, unique within its function group.
	SDXI_MMIO_CAP0_SFUNC = GENMASK_ULL(15, 0),

	// Encoded address stride between doorbell sections.
	SDXI_MMIO_CAP0_DB_STRIDE = GENMASK_ULL(22, 20),

	// Encoded maximum descriptor ring size for any context in
	// this function.
	SDXI_MMIO_CAP0_MAX_DS_RING_SZ = GENMASK_ULL(28, 24),

	//// SDXI_MMIO_CAP1 bit definitions
	
	//// SDXI_MMIO_CXT_L2 bit definitions

	// Pointer to level 2 context table (4KB-aligned).
	SDXI_MMIO_CXT_L2_PTR = GENMASK_ULL(63, 12),

	//// SDXI_MMIO_ERR_CFG bit definitions

	// Pointer to error log buffer (4KB-aligned).
	SDXI_MMIO_ERR_CFG_PTR = GENMASK_ULL(63, 12),

	// Encoded error log buffer size.
	SDXI_MMIO_ERR_CFG_SZ  = GENMASK_ULL(5, 1),

	// Error log enable.
	SDXI_MMIO_ERR_CFG_EN  = BIT_ULL(0),

	//// SDXI_MMIO_RKEY bit definitions

	// Pointer to RKey table (4KB-aligned).
	SDXI_MMIO_RKEY_PTR = GENMASK_ULL(63, 12),
	// Encoded RKey table size.
	SDXI_MMIO_RKEY_SZ = GENMASK_ULL(4, 1),
	// RKey functionality enabled, also functions as validity bit
	// for ptr and sz.
	SDXI_MMIO_RKEY_EN = BIT_ULL(0),
	

	//// SDXI_MMIO_ERR_CTL bit definitions

	// When set to 1, an interrupt is signaled when hardware
	// transitions MMIO_ERR_STS.sts from 0 to 1. Otherwise no
	// interrupt is generated.
	SDXI_MMIO_ERR_CTL_EN = BIT_ULL(0),

	//// SDXI_MMIO_ERR_STS bit definitions.

	// The device has attempted to log an error. Other bits
	// indicate whether this was successful.
	SDXI_MMIO_ERR_STS_STS_BIT = BIT_ULL(0),

	// Error log overflowed and some error information was discarded.
	SDXI_MMIO_ERR_STS_OVF_BIT = BIT_ULL(1),

	// The device has encountered an error while attempting to log
	// an error; some error information has been discarded.
	SDXI_MMIO_ERR_STS_ERR_BIT = BIT_ULL(3),
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
