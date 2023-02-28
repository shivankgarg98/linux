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

/* MMIO register regions */
#define MMIO_CTRL_STATUS_OFFSET		0x000000
#define MMIO_CXT_TABLE_OFFSET		0x010000
#define MMIO_ERR_LOG_CTRL_OFFSET	0x020000
#define MMIO_MAILBOX_CTRL_OFFSET	0x030000

/* MMIO ctrl and status register offsets */
#define MMIO_CTRL0_OFFSET		0x000000
#define MMIO_CTRL1_OFFSET		0x000008
#define MMIO_CTRL2_OFFSET		0x000010
#define MMIO_STATUS_0_OFFSET		0x000100
#define MMIO_CAP0_OFFSET		0x000200
#define MMIO_CAP1_OFFSET		0x000208

/* MMIO control register 0 */
#define CTRL0_ENABLE_MASK		0x3
#define CTRL0_FUNC_PASID_V_MASK		0x1
#define CTRL0_FUNC_PASID_V_SHIFT	0x2
#define CTRL0_FUNC_ERR_INT_EN_MASK	0x1
#define CTRL0_FUNC_ERR_INT_EN_SHIFT	0x4
#define CTRL0_FUNC_PASID_MASK		0xFFFFF
#define CTRL0_FUNC_PASID_SHIFT		0x8

/* MMIO control register 1 */
#define CTRL1_ALL_CXT_DB_MASK		0x1

/* MMIO control register 2 */
#define CTRL2_MAX_BUFF_CNTL_MASK	0xF
#define CTRL2_MULTI_FUNC_EN_MASK	0x1
#define CTRL2_MULTI_FUNC_EN_SHIFT	0x4
#define CTRL2_MAX_AKT_SIZE_CNTL_MASK	0xF
#define CTRL2_MAX_AKT_SIZE_CNTL_SHIFT	0x12
#define CTRL2_N_CXT_CNTL_MASK		0xFFFF
#define CTRL2_N_CXT_CNTL_SHIFT		0x10
#define CTRL2_CMD_GRP0_EN_MASK		0xFFFFFFFF
#define CTRL2_CMD_GRP0_EN_SHIFT		0x20

/* MMIO status register 0 (offset: 0x00100) */
#define STATUS0_GLOBAL_STATE_MASK	0x7

/* status register 0 constants */
#define STATUS_STOPPED			0x0
#define STATUS_INIT			0x1
#define STATUS_ACTIVE			0x2
#define STATUS_SOFT_STP_REQ		0x3
#define STATUS_HARD_STP_REQ		0x4
#define STATUS_STP_ON_ERR		0x5

/* MMIO capability register 0 */
#define CAP0_SFUNC_MASK			0xFFFF
#define CAP0_VF_SHIFT			16
#define CAP0_VF_MASK			0x1
#define CAP0_DB_STRIDE_SHIFT		20
#define CAP0_DB_STRIDE_MASK		0x7
#define CAP0_MAX_RING_SZ_SHIFT		24
#define CAP0_MAX_RING_SZ_MASK		0x1F
#define CAP0_MAX_RKEY_SZ_SHIFT		32
#define CAP0_MAX_RKEY_SZ_MASK		0xF

/* MMIO capability register 1 */
#define CAP1_MAX_BUFF_MASK		0xF
#define CAP1_RKEY_SUP_SHIFT		4
#define CAP1_RKEY_SUP_MASK		0x1
#define CAP1_RM_SHIFT			5
#define CAP1_RM_MASK			0x1
#define CAP1_MMIO64_SHIFT		6
#define CAP1_MMIO64_MASK		0x1
#define CAP1_MAX_ERR_LOG_SZ_SHIFT	8
#define CAP1_MAX_ERR_LOG_SZ_MASK	0xF
#define CAP1_MAX_AKT_SIZE_SHIFT		12
#define CAP1_MAX_AKT_SIZE_MASK		0xF
#define CAP1_N_CXT_SHIFT		16
#define CAP1_N_CXT_MASK			0xFFFF
#define CAP1_OP_GRP_CAP_SHIFT		32
#define CAP1_OP_GRP_CAP_MASK		0xFFFFFFFF

/* MMIO context and rkey table registers */
#define CXT_TABLE_BASE_OFFSET		0x10000
#define CXT_TABLE_BASE_PTR_MASK		0xFFFFFFFFFFFFF000

#define RKEY_TABLE_BASE_OFFSET		0x10100
#define RKEY_TABLE_EN_MASK		0x1
#define RKEY_TABLE_SIZE_SHIFT		0x1
#define RKEY_TABLE_SIZE_MASK		0xF
#define RKEY_TABLE_BASE_PTR_SHIFT	0x1
#define RKEY_TABLE_BASE_PTR_MASK	0xFFFFFFFFFFFFF000

/* MMIO Error log control and status */
#define ERR_LOG_CTRL_OFFSET		0x20000
#define ERR_LOG_STS_OFFSET		0x20008
#define ERR_LOG_CFG_OFFSET		0x20010
#define ERR_LOG_WRITE_PTR_OFFSET	0x20020
#define ERR_LOG_READ_PTR_OFFSET		0x20028
#define ERR_LOG_BASE_PTR_MASK		0xFFFFFFFFFFFFF000
#define ERR_LOG_STATUS_MASK		0x1

#define FUNC_STATE_STOPPED	0x0
#define FUNC_STATE_INIT		0x1
#define FUNC_STATE_ACTIVE	0x2
#define FUNC_STATE_SOFT_STOP	0x3
#define FUNC_STATE_HARD_STOP	0x4
#define FUNC_STATE_ERR_STOPPED	0x5

#define FUNC_REQ_RESET		0x0
#define FUNC_REQ_SOFT_STOP	0x1
#define FUNC_REQ_HARD_STOP	0x2
#define FUNC_REQ_ACTIVE		0x3

u64 reg_read64(void __iomem *addr);
void reg_write64(void __iomem *addr, u64 val);

#endif /* __SDXI_PCI_H */
