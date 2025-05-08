// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDXI PCI device code
 *
 * Copyright (C) 2022 AMD, Inc. All rights reserved.
 *
 * Author: Wei Huang <wei.huang2@amd.com>
 */

#define pr_fmt(fmt)     "SDXI: " fmt
#define dev_fmt(fmt)    pr_fmt(fmt)

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/io.h>
#include <linux/iomap.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci-ats.h>
#include <linux/pci.h>

#include "pci.h"
#include "process.h"

LIST_HEAD(sdxi_device_list);

// Use this to insert artificial delays between critical register and
// data structure updates to more easily recreate issues with function
// and context init/exit.
static unsigned int update_delay_ms;
module_param(update_delay_ms, uint, 0644);
MODULE_PARM_DESC(iowrite_delay_ms, "Artificial delay to insert after critical data structure updates");

// Delay for up to one second, longer doesn't seem useful.
void sdxi_delay(void)
{
	might_sleep();

	if (update_delay_ms)
		fsleep(min(update_delay_ms, 1000) * 1000UL);
}

enum sdxi_reg {
	SDXI_MMIO_CTL0       = 0x00000,
	SDXI_MMIO_CTL2       = 0x00010,
	SDXI_MMIO_STS0       = 0x00100,
	SDXI_MMIO_CAP0       = 0x00200,
	SDXI_MMIO_CAP1       = 0x00208,
	SDXI_MMIO_VER        = 0x00210,
	SDXI_MMIO_CXT_L2     = 0x10000,
	SDXI_MMIO_RKEY       = 0x10100,
	SDXI_MMIO_ERR_CTL    = 0x20000,
	SDXI_MMIO_ERR_STS    = 0x20008,
	SDXI_MMIO_ERR_CFG    = 0x20010,
	SDXI_MMIO_ERR_WRT    = 0x20020,
	SDXI_MMIO_ERR_RD     = 0x20028,
};

enum {
	//// SDXI_MMIO_CTL0 bit definitions

	// Initiate function state transitions
	SDXI_MMIO_CTL0_FN_GSR = GENMASK_ULL(1, 0),

	//// SDXI_MMIO_STS0 bit definitions

	// Overall function state.
	SDXI_MMIO_STS0_FN_GSV = GENMASK_ULL(2, 0),

	//// SDXI_MMIO_CAP0 bit definitions

	// SDXI function identifier, unique within its function group.
	SDXI_MMIO_CAP0_SFUNC = GENMASK_ULL(15, 0),

	// Encoded address stride between doorbell sections.
	SDXI_MMIO_CAP0_DB_STRIDE = GENMASK_ULL(22, 20),

	// Encoded maximum descriptor ring size for any context in
	// this function.
	SDXI_MMIO_CAP0_MAX_DS_RING_SZ = GENMASK_ULL(28, 24),

	//// SDXI_MMIO_CAP1 bit definitions
	
	//// SDXI_MMIO_CXT_L2 bit definitions

	// Pointer to level 2 context table (4KB-aligned).
	SDXI_MMIO_CXT_L2_PTR = GENMASK_ULL(63, 12),

	//// SDXI_MMIO_ERR_CFG bit definitions

	// Pointer to error log buffer (4KB-aligned).
	SDXI_MMIO_ERR_CFG_PTR = GENMASK_ULL(63, 12),

	// Encoded error log buffer size.
	SDXI_MMIO_ERR_CFG_SZ  = GENMASK_ULL(5, 1),

	// Error log enable.
	SDXI_MMIO_ERR_CFG_EN  = BIT_ULL(0),

	//// SDXI_MMIO_RKEY bit definitions

	// Pointer to RKey table (4KB-aligned).
	SDXI_MMIO_RKEY_PTR = GENMASK_ULL(63, 12),
	// Encoded RKey table size.
	SDXI_MMIO_RKEY_SZ = GENMASK_ULL(4, 1),
	// RKey functionality enabled, also functions as validity bit
	// for ptr and sz.
	SDXI_MMIO_RKEY_EN = BIT_ULL(0),
	

	//// SDXI_MMIO_ERR_CTL bit definitions

	// When set to 1, an interrupt is signaled when hardware
	// transitions MMIO_ERR_STS.sts from 0 to 1. Otherwise no
	// interrupt is generated.
	SDXI_MMIO_ERR_CTL_EN = BIT_ULL(0),

	//// SDXI_MMIO_ERR_STS bit definitions.

	// The device has attempted to log an error. Other bits
	// indicate whether this was successful.
	SDXI_MMIO_ERR_STS_STS_BIT = BIT_ULL(0),

	// Error log overflowed and some error information was discarded.
	SDXI_MMIO_ERR_STS_OVF_BIT = BIT_ULL(1),

	// The device has encountered an error while attempting to log
	// an error; some error information has been discarded.
	SDXI_MMIO_ERR_STS_ERR_BIT = BIT_ULL(3),
};

static u64 sdxi_read64(const struct sdxi_dev *sdxi, enum sdxi_reg reg)
{
	return ioread64(sdxi->ctrl_regs + reg);
}

static void sdxi_write64(struct sdxi_dev *sdxi, enum sdxi_reg reg, u64 val)
{
	iowrite64(val, sdxi->ctrl_regs + reg);
	sdxi_delay();
}

static void sdxi_print_err(struct sdxi_dev *sdxi, struct sdxi_err *err)
{
	struct device *dev = &sdxi->pdev->dev;
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
		dev_err(dev, "error log entry:");
		dev_err(dev, "  step: 0x%x\n", err->step);
		dev_err(dev, "  type: 0x%x\n", err->type);
		dev_err(dev, "  cv: %x div: %x bv: %x\n", err->cv, err->div, err->bv);
		dev_err(dev, "  buff: 0x%x\n", err->buf);
		index = min(ARRAY_SIZE(sub_steps) - 1, (size_t)err->sub_step);
		dev_err(dev, "  sub_step: %s\n", sub_steps[index]);
		index = min(ARRAY_SIZE(reactions) - 1, (size_t)err->re);
		dev_err(dev, "  re: %s\n", reactions[index]);
		dev_err(dev, "  buff: 0x%x\n", err->buf);
		dev_err(dev, "  cxt_num: 0x%x\n", err->cxt_num);
		dev_err(dev, "  desc_idx: 0x%llx\n", err->desc_idx);
		dev_err(dev, "  err_class: 0x%x\n", err->err_class);
	} else {
		dev_err(dev, "Not a valid error log entry!\n");
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

static int sdxi_pci_irq_init(struct sdxi_dev *sdxi)
{
	struct pci_dev *pdev = sdxi->pdev;
	struct device *dev = &pdev->dev;
	int msi_count;
	int ret;

	/* 1st irq for error + 1 for each context */
	msi_count = sdxi->max_cxts + 1;

	ret = pci_alloc_irq_vectors(pdev, 1, msi_count,
				    PCI_IRQ_MSI | PCI_IRQ_MSIX);
	if (ret < 0) {
		dev_info(dev, "alloc MSI/MSI-X vectors failed\n");
		return ret;
	}

	sdxi->irq_count = ret;
	sdxi->err_irq.vector = pci_irq_vector(pdev, 0);
	/* setup err log interrupt handler */
	ret = request_threaded_irq(sdxi->err_irq.vector,
				   sdxi_irq_handler, sdxi_irq_thread, 0,
				   SDXI_DRV_NAME, sdxi);
	if (ret) {
		dev_err(dev, "cannot alloc irq handler for error irq\n");
		goto err_irq0_alloc;
	}

	return 0;

err_irq0_alloc:
	pci_free_irq_vectors(pdev);
	return ret;
}

static void sdxi_pci_irq_exit(struct sdxi_dev *sdxi)
{
	struct pci_dev *pdev = sdxi->pdev;

	free_irq(sdxi->err_irq.vector, sdxi);
	/* NB: free context IRQs */
	pci_free_irq_vectors(pdev);
}

static void sdxi_pci_parse_cap(struct sdxi_dev *sdxi)
{
	union mmio_cap1_reg cap1;
	u64 cap0;

	cap0 = sdxi_read64(sdxi, SDXI_MMIO_CAP0);
	sdxi->max_ring_entries = 1ULL << (FIELD_GET(SDXI_MMIO_CAP0_MAX_DS_RING_SZ, cap0) + 10);
	sdxi->db_stride = 1UL << (FIELD_GET(SDXI_MMIO_CAP0_DB_STRIDE, cap0) + 12);
	sdxi->sfunc = FIELD_GET(SDXI_MMIO_CAP0_SFUNC, cap0);

	/* CAP1 */
	cap1.data = sdxi_read64(sdxi, SDXI_MMIO_CAP1);

	sdxi->max_akeys = 1 << (cap1.max_akey_sz + 8);
	sdxi->max_cxts = cap1.max_cxt + 1;
	sdxi->op_grp_cap = cap1.opb_000_cap;

	pr_info("Device 0x%04x found [cap0=0x%llx, cap1=0x%llx]\n",
		sdxi->sfunc, cap0, cap1.data);
}

static int sdxi_pci_map(struct sdxi_dev *sdxi)
{
	struct pci_dev *pdev = sdxi->pdev;
	struct device *dev = &pdev->dev;
	int bars, ret;

	bars = 1 << MMIO_CTL_REGS_BAR | 1 << MMIO_DOORBELL_BAR;
	ret = pcim_iomap_regions(pdev, bars, SDXI_DRV_NAME);
	if (ret) {
		dev_err(dev, "pcim_iomap_regions failed (%d)\n", ret);
		return ret;
	}

	sdxi->dbs_bar = pci_resource_start(pdev, MMIO_DOORBELL_BAR);

	// FIXME: pcim_iomap_table may return NULL, and it's deprecated.
	sdxi->ctrl_regs = pcim_iomap_table(pdev)[MMIO_CTL_REGS_BAR];
	sdxi->dbs = pcim_iomap_table(pdev)[MMIO_DOORBELL_BAR];
	if (!sdxi->ctrl_regs || !sdxi->dbs) {
		dev_err(dev, "pcim_iomap_table failed\n");
		pcim_iounmap_regions(pdev, bars);
		return -EINVAL;
	}

	return 0;
}

static void sdxi_pci_unmap(struct sdxi_dev *sdxi)
{
	struct pci_dev *pdev = sdxi->pdev;

	pcim_iounmap(pdev, sdxi->ctrl_regs);
	pcim_iounmap(pdev, sdxi->dbs);
}

static int sdxi_pci_init(struct sdxi_dev *sdxi)
{
	struct pci_dev *pdev = sdxi->pdev;
	struct device *dev = &pdev->dev;
	int dma_bits = 64;
	int ret;

	ret = pcim_enable_device(pdev);
	if (ret) {
		dev_err(dev, "pcim_enbale_device failed\n");
		return ret;
	}

	pci_set_master(pdev);
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(dma_bits));
	if (ret) {
		dev_err(dev, "failed to set DMA mask & coherent bits\n");
		return ret;
	}

	ret = sdxi_pci_map(sdxi);
	if (ret) {
		dev_err(dev, "failed to map device IO resources\n");
		return ret;
	}

	ret = sdxi_pci_irq_init(sdxi);
	if (ret) {
		sdxi_pci_unmap(sdxi);
		return ret;
	}

	sdxi_pci_parse_cap(sdxi);

	return 0;
}

static void sdxi_pci_exit(struct sdxi_dev *sdxi)
{
	sdxi_pci_irq_exit(sdxi);
	sdxi_pci_unmap(sdxi);
}

typedef enum sdxi_fn_gsv {
	SDXI_GSV_STOP,
	SDXI_GSV_INIT,
	SDXI_GSV_ACTIVE,
	SDXI_GSV_STOPG_SF,
	SDXI_GSV_STOPG_HD,
	SDXI_GSV_ERROR,
} sdxi_fn_gsv_t;

typedef enum sdxi_fn_gsr {
	SDXI_GSRV_RESET,
	SDXI_GSRV_STOP_SF,
	SDXI_GSRV_STOP_HD,
	SDXI_GSRV_ACTIVE,
} sdxi_fn_gsr_t;


static sdxi_fn_gsv_t sdxi_dev_gsv(const struct sdxi_dev *sdxi)
{
	return (sdxi_fn_gsv_t)FIELD_GET(SDXI_MMIO_STS0_FN_GSV,
					sdxi_read64(sdxi, SDXI_MMIO_STS0));
}

// Get the device to the GSV_STOP state.
static int sdxi_dev_stop(struct sdxi_dev *sdxi)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(1000);

	do {
		u64 reset = FIELD_PREP(SDXI_MMIO_CTL0_FN_GSR, SDXI_GSRV_RESET);
		u64 stop = FIELD_PREP(SDXI_MMIO_CTL0_FN_GSR, SDXI_GSRV_STOP_SF);
		sdxi_fn_gsv_t status = sdxi_dev_gsv(sdxi);

		switch (status) {
		case SDXI_GSV_ACTIVE:
			sdxi_write64(sdxi, SDXI_MMIO_CTL0, stop);
			break;
		case SDXI_GSV_ERROR:
		case SDXI_GSV_STOP:
			// Perform a reset command and clear all other configuration
			// from MMIO_CTL0 at least once. If the function is already in
			// GSV_STOP the command will be ignored.
			sdxi_write64(sdxi, SDXI_MMIO_CTL0, reset);
			return 0;
			break;
		case SDXI_GSV_INIT:
		case SDXI_GSV_STOPG_SF:
		case SDXI_GSV_STOPG_HD:
			// transitional states, wait
			fsleep(1000);
			break;
		default:
			dev_err(&sdxi->pdev->dev, "unknown gsv %u, giving up\n", status);
			return -EIO;
			break;
		}
	} while (time_before(jiffies, deadline));

	dev_err(&sdxi->pdev->dev, "stop attempt timed out, current status %u\n",
		sdxi_dev_gsv(sdxi));
	return -ETIMEDOUT;
}

static int sdxi_pci_enable(struct sdxi_dev *sdxi)
{
	struct device *dev = &sdxi->pdev->dev;
	u64 ctrl2, status;
	union mmio_ctl0_reg ctl0_reg;
	int err;

	if ((err = sdxi_dev_stop(sdxi)))
		return err;

	/* l2 table */
	sdxi->l2_dma = dma_map_single(dev, sdxi->l2_table, L2_TABLE_SIZE,
				      DMA_TO_DEVICE);
	if (dma_mapping_error(dev, sdxi->l2_dma))
		return -ENOMEM;
	sdxi_write64(sdxi, SDXI_MMIO_CXT_L2,
		     FIELD_PREP(SDXI_MMIO_CXT_L2_PTR, sdxi->l2_dma >> 12));

	/* err log */
	sdxi->err_log_dma = dma_map_single(dev, sdxi->err_log,
					   sdxi->err_log_num * sizeof(struct sdxi_err),
					   DMA_FROM_DEVICE);
	if (dma_mapping_error(dev, sdxi->err_log_dma))
		goto unmap_l2;

	sdxi_write64(sdxi, SDXI_MMIO_ERR_CFG,
		     FIELD_PREP(SDXI_MMIO_ERR_CFG_PTR, sdxi->err_log_dma >> 12) |
		     FIELD_PREP(SDXI_MMIO_ERR_CFG_SZ, sdxi->err_log_num >> 6) |
		     FIELD_PREP(SDXI_MMIO_ERR_CFG_EN, 1));

	/* Signal interrupt on new error log entry */
	sdxi_write64(sdxi, SDXI_MMIO_ERR_CTL,
		     FIELD_PREP(SDXI_MMIO_ERR_CTL_EN, 1));

	/* enable device */
	ctl0_reg.data = sdxi_read64(sdxi, SDXI_MMIO_CTL0);
	ctl0_reg.fn_gsr = GSRV_ACTIVE;
	ctl0_reg.fn_err_intr_en = 1;
	sdxi_write64(sdxi, SDXI_MMIO_CTL0, ctl0_reg.data);

	ctrl2 = sdxi_read64(sdxi, SDXI_MMIO_CTL2);
	ctrl2 &= 0xFFFFFFFF0000FFFFULL;
	ctrl2 |= (sdxi->max_cxts << 16) & 0x00000000FFFF0000ULL;
	ctrl2 &= 0x00000000FFFFFFFFULL;
	ctrl2 |= (uint64_t)sdxi->op_grp_cap << 32;
	sdxi_write64(sdxi, SDXI_MMIO_CTL2, ctrl2);

	status = sdxi_read64(sdxi, SDXI_MMIO_STS0);

	pr_debug("function info:\n"
		 "  err log addr: v=0x%p:d=0x%llx\n"
		 "  func status:  0x%lx\n"
		 "  ctrl2:        0x%llx\n",
		 sdxi->err_log, sdxi->err_log_dma & ~0x1,
		 (unsigned long)status, (unsigned long long)ctrl2);

	return 0;

unmap_l2:
	dma_unmap_single(dev, sdxi->l2_dma, L2_TABLE_SIZE, DMA_TO_DEVICE);
	return -ENOMEM;
}

static void sdxi_dump_errlog(struct sdxi_dev *sdxi)
{
	(void)sdxi_read64(sdxi, SDXI_MMIO_ERR_CTL);
	(void)sdxi_read64(sdxi, SDXI_MMIO_ERR_STS);
	(void)sdxi_read64(sdxi, SDXI_MMIO_ERR_CFG);
	(void)sdxi_read64(sdxi, SDXI_MMIO_ERR_WRT);
	(void)sdxi_read64(sdxi, SDXI_MMIO_ERR_RD);

	// FIXME: is this function supposed to log these regs or
	// something? Leaving it for now, just in case we're depending
	// on some side effect of the reads. -ntl
}

static void sdxi_pci_disable(struct sdxi_dev *sdxi)
{
	struct device *dev = &sdxi->pdev->dev;
	union mmio_ctl0_reg ctl0_reg;

	sdxi_dump_errlog(sdxi);

	/* disable device */
	ctl0_reg.data = sdxi_read64(sdxi, SDXI_MMIO_CTL0);
	ctl0_reg.fn_gsr = GSRV_STOP_SF;
	sdxi_write64(sdxi, SDXI_MMIO_CTL0, ctl0_reg.data);

	dma_unmap_single(dev, sdxi->l2_dma, L2_TABLE_SIZE, DMA_TO_DEVICE);
	dma_unmap_single(dev, sdxi->err_log_dma,
			 sdxi->err_log_num * sizeof(struct sdxi_err),
			 DMA_FROM_DEVICE);
}

static struct sdxi_dev *sdxi_device_alloc(struct device *dev)
{
	struct sdxi_dev *sdxi;
	int entries;

	sdxi = kzalloc(sizeof(*sdxi), GFP_KERNEL);
	if (!sdxi)
		return NULL;

	sdxi->l2_table = kzalloc(sizeof(*sdxi->l2_table), GFP_KERNEL);
	if (!sdxi->l2_table)
		goto l2_fail;

	/* error log */
	entries = DEFAULT_ERR_LOG_NUM;
	sdxi->err_log = kcalloc(entries, sizeof(struct sdxi_err), GFP_KERNEL);
	if (!sdxi->err_log)
		goto err_log_fail;
	sdxi->err_log_num = entries;

	mutex_init(&sdxi->cxt_lock);
	INIT_LIST_HEAD(&sdxi->cxt_list);
	list_add_tail(&sdxi->list, &sdxi_device_list);

	return sdxi;

err_log_fail:
	kfree(sdxi->l2_table);
l2_fail:
	kfree(sdxi);
	return NULL;
}

static void sdxi_device_free(struct sdxi_dev *sdxi)
{
	list_del(&sdxi->list);
	kfree(sdxi->err_log);
	kfree(sdxi->l2_table);
	kfree(sdxi);
}

static int sdxi_pci_probe(struct pci_dev *pdev,
			  const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;
	struct sdxi_dev *sdxi;
	int ret;

	sdxi = sdxi_device_alloc(dev);
	if (!sdxi) {
		dev_err(dev, "failed to allocate sdxi device\n");
		return -ENOMEM;
	}

	sdxi->pdev = pdev;
	pci_set_drvdata(pdev, sdxi);

	ret = sdxi_pci_init(sdxi);
	if (ret)
		goto err_pci_init;

	ret = sdxi_pci_enable(sdxi);
	if (ret)
		goto err_pci_enable;

	ret = sdxi_iommu_device_init(sdxi);
	if (ret)
		goto err_iommu_init;

	ret = sdxi_device_init(sdxi);
	if (ret)
		goto err_dev_init;

	return 0;

err_dev_init:
	sdxi_iommu_device_exit(sdxi);
err_iommu_init:
	sdxi_pci_disable(sdxi);
err_pci_enable:
	sdxi_pci_exit(sdxi);
err_pci_init:
	sdxi_device_free(sdxi);

	return ret;
}

static void sdxi_pci_remove(struct pci_dev *pdev)
{
	struct sdxi_dev *sdxi = pci_get_drvdata(pdev);

	sdxi_device_exit(sdxi);
	sdxi_iommu_device_exit(sdxi);
	sdxi_pci_disable(sdxi);
	sdxi_pci_exit(sdxi);
	sdxi_device_free(sdxi);
}

#ifdef CONFIG_PM_SLEEP
static int sdxi_pci_suspend(struct device *dev)
{
	/* place holder, need to expand */
	sdxi_iommu_suspend(NULL);

	return 0;
}

static int sdxi_pci_resume(struct device *dev)
{
	/* place holder, need to expand */
	sdxi_iommu_resume(NULL);

	return 0;
}

static const struct dev_pm_ops sdxi_pci_pm_ops = {
	.suspend	= sdxi_pci_suspend,
	.resume		= sdxi_pci_resume,
};
#endif /* CONFIG_PM_SLEEP */

static const struct pci_device_id sdxi_id_table[] = {
	{ PCI_DEVICE_CLASS(PCI_CLASS_ACCEL_SDXI, 0xffffff) },
	{0, }
};
MODULE_DEVICE_TABLE(pci, sdxi_id_table);

static struct pci_driver sdxi_driver = {
	.name = "sdxi",
	.id_table = sdxi_id_table,
	.probe = sdxi_pci_probe,
	.remove = sdxi_pci_remove,
	.driver = {
		.pm = &sdxi_pci_pm_ops,
	},
};

static int __init sdxi_module_init(void)
{
	int rc;

	rc = pci_register_driver(&sdxi_driver);
	if (rc)
		return rc;

	rc = sdxi_chardev_init();

	return rc;
}

static void __exit sdxi_module_exit(void)
{
	sdxi_chardev_exit();
	pci_unregister_driver(&sdxi_driver);
}

MODULE_AUTHOR("Wei Huang <wei.huang2@amd.com>");
MODULE_DESCRIPTION(SDXI_DRV_DESC);
MODULE_LICENSE("GPL v2");
module_init(sdxi_module_init);
module_exit(sdxi_module_exit);
