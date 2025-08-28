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

struct sdxi_cxt;
struct sdxi_dev;

enum sdxi_cxt_id {
	SDXI_ADMIN_CXT_ID = 0,
	SDXI_DMA_CXT_ID = 1,
	SDXI_ANY_CXT_ID,
};

/* Submission Queue */
struct sdxi_sq *sdxi_sq_alloc(struct sdxi_cxt *cxt, int ring_size);
struct sdxi_sq *sdxi_sq_alloc_default(struct sdxi_cxt *cxt);
void sdxi_sq_free(struct sdxi_sq *sq);

/* Context Control */
void sdxi_cxt_free(struct sdxi_cxt *cxt);
struct sdxi_cxt *sdxi_working_cxt_init(struct sdxi_dev *sdxi,
				       enum sdxi_cxt_id);
void sdxi_working_cxt_exit(struct sdxi_cxt *cxt);

#endif /* __SDXI_SQ_H */
