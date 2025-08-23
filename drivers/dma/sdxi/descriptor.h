#ifndef DMA_SDXI_DESCRIPTOR_H
#define DMA_SDXI_DESCRIPTOR_H

#include <linux/bits.h>
#include <linux/container_of.h>
#include <linux/errno.h>
#include <linux/minmax.h>
#include <linux/stddef.h>
#include <linux/types.h>
#include <asm/byteorder.h>

#include "hw.h"

// FIXME: reconcile this with SDXI_DSC_VL etc.
typedef enum sdxi_desc_flags {
	SDXI_DESC_VALID  = BIT(0),
	SDXI_DESC_SEQCON = BIT(1),
	SDXI_DESC_FENCE  = BIT(2),
	SDXI_DESC_CHAIN  = BIT(3),
	SDXI_DESC_CSR    = BIT(4),
	SDXI_DESC_RB     = BIT(5),
} sdxi_desc_flags_t;

typedef enum sdxi_op_group {
	SDXI_OPGRP_RESERVED = 0x000,
	SDXI_OPGRP_DMAB     = 0x001,
	SDXI_OPGRP_ADMIN    = 0x002,
} sdxi_op_group_t;

typedef enum sdxi_operation {
	SDXI_OP_DMAB_NOP = 0x01,
	SDXI_OP_DMAB_WRT_IMM = 0x02,
	SDXI_OP_DMAB_COPY = 0x03,

	SDXI_OP_CXT_START_NM = 0x03,
	SDXI_OP_CXT_START_RS = 0x08,
} sdxi_operation_t; // FIXME: pack to minimize sdxi_desc_attrs size

// Generic attributes common to all descriptors
struct sdxi_desc_attrs {
	dma_addr_t csb_handle;
	u16 op_group; // FIXME: use the enums
	u8 operation;
	u8 flags; // bitfield of sdxi_desc_flags_t
	bool use_csb;
};

static inline void sdxi_desc_set_csb(struct sdxi_desc_new *desc,
				     dma_addr_t addr)
{
	desc->csb_ptr = cpu_to_le64(FIELD_PREP(SDXI_DSC_CSB_PTR, addr >> 5));
}

static inline void
sdxi_desc_attrs_set_csb(struct sdxi_desc_attrs *attrs, dma_addr_t handle)
{
	attrs->use_csb = true;
	attrs->csb_handle = handle; // FIXME: align, cpu_to_le64
}

static inline __must_check
int sdxi_encode_size32(u64 size, __le32 *dest)
{
	// sizes are encoded as value - 1:
	// value    encoding
	//    1           0
	//    2           1
	//   4G  0xffffffff
	if (WARN_ON_ONCE(size > SZ_4G) ||
	    WARN_ON_ONCE(size == 0))
		return -EINVAL;
	size = clamp_val(size, 1, SZ_4G);
	*dest = cpu_to_le32((u32)(size - 1));
	return 0;
}

int __must_check __sdxi_desc_encode(struct sdxi_desc_new *desc,
				  const struct sdxi_desc_attrs *attrs);

void sdxi_desc_decode(const struct sdxi_desc_new *desc, struct sdxi_desc_attrs *attrs);

// The "unpacked" version of a generic, operation-agnostic SDXI
// descriptor.
struct sdxi_desc_unpacked {
	u64 csb_ptr;
	u16 type;
	u8 subtype;
	bool vl;
	bool se;
	bool fe;
	bool ch;
	bool csr;
	bool rb;
	bool np;
};

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

struct sdxi_copy {
	dma_addr_t src;
	dma_addr_t dst;
	size_t len;
	u16 src_akey;
	u16 dst_akey;
};

int sdxi_encode_copy(struct sdxi_desc_new *desc,
		     const struct sdxi_copy *params);

struct sdxi_intr {
	u16 akey;
};

int sdxi_encode_intr(struct sdxi_desc_new *desc,
		     const struct sdxi_intr *params);

struct sdxi_cxt_start {
	struct sdxi_cxt_range range;
};

int sdxi_encode_cxt_start(struct sdxi_desc_new *desc,
			  const struct sdxi_cxt_start *params);

struct sdxi_cxt_stop {
	struct sdxi_cxt_range range;
};

int sdxi_encode_cxt_stop(struct sdxi_desc_new *desc,
			  const struct sdxi_cxt_stop *params);

#define sdxi_desc_encode(d_, op_)			\
	_Generic((op_)					\
		 , struct sdxi_copy *: sdxi_copy_encode	\
		 )(d_, op_)

void sdxi_desc_pack(struct sdxi_desc_new *to,
		    const struct sdxi_desc_unpacked *from);
void sdxi_desc_unpack(struct sdxi_desc_unpacked *to,
		      const struct sdxi_desc_new *from);

#endif /* DMA_SDXI_DESCRIPTOR_H */
