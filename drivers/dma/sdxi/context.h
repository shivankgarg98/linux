/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Header for sq and descriptor management
 *
 * Copyright (C) 2022 AMD, Inc. All rights reserved.
 *
 * Author: Wei Huang <wei.huang2@amd.com>
 *
 */

#ifndef __SDXI_SQ_H
#define __SDXI_SQ_H

#include "sdxi.h"

#define DESC_BUILD_TYPE(d, t, s)		\
	do {					\
		(d)->vl = 1;			\
		(d)->type = (t);		\
		(d)->subtype = (s);		\
	} while (0)

void build_dma_copy(struct sdxi_desc *desc, u32 size, u8 src_attr,
		    u8 dst_attr, u16 src_akey, u16 dst_akey,
		    u64 src_addr, u64 dst_dst);

#endif /* __SDXI_SQ_H */
