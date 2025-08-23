/*
 * SDXI descriptor encoding.
 */

#include <kunit/test.h>
#include <kunit/test-bug.h>
#include <kunit/visibility.h>
#include <linux/align.h>
#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/dma-mapping.h>
#include <linux/log2.h>
#include <linux/packing.h>
#include <linux/types.h>
#include <asm/byteorder.h>

#include "hw.h"
#include "descriptor.h"

enum {
	SDXI_PACKING_QUIRKS = QUIRK_LITTLE_ENDIAN | QUIRK_LSW32_IS_FIRST,
};

#define sdxi_desc_field(_high, _low, _member) \
	PACKED_FIELD(_high, _low, struct sdxi_desc_unpacked, _member)
#define sdxi_desc_flag(_bit, _member) \
	sdxi_desc_field(_bit, _bit, _member)

static const struct packed_field_u16 common_descriptor_fields[] = {
	sdxi_desc_flag(0, vl),
	sdxi_desc_flag(1, se),
	sdxi_desc_flag(2, fe),
	sdxi_desc_flag(3, ch),
	sdxi_desc_flag(4, csr),
	sdxi_desc_flag(5, rb),
	sdxi_desc_field(15, 8, subtype),
	sdxi_desc_field(26, 16, type),
	sdxi_desc_flag(448, np),
	sdxi_desc_field(511, 453, csb_ptr),
};

void sdxi_desc_pack(struct sdxi_desc_new *to,
		    const struct sdxi_desc_unpacked *from)
{
	*to = (struct sdxi_desc_new){};
	pack_fields(to, sizeof(*to), from, common_descriptor_fields,
		    SDXI_PACKING_QUIRKS);
}
EXPORT_SYMBOL_IF_KUNIT(sdxi_desc_pack);

void sdxi_desc_unpack(struct sdxi_desc_unpacked *to,
		      const struct sdxi_desc_new *from)
{
	*to = (struct sdxi_desc_unpacked){};
	unpack_fields(from, sizeof(*from), to, common_descriptor_fields,
		      SDXI_PACKING_QUIRKS);
}
EXPORT_SYMBOL_IF_KUNIT(sdxi_desc_unpack);

int __sdxi_desc_encode(struct sdxi_desc_new *desc, const struct sdxi_desc_attrs *attrs)
{
	unsigned int csb_shift = ilog2(sizeof(struct sdxi_cst_blk));
	u64 csb_ptr = 0;

	// FIXME: Fail on unknown/invalid operation. We should be able
	// to specify all valid opgrp/operation combinations, at least
	// until we have to deal with dynamic operation groups.

	// FIXME: some flags have only one valid value for the
	// operation. E.g. se and ch must be 0 for NOPs, and almost
	// all flags have prescribed values for context start/stop
	// operations. (Generally ch must be 0 except for extended
	// descriptors, and I don't see any examples of those in the
	// spec.)
	if (attrs->op_group == SDXI_OPGRP_RESERVED)
		return -EINVAL;

	if (attrs->use_csb) {
		if (attrs->csb_handle == DMA_MAPPING_ERROR)
			return -EFAULT;
		if (!IS_ALIGNED(attrs->csb_handle, sizeof(struct sdxi_cst_blk)))
			return -EFAULT;
		csb_ptr = attrs->csb_handle >> csb_shift;
	}

	*desc = (struct sdxi_desc_new) {
		.opcode = cpu_to_le32(FIELD_PREP(SDXI_DSC_TYPE, attrs->op_group) |
				      FIELD_PREP(SDXI_DSC_SUBTYPE, attrs->operation) |
				      FIELD_PREP(SDXI_DSC_FLAGS, attrs->flags)),
		.csb_ptr = cpu_to_le64(FIELD_PREP(SDXI_DSC_NP, !attrs->use_csb) |
				       FIELD_PREP(SDXI_DSC_CSB_PTR, csb_ptr)),
	};

	struct sdxi_desc_unpacked unpacked = {
		.vl  = attrs->flags & SDXI_DSC_VL,
		.se  = attrs->flags & SDXI_DSC_SE,
		.fe  = attrs->flags & SDXI_DSC_FE,
		.ch  = attrs->flags & SDXI_DSC_CH,
		.csr = attrs->flags & SDXI_DSC_CSR,
		.rb  = attrs->flags & SDXI_DSC_RB,
		.subtype = attrs->operation,
		.type = attrs->op_group,
		.np = !attrs->use_csb,
		.csb_ptr = csb_ptr,
	};
	u8 quirks = QUIRK_LITTLE_ENDIAN | QUIRK_LSW32_IS_FIRST;
	struct sdxi_desc_new desc2 = {};

	pack_fields(&desc2, sizeof(desc2), &unpacked, common_descriptor_fields, quirks);
	struct kunit *t = kunit_get_current_test();
	if (t)
		KUNIT_EXPECT_MEMEQ(t, desc, &desc2, sizeof(desc2));

	return 0;
}

void sdxi_desc_decode(const struct sdxi_desc_new *desc, struct sdxi_desc_attrs *attrs)
{
	unsigned int csb_shift = ilog2(sizeof(struct sdxi_cst_blk));
	u64 csb_ptr = le64_to_cpu(desc->csb_ptr);
	u32 opcode = le32_to_cpu(desc->opcode);

	*attrs = (struct sdxi_desc_attrs) {
		.operation  = FIELD_GET(SDXI_DSC_SUBTYPE, opcode),
		.op_group   = FIELD_GET(SDXI_DSC_TYPE, opcode),
		.flags      = FIELD_GET(SDXI_DSC_FLAGS, opcode),
		.csb_handle = FIELD_GET(SDXI_DSC_CSB_PTR, csb_ptr) << csb_shift,
		.use_csb    = !FIELD_GET(SDXI_DSC_NP, csb_ptr),
	};

	struct sdxi_desc_unpacked unpacked;
	u8 quirks = QUIRK_LITTLE_ENDIAN | QUIRK_LSW32_IS_FIRST;
	unpack_fields(desc, sizeof(*desc), &unpacked, common_descriptor_fields, quirks);
	if (unpacked.type != attrs->op_group)
		kunit_fail_current_test("mismatch (%i vs %i)", unpacked.type, attrs->op_group);
	if (unpacked.csb_ptr << 5 != attrs->csb_handle)
		kunit_fail_current_test("mismatch (0x%llx vs 0x%llx)",
					unpacked.csb_ptr, attrs->csb_handle);
}

int sdxi_encode_copy(struct sdxi_desc_new *desc, const struct sdxi_copy *params)
{
	u64 csb_ptr;
	u32 opcode;
	__le32 size;
	int err;

	if ((err = sdxi_encode_size32(params->len, &size)))
		return err;

	// TODO: reject overlapping src and dst

	opcode = (FIELD_PREP(SDXI_DSC_VL, 1) |
		  FIELD_PREP(SDXI_DSC_SUBTYPE, SDXI_OP_DMAB_COPY) |
		  FIELD_PREP(SDXI_DSC_TYPE, SDXI_OPGRP_DMAB));

	csb_ptr = FIELD_PREP(SDXI_DSC_NP, 1);

	desc->copy = (struct sdxi_dsc_dmab_copy) {
		.opcode = cpu_to_le32(opcode),
		.size = size,
		.akey0 = cpu_to_le16(params->src_akey),
		.akey1 = cpu_to_le16(params->dst_akey),
		.addr0 = cpu_to_le64(params->src),
		.addr1 = cpu_to_le64(params->dst),
		.csb_ptr = cpu_to_le64(csb_ptr),
	};

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(sdxi_encode_copy);

int sdxi_encode_intr(struct sdxi_desc_new *desc,
		     const struct sdxi_intr *params)
{
	u64 csb_ptr;
	u32 opcode;

	opcode = (FIELD_PREP(SDXI_DSC_VL, 1) |
		  FIELD_PREP(SDXI_DSC_SUBTYPE, SDXI_DSC_OP_SUBTYPE_INTR) |
		  FIELD_PREP(SDXI_DSC_TYPE, SDXI_DSC_OP_TYPE_INTR));

	csb_ptr = FIELD_PREP(SDXI_DSC_NP, 1);

	desc->intr = (struct sdxi_dsc_intr) {
		.opcode = cpu_to_le32(opcode),
		.akey = cpu_to_le16(params->akey),
		.csb_ptr = cpu_to_le64(csb_ptr),
	};

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(sdxi_encode_intr);

int sdxi_encode_cxt_start(struct sdxi_desc_new *desc,
			  const struct sdxi_cxt_start *params)
{
	u16 cxt_start;
	u16 cxt_end;
	u64 csb_ptr;
	u32 opcode;

	opcode = (FIELD_PREP(SDXI_DSC_VL, 1) |
		  FIELD_PREP(SDXI_DSC_FE, 1) |
		  FIELD_PREP(SDXI_DSC_SUBTYPE, SDXI_DSC_OP_SUBTYPE_CXT_START_NM) |
		  FIELD_PREP(SDXI_DSC_TYPE, SDXI_DSC_OP_TYPE_ADMIN));

	cxt_start = params->range.cxt_start;
	cxt_end = params->range.cxt_end;

	csb_ptr = FIELD_PREP(SDXI_DSC_NP, 1);

	desc->cxt_start = (struct sdxi_dsc_cxt_start) {
		.opcode = cpu_to_le32(opcode),
		.cxt_start = cpu_to_le16(cxt_start),
		.cxt_end = cpu_to_le16(cxt_end),
		.csb_ptr = cpu_to_le64(csb_ptr),
	};

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(sdxi_encode_cxt_start);

int sdxi_encode_cxt_stop(struct sdxi_desc_new *desc,
			  const struct sdxi_cxt_stop *params)
{
	u16 cxt_start;
	u16 cxt_end;
	u64 csb_ptr;
	u32 opcode;

	opcode = (FIELD_PREP(SDXI_DSC_VL, 1) |
		  FIELD_PREP(SDXI_DSC_FE, 1) |
		  FIELD_PREP(SDXI_DSC_SUBTYPE, SDXI_DSC_OP_SUBTYPE_CXT_STOP) |
		  FIELD_PREP(SDXI_DSC_TYPE, SDXI_DSC_OP_TYPE_ADMIN));

	cxt_start = params->range.cxt_start;
	cxt_end = params->range.cxt_end;

	csb_ptr = FIELD_PREP(SDXI_DSC_NP, 1);

	desc->cxt_stop = (struct sdxi_dsc_cxt_stop) {
		.opcode = cpu_to_le32(opcode),
		.cxt_start = cpu_to_le16(cxt_start),
		.cxt_end = cpu_to_le16(cxt_end),
		.csb_ptr = cpu_to_le64(csb_ptr),
	};

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(sdxi_encode_cxt_stop);
