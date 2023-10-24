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
#define MMIO_CTRL_REGS_BAR		0x0
#define MMIO_DOORBELL_BAR		0x2

/* MMIO ctrl and status register offsets */
#define MMIO_CTRL0_OFFSET		0x000000
#define MMIO_GRP_ENUM_OFFSET		0x000008
#define MMIO_CTRL2_OFFSET		0x000010
#define MMIO_STS0_OFFSET		0x000100
#define MMIO_CAP0_OFFSET		0x000200
#define MMIO_CAP1_OFFSET		0x000208
#define MMIO_VER_OFFSET			0x000210

/* control register 0 (CTRL0) */
#define CTRL0_FN_GSR_MASK		0x3
#define CTRL0_FN_PASID_VL_SHIFT		2
#define CTRL0_FN_PASID_VL_MASK		0x1
#define CTRL0_FN_ERR_INTR_EN_SHIFT	4
#define CTRL0_FN_ERR_INTR_EN_MASK	0x1
#define CTRL0_FN_PASID_SHIFT		8
#define CTRL0_FN_PASID_MASK		0xFFFFF

/* function state control (FN_GSR) constants */
#define GSRV_RESET				0x0
#define GSRV_STOP_SF				0x1
#define GSRV_STOP_HD				0x2
#define GSRV_ACTIVE				0x3

/* group enum (GRP_ENUM) */
#define GRP_ENUM_BUSY_MASK		0x1
#define GRP_ENUM_PROBE_SHIFT		1
#define GRP_ENUM_PROBE_MASK		0x1

/* control register 2 (CTRL2) */
#define CTRL2_MAX_BUFF_MASK		0xF
#define CTRL2_MAX_AKEY_SZ_SHIFT		12
#define CTRL2_MAX_AKEY_SZ_MASK		0xF
#define CTRL2_MAX_CTXT_SHIFT		16
#define CTRL2_MAX_CTXT_MASK		0xFFFF
#define CTRL2_OPB_000_AVL_SHIFT		32
#define CTRL2_OPB_000_AVL_MASK		0xFFFFFFFF

/* state register 0 (STS0) */
#define STS0_FN_GSV_MASK		0x7

/* function state (FN_GSV) constants */
#define GSV_STOP				0x0
#define GSV_INIT				0x1
#define GSV_ACTIVE				0x2
#define GSV_STOPG_SF				0x3
#define GSV_STOPG_HD				0x4
#define GSV_ERROR				0x5

/* capability register 0 (CAP0) */
#define CAP0_SFUNC_MASK			0xFFFF
#define CAP0_VF_SHIFT			16
#define CAP0_VF_MASK			0x1
#define CAP0_CS_CAP_SHIFT		16
#define CAP0_CS_CAP_MASK		0x1
#define CAP0_DB_STRIDE_SHIFT		20
#define CAP0_DB_STRIDE_MASK		0x7
#define CAP0_MAX_RING_SZ_SHIFT		24
#define CAP0_MAX_RING_SZ_MASK		0x1F
#define CAP0_MAX_RKEY_SZ_SHIFT		32
#define CAP0_MAX_RKEY_SZ_MASK		0xF

/* capability register 1 (CAP1) */
#define CAP1_MAX_BUFF_MASK		0xF
#define CAP1_RKEY_CAP_SHIFT		4
#define CAP1_RKEY_CAP_MASK		0x1
#define CAP1_RM_SHIFT			5
#define CAP1_RM_MASK			0x1
#define CAP1_MMIO64_SHIFT		6
#define CAP1_MMIO64_MASK		0x1
#define CAP1_MAX_ERR_LOG_SZ_SHIFT	8
#define CAP1_MAX_ERR_LOG_SZ_MASK	0xF
#define CAP1_MAX_AKT_SIZE_SHIFT		12
#define CAP1_MAX_AKT_SIZE_MASK		0xF
#define CAP1_MAX_CXT_SHIFT		16
#define CAP1_MAX_CXT_MASK		0xFFFF
#define CAP1_OPB_000_CAP_SHIFT		32
#define CAP1_OPB_000_CAP_MASK		0xFFFFFFFF

/* version register (VER) */
#define VER_MINOR_MASK			0xFF
#define VER_MAJOR_SHIFT			16
#define VER_MAJOR_MASK			0xFF

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

u64 reg_read64(void __iomem *addr);
void reg_write64(void __iomem *addr, u64 val);

#endif /* __SDXI_PCI_H */
