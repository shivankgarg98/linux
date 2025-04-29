#ifndef DMA_SDXI_VERSION_H
#define DMA_SDXI_VERSION_H

#include <linux/types.h>

typedef struct {
	u8 major;
	u8 minor;
} sdxi_version_t;

#define SDXI_VERSION_1_0 (sdxi_version_t){ .major = 1, .minor = 0, }
#define SDXI_VERSION_1_1 (sdxi_version_t){ .major = 1, .minor = 1, }

static inline u16 sdxi_version_as_u16(sdxi_version_t v)
{
	return (v.major << 8) | v.minor;
}

#define sdxi_version_compare(_name, _op)				\
	static inline bool sdxi_version_ ## _name(sdxi_version_t v1,	\
						  sdxi_version_t v2)	\
	{								\
		return sdxi_version_as_u16(v1) _op sdxi_version_as_u16(v2); \
	}

sdxi_version_compare(eq, ==)
sdxi_version_compare(gt, >)
sdxi_version_compare(ge, >=)
sdxi_version_compare(lt, <)
sdxi_version_compare(le, <=)

#undef sdxi_version_compare

#endif
