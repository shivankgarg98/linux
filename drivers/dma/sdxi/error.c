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

static void sdxi_print_err(struct sdxi_dev *sdxi, struct sdxi_err *err)
{
	int index;
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

	if (err->vl) {
		sdxi_err(sdxi, "error log entry:");
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

static void sdxi_handle_err(struct sdxi_dev *sdxi)
{
	u64 read_ptr, write_ptr, offset;
	struct sdxi_err *err_entry;

	read_ptr = sdxi_read64(sdxi, SDXI_MMIO_ERR_RD);
	write_ptr = sdxi_read64(sdxi, SDXI_MMIO_ERR_WRT);

	while (read_ptr < write_ptr) {
		offset = (read_ptr * 64) % ((sdxi->err_log_num + 1) * 4096);
		err_entry = (struct sdxi_err *)sdxi->err_log + offset;
		sdxi_print_err(sdxi, err_entry);
		read_ptr++;
	}

	sdxi_write64(sdxi, SDXI_MMIO_ERR_RD, read_ptr);
}

static void sdxi_do_cmd_complete(unsigned long data)
{
	struct sdxi_tasklet_data *tdata = (void *)data;
	struct sdxi_cmd *cmd = tdata->cmd;

	if (cmd && cmd->sdxi_cmd_callback)
		cmd->sdxi_cmd_callback(cmd->data, cmd->ret);
}

static irqreturn_t sdxi_irq_thread(int irq, void *data)
{
	struct sdxi_dev *sdxi = data;

	while (sdxi_read64(sdxi, SDXI_MMIO_ERR_STS) & SDXI_MMIO_ERR_STS_STS_BIT) {
		sdxi_handle_err(sdxi);

		// The flags in this register are RW1C.
		sdxi_write64(sdxi, SDXI_MMIO_ERR_STS,
			     SDXI_MMIO_ERR_STS_STS_BIT |
			     SDXI_MMIO_ERR_STS_OVF_BIT |
			     SDXI_MMIO_ERR_STS_ERR_BIT);
	}

	sdxi_do_cmd_complete((ulong)&sdxi->tdata);

	return IRQ_HANDLED;
}

static irqreturn_t sdxi_irq_handler(int irq, void *data)
{
	return IRQ_WAKE_THREAD;
}

int sdxi_error_init(struct sdxi_dev *sdxi, unsigned int irq)
{
	int err;

	err = request_threaded_irq(irq, sdxi_irq_handler, sdxi_irq_thread, 0,
				   SDXI_DRV_NAME, sdxi);
	if (err)
		return err;

	sdxi->err_irq.vector = irq;
	return 0;
}

void sdxi_error_exit(struct sdxi_dev *sdxi)
{
	sdxi_write64(sdxi, SDXI_MMIO_ERR_CFG, 0);
	free_irq(sdxi->err_irq.vector, sdxi);
}
