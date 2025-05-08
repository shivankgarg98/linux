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

#include <asm/barrier.h>
#include <asm/byteorder.h>
#include <linux/align.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/bug.h>
#include <linux/build_bug.h>
#include <linux/log2.h>
#include <linux/sizes.h>
#include <linux/types.h>

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

#define SDXI_CXT_L2_ENT_LV01_PTR GENMASK_ULL(63, 12)
#define SDXI_CXT_L2_ENT_VL       BIT_ULL(0)

} __packed;
static_assert(sizeof(struct sdxi_cxt_l2_ent) == 8);

static inline void sdxi_cxt_l2_ent_set(struct sdxi_cxt_l2_ent *ent,
				       dma_addr_t addr, bool valid)
{
	WARN_ON(!IS_ALIGNED(addr, SZ_4K));
	u64 tmp = FIELD_PREP(SDXI_CXT_L2_ENT_LV01_PTR, addr >> ilog2(SZ_4K)) |
		  FIELD_PREP(SDXI_CXT_L2_ENT_VL, valid);

	// We're potentially releasing the entry to the hw, ensure
	// the valid bit update follows any prior stores.
	dma_wmb();
	WRITE_ONCE(ent->lv01_ptr, cpu_to_le64(tmp));
}

static inline dma_addr_t
sdxi_cxt_l2_ent_lv01_ptr(const struct sdxi_cxt_l2_ent *ent)
{
	return FIELD_GET(SDXI_CXT_L2_ENT_LV01_PTR, le64_to_cpu(ent->lv01_ptr)) << ilog2(SZ_4K);
}

static inline bool
sdxi_cxt_l2_ent_vl(const struct sdxi_cxt_l2_ent *ent)
{
	return FIELD_GET(SDXI_CXT_L2_ENT_VL, le64_to_cpu(ent->lv01_ptr));
}

/*
 * The level 2 table is 4KB and has 512 level 1 pointer entries.
 */
#define SDXI_L2_TABLE_ENTRIES 512
struct sdxi_cxt_l2_table {
	struct sdxi_cxt_l2_ent entry[SDXI_L2_TABLE_ENTRIES];
};
static_assert(sizeof(struct sdxi_cxt_l2_table) == 4096);

/** 
 * struct sdxi_cxt_l1_ent - Context level 1 table entry (CXT_L1_ENT).
 */
struct sdxi_cxt_l1_ent {
    /**
     * @cxt_ctl_ptr: context control pointer with associated flags.
     *
     * Bit 0:       (vl) Valid.
     * Bit 1:       (ka) "Keep active" hint.
     * Bit 2:       (pv) Validity bit for cxt_pasid.
     * Bits 5-3:    (rsvd) Reserved.
     * Bits 63-6:   (cxt_ctl_ptr) Pointer to context control block.
     */
    __le64 cxt_ctl_ptr;

#define SDXI_CXT_L1_ENT_VL_BIT           BIT_ULL(0)
#define SDXI_CXT_L1_ENT_KA_BIT           BIT_ULL(1)
#define SDXI_CXT_L1_ENT_PV_BIT           BIT_ULL(2)
#define SDXI_CXT_L1_ENT_CXT_CTL_PTR_MASK GENMASK_ULL(63, 6)

    /**
     * @akey_ptr: AKey table pointer and size.
     *
     * Bits 3-0: (akey_sz) AKey table size: 2^(akey_sz + 8) entries.
     * Bits 11-4: (rsvd) Reserved.
     * Bits 63-12: (akey_ptr) Pointer to AKey table.
     */
    __le64 akey_ptr;

#define SDXI_CXT_L1_ENT_AKEY_SZ_MASK     GENMASK_ULL(3, 0)
#define SDXI_CXT_L1_ENT_AKEY_PTR_MASK    GENMASK_ULL(63, 12)

    /**
     * @misc0: Miscellaneous attributes.
     *
     * Bits 19-0: (cxt_pasid) When pv=1, the PASID used by the device to access
     *            the descriptor ring and associated data structures.
     * Bits 23-20: (max_buffer) Maximum data buffer size supported by this
     *             context: 2^(max_buffer + 21) bytes.
     * Bits 31-24: (rsvd) Reserved.
     */
    __le32 misc0;

    /**
     * @opb_000_enb: Bitmask of operation groups enabled for this context.
     */
    __le32 opb_000_enb;

    /**
     * @rsvd_0: Reserved.
     */
    __u8 rsvd_0[8];
} __packed;
static_assert(sizeof(struct sdxi_cxt_l1_ent) == 32);

/**
 * struct sdxi_cst_blk - Completion status block (CST_BLK).
 */
struct sdxi_cst_blk {
	/**
	 * @signal: Completion signal value.
	 */
	__le64 signal;

	/**
	 * @flags: Flags.
	 *
	 * Bits 30-0: (rsvd) Reserved.
	 * Bit 31:    (er) Error recorded.
	 */
	__le32 flags;

#define SDXI_CST_BLK_ER_BIT BIT(31);

	/**
	 * @rsvd_0: Reserved.
	 */
	__u8 rsvd_0[20];
} __packed;
static_assert(sizeof(struct sdxi_cst_blk) == 32);

static inline void sdxi_cst_blk_set(struct sdxi_cst_blk *cst_blk, u64 signal)
{
	*cst_blk = (struct sdxi_cst_blk) {
		.signal = cpu_to_le64(signal),
	};
}

/**
 * struct sdxi_cxt_ctl - Context control entry (CXT_CTL).
 *
 * Control information for a single descriptor ring.
 */
struct sdxi_cxt_ctl {
	/**
	 * @ds_ring_ptr: Descriptor ring pointer and flags.
	 *
	 * Bit 0:     (vl) Valid.
	 * Bit 1:     (rsvd) Reserved.
	 * Bits 3-2:  (qos) QoS.
	 * Bit 4:     (se) Sequential consistency hint.
	 * Bit 5:     (csa) Completion status mode availability.
	 * Bits 63-6: (ds_ring_ptr) 64B-aligned descriptor ring address.
	 */
	__le64 ds_ring_ptr;

#define SDXI_CXT_CTL_VALID_BIT        BIT_ULL(0)
#define SDXI_CXT_CTL_QOS_MASK         GENMASK_ULL(3, 2)
#define SDXI_CXT_CTL_SE_BIT           BIT_ULL(4)
#define SDXI_CXT_CTL_CSA_BIT          BIT_ULL(5)
#define SDXI_CXT_CTL_DS_RING_PTR_MASK GENMASK_ULL(63, 6)

	/**
	 * @ds_ring_sz: Number of descriptors in the ring.
	 */
	__le32 ds_ring_sz;
	/**
	 * @rsvd_0: Reserved.
	 */
	__u8 rsvd_0[4];
	/**
	 * @cxt_sts_ptr: Context status pointer.
	 *
	 * Bits 3-0:  (rsvd) Reserved.
	 * Bits 63-4: (cxt_sts_ptr) 16B-aligned context status (CXT_STS) address.
	 */
	__le64 cxt_sts_ptr;

#define SDXI_CXT_CTL_CXT_STS_PTR_MASK GENMASK_ULL(63, 4)

	/**
	 * @write_index_ptr: Write index pointer.
	 *
	 * Bits 2-0:  (rsvd) Reserved.
	 * Bits 63-3: (write_index_ptr) 8B-aligned descriptor ring write index.
	 */
	__le64 write_index_ptr;

#define SDXI_CXT_CTL_WRITE_INDEX_PTR_MASK GENMASK_ULL(63, 3)

	/**
	 * @rsvd_1: Reserved.
	 */
	__u8 rsvd_1[32];
} __packed;
static_assert(sizeof(struct sdxi_cxt_ctl) == 64);

struct sdxi_cxt_sts {
	u8 state;
#define SDXI_CXT_STS_STATE GENMASK(3, 0)
	u8 misc0;
	u8 rsvd_0[6];
	__le64 read_index;
} __packed;
static_assert(sizeof(struct sdxi_cxt_sts) == 16);

static inline u8 sdxi_cxt_sts_state(const struct sdxi_cxt_sts *sts)
{
	u8 state;

	dma_rmb();
	state = READ_ONCE(sts->state);
	return FIELD_GET(SDXI_CXT_STS_STATE, state);
}

#endif /* LINUX_SDXI_HW_H */
