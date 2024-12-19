/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * Data structures and constants defined in the SDXI specification,
 * with low-level accessors.
 *
 * Names of structures, members, and subfields (bit ranges within
 * members) are written to match the spec, generally. E.g. struct
 * sdxi_cxt_l2_ent corresponds to CXT_L2_ENT in the spec.
 *
 * Note: a member can have a subfield whose name is identical to the
 * member's name. E.g. CXT_L2_ENT's lv01_ptr.
 *
 * All reserved fields and bits (usually named "rsvd" or some
 * variation) must be set to zero by the driver unless otherwise
 * specified.
 */

#ifndef LINUX_SDXI_HW_H
#define LINUX_SDXI_HW_H

#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/types.h>
#include <asm/barrier.h>
#include <asm/byteorder.h>

/**
 * struct sdxi_cxt_l2_ent - Context Level 2 Table Entry (CXT_L2_ENT).
 */
struct sdxi_cxt_l2_ent {
	/**
	 * @lv01_ptr: Address of L1 table, with a validity bit.
	 *
	 * Bit 0:      (vl) Valid.
	 * Bits 11-01: (rsvd) Reserved.
	 * Bits 63-12: (lv01_ptr) Pointer to the start of a Context Level 1
	 *             table, 4K-aligned.
	 */
	__le64 lv01_ptr;
} __packed;
static_assert(sizeof(struct sdxi_cxt_l2_ent) == 8);

#define SDXI_CXT_L2_ENT_LV01_PTR_MASK GENMASK_ULL(63, 12)
#define SDXI_CXT_L2_ENT_VL_MASK       BIT_ULL(0)

static inline void sdxi_cxt_l2_ent_set(struct sdxi_cxt_l2_ent *ent,
				       dma_addr_t addr, bool valid)
{
	u64 tmp;

	tmp = (addr & SDXI_CXT_L2_ENT_LV01_PTR_MASK);
	if (valid)
		tmp |= SDXI_CXT_L2_ENT_VL_MASK;

	// We're potentially releasing the entry to the hw, ensure
	// the valid bit update follows any prior stores.
	dma_wmb();
	WRITE_ONCE(ent->lv01_ptr, cpu_to_le64(tmp));
}

static inline dma_addr_t
sdxi_cxt_l2_ent_lv01_ptr(const struct sdxi_cxt_l2_ent *ent)
{
	return le64_to_cpu(ent->lv01_ptr) & SDXI_CXT_L2_ENT_LV01_PTR_MASK;
}

static inline bool
sdxi_cxt_l2_ent_vl(const struct sdxi_cxt_l2_ent *ent)
{
	return le64_to_cpu(ent->lv01_ptr) & SDXI_CXT_L2_ENT_VL_MASK;
}

/*
 * The level 2 table is 4KB and has 512 level 1 pointer entries.
 */
#define SDXI_L2_TABLE_ENTRIES 512
struct sdxi_cxt_l2_table {
	struct sdxi_cxt_l2_ent entry[SDXI_L2_TABLE_ENTRIES];
};
static_assert(sizeof(struct sdxi_cxt_l2_table) == 4096);

#endif /* LINUX_SDXI_HW_H */
