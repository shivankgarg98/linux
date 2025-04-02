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

#define DESC_ADM_BUILD_VF(d, vf, vf_num)				\
	do {								\
		if (vf) {						\
			(d)->body[0] |= ((vf_num) & 0xFFFF) << 16;	\
			(d)->body[0] |= (1 << 15);			\
		}							\
	} while (0)

#define DESC_ADM_BUILD_CXT(d, start, end)		\
	do {						\
		(d)->body[1] |= ((start) & 0xFFFF);	\
		(d)->body[1] |= ((end) & 0xFFFF) << 16;	\
	} while (0)

#define DESC_ADM_BUILD_AKEY(d, num, mask)			\
	do {							\
		(d)->body[2] |= ((num) & 0xFFFF);		\
		(d)->body[2] |= ((mask) & 0xFFFF) << 16;	\
	} while (0)

void build_admin_start_new(struct sdxi_desc *desc, bool vf, u16 vf_num,
			   u16 cxt_start, u16 cxt_end, u64 doorbell);
void build_admin_stop_new(struct sdxi_desc *desc, bool vf, u16 vf_num,
			   u16 cxt_start, u16 cxt_end, u64 doorbell);
void build_dma_copy(struct sdxi_desc *desc, u32 size, u8 src_attr,
		    u8 dst_attr, u16 src_akey, u16 dst_akey,
		    u64 src_addr, u64 dst_dst, u64 comp_ptr);
u64 sdxi_sq_submit_desc(struct sdxi_sq *sq, struct sdxi_desc *desc, bool cst,
			u64 init_signal);

#endif /* __SDXI_SQ_H */
