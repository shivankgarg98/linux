/*
 * DMA engine support
 *
 * Copyright (C) 2022 AMD, Inc. All rights reserved.
 *
 * Author: Wei Huang <wei.huang2@amd.com>
 * Author: Sanjay R Mehta <sanju.mehta@amd.com>
 *
 */

#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>

#include "sdxi.h"
#include "sq.h"

#ifndef __SDXI_DMA_H
#define __SDXI_DMA_H

void sdxi_check_trans_status(struct sdxi_dma_chan *chan);

#endif /* __SDXI_DMA_H */
