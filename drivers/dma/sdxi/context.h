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

/* Submission Queue */
struct sdxi_sq *sdxi_sq_alloc(struct sdxi_cxt *cxt, int ring_size);
struct sdxi_sq *sdxi_sq_alloc_default(struct sdxi_cxt *cxt);
void sdxi_sq_free(struct sdxi_sq *sq);

#endif /* __SDXI_SQ_H */
