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
#include <linux/io.h>
#include <linux/iomap.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci-ats.h>
#include <linux/pci.h>

#include "error.h"
#include "mmio.h"
#include "process.h"
#include "sdxi.h"

/* MMIO BARs */
#define MMIO_CTL_REGS_BAR		0x0
#define MMIO_DOORBELL_BAR		0x2

LIST_HEAD(sdxi_device_list);

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

	ret = sdxi_error_init(sdxi, pci_irq_vector(pdev, 0));
	if (ret)
		goto err_irq0_alloc;

	return 0;

err_irq0_alloc:
	pci_free_irq_vectors(pdev);
	return ret;
}

static void sdxi_pci_irq_exit(struct sdxi_dev *sdxi)
{
	sdxi_error_exit(sdxi);
	pci_free_irq_vectors(sdxi->pdev);
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
