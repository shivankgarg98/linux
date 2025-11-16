/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DMA_SDXI_DESCRIPTOR_H
#define DMA_SDXI_DESCRIPTOR_H

/*
 * Facilities for encoding SDXI descriptors.
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/minmax.h>
#include <linux/sizes.h>
#include <linux/stddef.h>
#include <linux/types.h>
#include <asm/byteorder.h>

#include "hw.h"

#if IS_ENABLED(CONFIG_KUNIT)
int __must_check sdxi_encode_size32(u64 size, __le32 *dest);
#endif

static inline void sdxi_desc_set_csb(struct sdxi_desc *desc,
				     dma_addr_t addr)
{
	desc->csb_ptr = cpu_to_le64(FIELD_PREP(SDXI_DSC_CSB_PTR, addr >> 5));
}

static inline void sdxi_desc_make_valid(struct sdxi_desc *desc)
{
	u32 opcode = le32_to_cpu(desc->opcode);

	/*
	 * We should only set vl once per submission, and never modify
	 * the descriptor after that, at least not until hardware has
	 * retired and invalidated it.
	 */
	WARN_ON_ONCE(FIELD_GET(SDXI_DSC_VL, opcode) == 1);

	FIELD_MODIFY(SDXI_DSC_VL, &opcode, 1);

	/*
	 * Once vl is set, no more modifications to the descriptor
	 * payload are allowed. Ensure the vl update is ordered after
	 * all other initialization of the descriptor.
	 */
	dma_wmb();
	desc->opcode = cpu_to_le32(opcode);
}

struct sdxi_cxt_range {
	u16 cxt_start;
	u16 cxt_end;
};

static inline struct sdxi_cxt_range __sdxi_cxt_range(u16 a, u16 b)
{
	return (struct sdxi_cxt_range) {
		.cxt_start = min(a, b),
		.cxt_end   = max(a, b),
	};
}

#define sdxi_cxt_range_1(_id)			\
	({					\
		u16 id = (_id);			\
		__sdxi_cxt_range(id, id);	\
	})

#define sdxi_cxt_range_2(_id1, _id2) __sdxi_cxt_range(_id1, _id2)

#define _sdxi_cxt_range(_1, _2, _fn, ...) _fn

#define sdxi_cxt_range(...)						\
	_sdxi_cxt_range(__VA_ARGS__,					\
			sdxi_cxt_range_2, sdxi_cxt_range_1)(__VA_ARGS__)

void sdxi_serialize_nop(struct sdxi_desc *desc);

struct sdxi_copy {
	dma_addr_t src;
	dma_addr_t dst;
	u64 len;
	u16 src_akey;
	u16 dst_akey;
};

int sdxi_encode_copy(struct sdxi_desc *desc,
		     const struct sdxi_copy *params);

struct sdxi_intr {
	u16 akey;
};

int sdxi_encode_intr(struct sdxi_desc *desc,
		     const struct sdxi_intr *params);

struct sdxi_cxt_start {
	struct sdxi_cxt_range range;
};

int sdxi_encode_cxt_start(struct sdxi_desc *desc,
			  const struct sdxi_cxt_start *params);

struct sdxi_cxt_stop {
	struct sdxi_cxt_range range;
};

int sdxi_encode_cxt_stop(struct sdxi_desc *desc,
			  const struct sdxi_cxt_stop *params);

#endif /* DMA_SDXI_DESCRIPTOR_H */
