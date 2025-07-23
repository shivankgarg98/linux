// SPDX-License-Identifier: GPL-2.0-only
// SDXI error reporting.

#include <linux/array_size.h>
#include <linux/device.h>
#include <linux/dev_printk.h>
#include <linux/interrupt.h>
#include <linux/irqreturn.h>
#include <linux/minmax.h>
#include <linux/types.h>

#include "error.h"
#include "mmio.h"
#include "sdxi.h"

static void sdxi_print_err(struct sdxi_dev *sdxi, u64 err_rd)
{
	static const char * const sub_steps[] = {
		"Other or Internal Error",
		"Address Translation Failure",
		"Data Access Failure",
		"Data Validation Failure",
		"Unknown/Reserved Type",
	};
	static const char * const reactions[] = {
		"Informative Entry (nothing stopped)",
		"SDXI Context Stopped",
		"SDXI Function Stopped",
		"Unknown/Reserved Reaction",
	};
	struct sdxi_err *err;
	size_t index;

	index = err_rd % sdxi->err_log_num;
	err = &sdxi->err_log[index];

	if (err->vl) {
		sdxi_err(sdxi, "error log entry[%zu], MMIO_ERR_RD=%#llx:\n",
			 index, err_rd);
		sdxi_err(sdxi, "  step: 0x%x\n", err->step);
		sdxi_err(sdxi, "  type: 0x%x\n", err->type);
		sdxi_err(sdxi, "  cv: %x div: %x bv: %x\n", err->cv, err->div, err->bv);
		sdxi_err(sdxi, "  buff: 0x%x\n", err->buf);
		index = min(ARRAY_SIZE(sub_steps) - 1, (size_t)err->sub_step);
		sdxi_err(sdxi, "  sub_step: %s\n", sub_steps[index]);
		index = min(ARRAY_SIZE(reactions) - 1, (size_t)err->re);
		sdxi_err(sdxi, "  re: %s\n", reactions[index]);
		sdxi_err(sdxi, "  buff: 0x%x\n", err->buf);
		sdxi_err(sdxi, "  cxt_num: 0x%x\n", err->cxt_num);
		sdxi_err(sdxi, "  desc_idx: 0x%llx\n", err->desc_idx);
		sdxi_err(sdxi, "  err_class: 0x%x\n", err->err_class);
	} else {
		sdxi_err(sdxi, "Not a valid error log entry!\n");
	}
}

// Refer to "Error Log Processing by Software"
static irqreturn_t sdxi_irq_thread(int irq, void *data)
{
	struct sdxi_dev *sdxi = data;
	u64 err_sts;
	u64 write_index;
	u64 read_index;

	// 1. Check MMIO_ERR_STS and perform any required remediation.
	err_sts = sdxi_read64(sdxi, SDXI_MMIO_ERR_STS);
	if (!(err_sts & SDXI_MMIO_ERR_STS_STS_BIT))
		return IRQ_HANDLED;

	if (err_sts & SDXI_MMIO_ERR_STS_ERR_BIT) {
		// Assume this isn't recoverable; e.g. the error log
		// isn't configured correctly. Don't clear
		// SDXI_MMIO_ERR_STS before returning.
		sdxi_err(sdxi, "attempted but failed to log errors\n");
		sdxi_err(sdxi, "error log not functional\n");
		return IRQ_HANDLED;
	}

	if (err_sts & SDXI_MMIO_ERR_STS_OVF_BIT)
		sdxi_err(sdxi, "error log overflow, some entries lost\n");

	// 2. If MMIO_ERR_STS.sts is 1, then compute read_index.
	read_index = sdxi_read64(sdxi, SDXI_MMIO_ERR_RD);

	// 3. Clear MMIO_ERR_STS. The flags in this register are RW1C.
	sdxi_write64(sdxi, SDXI_MMIO_ERR_STS,
		     SDXI_MMIO_ERR_STS_STS_BIT |
		     SDXI_MMIO_ERR_STS_OVF_BIT |
		     SDXI_MMIO_ERR_STS_ERR_BIT);

	// 4. Compute write_index.
	write_index = sdxi_read64(sdxi, SDXI_MMIO_ERR_WRT);

	// 5. If the indexes are equal then exit.
	if (read_index == write_index)
		return IRQ_HANDLED;

	// 6. While read_index < write_index...
	while (read_index < write_index) {

		// 7. and 8. Compute the real ring buffer index from
		// read_index and process the entry.
		sdxi_print_err(sdxi, read_index);

		// 9. Advance read_index.
		++read_index;

		// 10. Return to step 6.
	}

	// 11. Write read_index to MMIO_ERR_RD.
	sdxi_write64(sdxi, SDXI_MMIO_ERR_RD, read_index);

	return IRQ_HANDLED;
}

static irqreturn_t sdxi_irq_handler(int irq, void *data)
{
	return IRQ_WAKE_THREAD;
}

int sdxi_error_init(struct sdxi_dev *sdxi, unsigned int irq)
{
	struct sdxi_mmio_ctl0 ctl0 = sdxi_get_ctl0(sdxi);
	int err;

	sdxi_write64(sdxi, SDXI_MMIO_ERR_WRT, 0);
	sdxi_write64(sdxi, SDXI_MMIO_ERR_RD, 0);

	err = request_threaded_irq(irq, sdxi_irq_handler, sdxi_irq_thread, 0,
				   SDXI_DRV_NAME, sdxi);
	if (err)
		return err;

	ctl0.fn_err_intr_en = 1;
	sdxi_set_ctl0(sdxi, ctl0);
	sdxi->err_irq.vector = irq;
	return 0;
}

void sdxi_error_exit(struct sdxi_dev *sdxi)
{
	sdxi_write64(sdxi, SDXI_MMIO_ERR_CFG, 0);
	free_irq(sdxi->err_irq.vector, sdxi);
}
