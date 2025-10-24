// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDXI descriptor encoding tests.
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */
#include <kunit/device.h>
#include <kunit/test-bug.h>
#include <kunit/test.h>
#include <linux/container_of.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/packing.h>
#include <linux/string.h>

#include "descriptor.h"

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

/*
 * Fields common to all SDXI descriptors in "unpacked" form.
 */
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

/*
 * While the "real" driver code uses bitfield macros (BIT, GENMASK) to
 * encode descriptors, these tests use the packing API to decode them.
 * Capturing the descriptor layout using PACKED_FIELD() from packing.h
 * is basically a copy-paste exercise since SDXI defines control
 * structure fields in terms of bit offsets. Eschewing the bitfield
 * constants such as SDXI_DSC_VL in the test code makes it possible
 * for the tests to detect any mistakes in defining them.
 */
static void sdxi_desc_unpack(struct sdxi_desc_unpacked *to,
		      const struct sdxi_desc *from)
{
	*to = (struct sdxi_desc_unpacked){};
	unpack_fields(from, sizeof(*from), to, common_descriptor_fields,
		      QUIRK_LITTLE_ENDIAN | QUIRK_LSW32_IS_FIRST);
}

static void desc_poison(struct sdxi_desc *d)
{
	memset(d, 0xff, sizeof(*d));
}

static void copy(struct kunit *t)
{
	struct sdxi_desc_unpacked unpacked;
	struct sdxi_copy copy = {};
	struct sdxi_desc desc = {};

	desc_poison(&desc);
	KUNIT_EXPECT_EQ(t, -EINVAL, sdxi_encode_copy(&desc, &copy));

	desc_poison(&desc);
	copy.len = SZ_4G + 1;
	KUNIT_EXPECT_EQ(t, -EINVAL, sdxi_encode_copy(&desc, &copy));

	desc_poison(&desc);
	copy.len = 1;
	KUNIT_EXPECT_EQ(t, 0, sdxi_encode_copy(&desc, &copy));

	desc_poison(&desc);
	copy.len = SZ_4G;
	KUNIT_EXPECT_EQ(t, 0, sdxi_encode_copy(&desc, &copy));
	KUNIT_EXPECT_EQ(t, SZ_4G - 1, le32_to_cpu(desc.copy.size));

	desc_poison(&desc);
	KUNIT_EXPECT_EQ(t, 0,
			sdxi_encode_copy(&desc,
					 &(struct sdxi_copy) {
						 .src = 0x1000,
						 .dst = 0x2000,
						 .len = 0x100,
						 .src_akey = 1,
						 .dst_akey = 2,
					 }));
	KUNIT_EXPECT_EQ(t, 0x1000, le64_to_cpu(desc.copy.addr0));
	KUNIT_EXPECT_EQ(t, 0x2000, le64_to_cpu(desc.copy.addr1));
	KUNIT_EXPECT_EQ(t, 0x100, 1 + le32_to_cpu(desc.copy.size));
	KUNIT_EXPECT_EQ(t, 1, le16_to_cpu(desc.copy.akey0));
	KUNIT_EXPECT_EQ(t, 2, le16_to_cpu(desc.copy.akey1));

	sdxi_desc_unpack(&unpacked, &desc);
	KUNIT_EXPECT_EQ(t, unpacked.vl, 1);
	KUNIT_EXPECT_EQ(t, unpacked.ch, 0);
	KUNIT_EXPECT_EQ(t, unpacked.subtype, SDXI_DSC_OP_SUBTYPE_COPY);
	KUNIT_EXPECT_EQ(t, unpacked.type, SDXI_DSC_OP_TYPE_DMAB);
	KUNIT_EXPECT_EQ(t, unpacked.csb_ptr, 0);
	KUNIT_EXPECT_EQ(t, unpacked.np, 1);
}

static void intr(struct kunit *t)
{
	struct sdxi_desc_unpacked unpacked;
	struct sdxi_intr intr = {
		.akey = 1234,
	};
	struct sdxi_desc desc;

	desc_poison(&desc);
	KUNIT_EXPECT_EQ(t, 0, sdxi_encode_intr(&desc, &intr));
	KUNIT_EXPECT_EQ(t, 1234, le16_to_cpu(desc.intr.akey));

	sdxi_desc_unpack(&unpacked, &desc);
	KUNIT_EXPECT_EQ(t, unpacked.vl, 1);
	KUNIT_EXPECT_EQ(t, unpacked.ch, 0);
	KUNIT_EXPECT_EQ(t, unpacked.subtype, SDXI_DSC_OP_SUBTYPE_INTR);
	KUNIT_EXPECT_EQ(t, unpacked.type, SDXI_DSC_OP_TYPE_INTR);
	KUNIT_EXPECT_EQ(t, unpacked.csb_ptr, 0);
	KUNIT_EXPECT_EQ(t, unpacked.np, 1);
}

static void cxt_start(struct kunit *t)
{
	struct sdxi_cxt_start start = {
		.range = sdxi_cxt_range(1, U16_MAX)
	};
	struct sdxi_desc desc = {};
	struct sdxi_desc_unpacked unpacked;

	KUNIT_EXPECT_EQ(t, 0, sdxi_encode_cxt_start(&desc, &start));

	/* Check op-specific fields. */
	KUNIT_EXPECT_EQ(t, 0, desc.cxt_start.vflags);
	KUNIT_EXPECT_EQ(t, 0, le16_to_cpu(desc.cxt_start.vf_num));
	KUNIT_EXPECT_EQ(t, 1, le16_to_cpu(desc.cxt_start.cxt_start));
	KUNIT_EXPECT_EQ(t, U16_MAX, le16_to_cpu(desc.cxt_start.cxt_end));
	KUNIT_EXPECT_EQ(t, 0, le64_to_cpu(desc.cxt_start.db_value));

	/*
	 * Check generic fields. Some flags have mandatory values
	 * according to the operation type.
	 */
	sdxi_desc_unpack(&unpacked, &desc);
	KUNIT_EXPECT_EQ(t, unpacked.vl, 1);
	KUNIT_EXPECT_EQ(t, unpacked.se, 0);
	KUNIT_EXPECT_EQ(t, unpacked.fe, 1);
	KUNIT_EXPECT_EQ(t, unpacked.ch, 0);
	KUNIT_EXPECT_EQ(t, unpacked.subtype, SDXI_DSC_OP_SUBTYPE_CXT_START_NM);
	KUNIT_EXPECT_EQ(t, unpacked.type, SDXI_DSC_OP_TYPE_ADMIN);
	KUNIT_EXPECT_EQ(t, unpacked.csb_ptr, 0);
	KUNIT_EXPECT_EQ(t, unpacked.np, 1);
}

static void cxt_stop(struct kunit *t)
{
	struct sdxi_cxt_stop stop = {
		.range = sdxi_cxt_range(1, U16_MAX)
	};
	struct sdxi_desc desc = {};
	struct sdxi_desc_unpacked unpacked;

	KUNIT_EXPECT_EQ(t, 0, sdxi_encode_cxt_stop(&desc, &stop));

	/* Check op-specific fields */
	KUNIT_EXPECT_EQ(t, 0, desc.cxt_stop.vflags);
	KUNIT_EXPECT_EQ(t, 0, le16_to_cpu(desc.cxt_stop.vf_num));
	KUNIT_EXPECT_EQ(t, 1, le16_to_cpu(desc.cxt_stop.cxt_start));
	KUNIT_EXPECT_EQ(t, U16_MAX, le16_to_cpu(desc.cxt_stop.cxt_end));

	/*
	 * Check generic fields. Some flags have mandatory values
	 * according to the operation type.
	 */
	sdxi_desc_unpack(&unpacked, &desc);
	KUNIT_EXPECT_EQ(t, unpacked.vl, 1);
	KUNIT_EXPECT_EQ(t, unpacked.se, 0);
	KUNIT_EXPECT_EQ(t, unpacked.fe, 1);
	KUNIT_EXPECT_EQ(t, unpacked.ch, 0);
	KUNIT_EXPECT_EQ(t, unpacked.subtype, SDXI_DSC_OP_SUBTYPE_CXT_STOP);
	KUNIT_EXPECT_EQ(t, unpacked.type, SDXI_DSC_OP_TYPE_ADMIN);
	KUNIT_EXPECT_EQ(t, unpacked.csb_ptr, 0);
	KUNIT_EXPECT_EQ(t, unpacked.np, 1);
}

static struct kunit_case generic_desc_tcs[] = {
	KUNIT_CASE(copy),
	KUNIT_CASE(intr),
	KUNIT_CASE(cxt_start),
	KUNIT_CASE(cxt_stop),
	{}
};

static int generic_desc_setup_device(struct kunit *t)
{
	struct device *dev = kunit_device_register(t, "sdxi-mock-device");

	KUNIT_ASSERT_NOT_ERR_OR_NULL(t, dev);
	t->priv = dev;
	return 0;
}

static struct kunit_suite generic_desc_ts = {
	.name = "Generic SDXI descriptor encoding",
	.test_cases = generic_desc_tcs,
	.init = generic_desc_setup_device,
};
kunit_test_suite(generic_desc_ts);

MODULE_DESCRIPTION("SDXI descriptor encoding tests");
MODULE_AUTHOR("Nathan Lynch");
MODULE_LICENSE("GPL");
