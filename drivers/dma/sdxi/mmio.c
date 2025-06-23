#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/packing.h>
#include <linux/types.h>

#include "mmio.h"

enum {
	SDXI_PACKING_QUIRKS = QUIRK_LITTLE_ENDIAN | QUIRK_LSW32_IS_FIRST,
};

static const struct packed_field_u8 ctl0_fields[] = {
	PACKED_FIELD(1, 0, struct sdxi_mmio_ctl0, fn_gsr),
	PACKED_FIELD(2, 2, struct sdxi_mmio_ctl0, fn_pasid_vl),
	PACKED_FIELD(4, 4, struct sdxi_mmio_ctl0, fn_err_intr_en),
	PACKED_FIELD(27, 8, struct sdxi_mmio_ctl0, fn_pasid),
	PACKED_FIELD(63, 32, struct sdxi_mmio_ctl0, fn_grp_id),
};

static const struct packed_field_u8 ctl2_fields[] = {
	PACKED_FIELD(3, 0, struct sdxi_mmio_ctl2, max_buffer),
	PACKED_FIELD(15, 12, struct sdxi_mmio_ctl2, max_akey_sz),
	PACKED_FIELD(31, 16, struct sdxi_mmio_ctl2, max_cxt),
	PACKED_FIELD(63, 32, struct sdxi_mmio_ctl2, obp_000_avl),
};

static const struct packed_field_u8 cxt_l2_fields[] = {
	PACKED_FIELD(63, 12, struct sdxi_mmio_cxt_l2, lv02_ptr),
};

static const struct packed_field_u8 rkey_fields[] = {
	PACKED_FIELD(0, 0, struct sdxi_mmio_rkey, en),
	PACKED_FIELD(4, 1, struct sdxi_mmio_rkey, sz),
	PACKED_FIELD(63, 12, struct sdxi_mmio_rkey, ptr),
};

#define define_reg_commit_func(_regname, _offset, _field_struct)		\
void sdxi_mmio_##_regname##_commit(void __iomem *base,				\
				   const struct sdxi_mmio_##_regname *unpacked)	\
{									\
	u64 val = 0;							\
	pack_fields(&val, sizeof(val), unpacked,			\
		    _field_struct, SDXI_PACKING_QUIRKS);		\
	iowrite64(val, base + _offset);					\
}

#define define_reg_read_func(_regname, _offset, _field_struct)		\
void sdxi_mmio_##_regname##_read(void __iomem *base,			\
				 struct sdxi_mmio_##_regname *unpacked)	\
{								\
	u64 val = ioread64(base + _offset);			\
	unpack_fields(&val, sizeof(val), unpacked,		\
		      _field_struct, SDXI_PACKING_QUIRKS);	\
}

#define define_reg_access_funcs_rw(_regname, _offset, _field_struct) \
	define_reg_read_func(_regname, _offset, _field_struct)       \
	define_reg_commit_func(_regname, _offset, _field_struct)

define_reg_access_funcs_rw(ctl0,   SDXI_MMIO_CTL0,   ctl0_fields)
define_reg_access_funcs_rw(ctl2,   SDXI_MMIO_CTL2,   ctl2_fields)
define_reg_access_funcs_rw(cxt_l2, SDXI_MMIO_CXT_L2, cxt_l2_fields)
define_reg_access_funcs_rw(rkey,   SDXI_MMIO_RKEY,   rkey_fields)
