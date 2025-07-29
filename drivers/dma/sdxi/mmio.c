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
	PACKED_FIELD(31, 31, struct sdxi_mmio_ctl0, fn_pr),
	PACKED_FIELD(63, 32, struct sdxi_mmio_ctl0, fn_grp_id),
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
