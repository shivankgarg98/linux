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

#endif /* __SDXI_SQ_H */
