/*
 * SDXI PCI device code
 *
 * Copyright (C) 2022 AMD, Inc. All rights reserved.
 *
 * Author: Wei Huang <wei.huang2@amd.com>
 */

#define pr_fmt(fmt)     "SDXI: " fmt
#define dev_fmt(fmt)    pr_fmt(fmt)

#include <linux/module.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/pci-ats.h>
#include <linux/io.h>
#include <linux/iomap.h>

#include "sdxi.h"
#include "pci.h"
#include "process.h"

LIST_HEAD(sdxi_device_list);

u64 reg_read64(void __iomem *addr)
{
        u64 low, high;

        low = ioread32(addr);
        high = ioread32(addr + sizeof(u32));

        return low | (high << 32);
}

void reg_write64(void __iomem *addr, u64 value)
{
        u32 low, high;

        high = value >> 32;
        low = value & 0xFFFFFFFF;

        iowrite32(low, addr);
        iowrite32(high, addr + sizeof(u32));
}

/* NB: Just print out for now. Need to expand to handle the errors */
static void sdxi_print_err(struct sdxi_dev *sdxi, struct sdxi_err *err)
{
	struct device *dev = &sdxi->pdev->dev;

	dev_err(dev, "error found\n");
}

static void sdxi_handle_err(struct sdxi_dev *sdxi)
{
	u64 read_ptr, write_ptr, offset;
	struct sdxi_err *err_entry;

	read_ptr = reg_read64(sdxi->ctrl_regs + MMIO_ERR_RD_OFFSET);
	write_ptr = reg_read64(sdxi->ctrl_regs + MMIO_ERR_WRT_OFFSET);

	while (read_ptr != write_ptr) {
		offset = (read_ptr * 64) % ((sdxi->err_log_num + 1) * 4096);
		err_entry = (struct sdxi_err *)sdxi->err_log + offset;

		sdxi_print_err(sdxi, err_entry);
		read_ptr++;
	}

	reg_write64(sdxi->ctrl_regs + MMIO_ERR_RD_OFFSET, read_ptr);
	reg_write64(sdxi->ctrl_regs + MMIO_ERR_WRT_OFFSET, 0xB);
}

static irqreturn_t sdxi_irq_thread(int irq, void *data)
{
	struct sdxi_dev *sdxi = (struct sdxi_dev *)data;
	u64 status = reg_read64(sdxi->ctrl_regs + MMIO_ERR_STS_OFFSET);

	while (status & ERR_STS_REC_MASK) {
		printk(KERN_ERR "read status\n");
		sdxi_handle_err(sdxi);
		status = reg_read64(sdxi->ctrl_regs + MMIO_ERR_STS_OFFSET);
	}

	return IRQ_HANDLED;
}

static int sdxi_pci_irq_init(struct sdxi_dev *sdxi)
{
	struct pci_dev *pdev = sdxi->pdev;
	struct device *dev = &pdev->dev;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(sdxi->msix_entry); i++)
		sdxi->msix_entry[i].entry = i;
	ret = pci_enable_msix_range(pdev, sdxi->msix_entry, 1, i);
	pr_debug("enable_msix_range i=%d, ret = %d, vector[0]=%d\n", i, ret,
		 sdxi->msix_entry[0].vector);

        /* setup err log interrupt handler */
        sdxi->err_irq.vector = sdxi->msix_entry[0].vector;
	ret = request_irq(sdxi->err_irq.vector, sdxi_irq_thread, 0,
			  SDXI_DRV_NAME, sdxi);

	if (ret) {
		dev_err(dev, "cannot alloc irq handler for error irq\n");
		goto err_irq0_alloc;
	}

	return 0;

err_irq0_alloc:
	pci_disable_msix(pdev);
	return ret;
}

static void sdxi_pci_irq_exit(struct sdxi_dev *sdxi)
{
	struct pci_dev *pdev = sdxi->pdev;

	free_irq(sdxi->err_irq.vector, sdxi);
	/* NB: free context IRQs */
	//pci_free_irq_vectors(pdev);
	pci_disable_msix(pdev);
}

static void sdxi_pci_parse_cap(struct sdxi_dev *sdxi)
{
	u64 cap0, cap1;
	u32 db_stride, max_ring_sz, max_rkey_sz;
	u32 max_buff_sz, max_errlog_sz, max_akey_sz;

	/* generic properties */
	sdxi->max_pasids = pci_max_pasids(sdxi->pdev);

	/* CAP0 */
	cap0 = reg_read64(sdxi->ctrl_regs + MMIO_CAP0_OFFSET);
	cap1 = reg_read64(sdxi->ctrl_regs + MMIO_CAP1_OFFSET);

	sdxi->sfunc = cap0 & CAP0_SFUNC_MASK;
	sdxi->is_vf = (cap0 >> CAP0_VF_SHIFT) & CAP0_VF_MASK;

	db_stride = (cap0 >> CAP0_DB_STRIDE_SHIFT) & CAP0_DB_STRIDE_MASK;
	sdxi->db_stride = 1 << (db_stride + 12);

	max_ring_sz = (cap0 >> CAP0_MAX_DS_RING_SZ_SHIFT) & CAP0_MAX_DS_RING_SZ_MASK;
	sdxi->max_ring_entries = 1ULL << (max_ring_sz + 10);

	max_rkey_sz = (cap0 >> CAP0_MAX_RKEY_SZ_SHIFT) & CAP0_MAX_RKEY_SZ_MASK;
	sdxi->max_rkeys = 1 << (max_rkey_sz + 8);

	/* CAP1 */
	max_buff_sz = cap1 & CAP1_MAX_BUFFER_MASK;
	sdxi->max_buffer = 2ULL << (max_buff_sz + 21);

	sdxi->has_rkey = (cap1 >> CAP1_RKEY_CAP_SHIFT) & CAP1_RKEY_CAP_MASK;

	max_errlog_sz = (cap1 >> CAP1_MAX_ERRLOG_SZ_SHIFT) & CAP1_MAX_ERRLOG_SZ_MASK;
	sdxi->max_err_logs = 2 << (max_errlog_sz + 7);

	max_akey_sz = (cap1 >> CAP1_MAX_AKEY_SZ_SHIFT) & CAP1_MAX_AKEY_SZ_MASK;
	sdxi->max_akeys = 1 << (max_akey_sz + 8);

	sdxi->max_cxts = ((cap1 >> CAP1_MAX_CXT_SHIFT) & CAP1_MAX_CXT_MASK) + 1;

	sdxi->op_grp_cap = (cap1 >> CAP1_OPB_000_CAP_SHIFT) & CAP1_OPB_000_CAP_MASK;

	pr_info("Device 0x%04x found [cap0=0x%llx, cap1=0x%llx]\n",
		sdxi->sfunc, cap0, cap1);
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

static int sdxi_pci_enable(struct sdxi_dev *sdxi)
{
	struct device *dev = &sdxi->pdev->dev;
	dma_addr_t l2_addr, rkey_addr, err_log_addr;
	u64 ctrl0, ctrl2, status;

	/* l2 table */
	l2_addr = dma_map_single(dev, sdxi->l2_table, L2_TABLE_SIZE,
				 DMA_TO_DEVICE);
	l2_addr &= CXT_L2_PTR_MASK;
	reg_write64(sdxi->ctrl_regs + MMIO_CXT_L2_OFFSET, l2_addr);

	/* rkey */
	rkey_addr = dma_map_single(dev, sdxi->rkey,
				   sdxi->rkey_num * sizeof(struct rkey_ent),
				   DMA_FROM_DEVICE);
	rkey_addr &= RKEY_PTR_MASK;
	rkey_addr |= RKEY_EN_MASK;
	rkey_addr |= (((sdxi->rkey_num >> 8) & RKEY_SZ_MASK) << RKEY_SZ_SHIFT);
	reg_write64(sdxi->ctrl_regs + MMIO_RKEY_OFFSET, rkey_addr);

	/* err log */
	err_log_addr = dma_map_single(dev, sdxi->err_log,
				 sdxi->err_log_num * sizeof(struct sdxi_err),
				 DMA_FROM_DEVICE);
	err_log_addr &= ERR_CFG_PTR_MASK;
	err_log_addr |= ERR_CFG_EN_MASK;
	err_log_addr |= (((sdxi->err_log_num >> 6) & ERR_CFG_SZ_MASK) << ERR_CFG_SZ_SHIFT);
	reg_write64(sdxi->ctrl_regs + MMIO_ERR_CFG_OFFSET, err_log_addr);
	reg_write64(sdxi->ctrl_regs + MMIO_ERR_CTL_OFFSET, ERR_CTL_INTR_EN_MASK);

	/* enable device */
	ctrl0 = reg_read64(sdxi->ctrl_regs + MMIO_CTL0_OFFSET);
	ctrl0 |= GSRV_ACTIVE & CTL0_FN_GSR_MASK;
	ctrl0 |= (CTL0_FN_ERR_INTR_EN_MASK << CTL0_FN_ERR_INTR_EN_SHIFT);
	reg_write64(sdxi->ctrl_regs + MMIO_CTL0_OFFSET, ctrl0);

	ctrl2 = reg_read64(sdxi->ctrl_regs + MMIO_CTL2_OFFSET);
	ctrl2 &= 0xFFFFFFFF0000FFFFULL;
	ctrl2 |= (sdxi->max_cxts << 16) & 0x00000000FFFF0000ULL;
	ctrl2 &= 0x00000000FFFFFFFFULL;
	ctrl2 |= (uint64_t)sdxi->op_grp_cap << 32;
	reg_write64(sdxi->ctrl_regs + MMIO_CTL2_OFFSET, ctrl2);

	status = reg_read64(sdxi->ctrl_regs + MMIO_STS0_OFFSET);

	pr_debug("function info:\n"
		 "  err log addr: v=0x%p:d=0x%llx\n"
		 "  rkey addr:    v=0x%p:d=0x%llx\n"
		 "  func status:  0x%lx\n"
		 "  ctrl2:        0x%llx\n",
		 sdxi->err_log, err_log_addr & ~0x1, sdxi->rkey, rkey_addr,
		 (unsigned long)status, (unsigned long long)ctrl2);

	return 0;
}

static void sdxi_dump_errlog(struct sdxi_dev *sdxi)
{
	u64 err_ctl, err_sts, err_cfg, err_wrt, err_rd;

	err_ctl = reg_read64(sdxi->ctrl_regs + MMIO_ERR_CTL_OFFSET);
	err_sts = reg_read64(sdxi->ctrl_regs + MMIO_ERR_STS_OFFSET);
	err_cfg = reg_read64(sdxi->ctrl_regs + MMIO_ERR_CFG_OFFSET);
	err_wrt = reg_read64(sdxi->ctrl_regs + MMIO_ERR_WRT_OFFSET);
	err_rd = reg_read64(sdxi->ctrl_regs + MMIO_ERR_RD_OFFSET);
}

static void sdxi_pci_disable(struct sdxi_dev *sdxi)
{
	u64 ctrl0;

	sdxi_dump_errlog(sdxi);
	/* disable device */
	ctrl0 = reg_read64(sdxi->ctrl_regs + MMIO_CTL0_OFFSET);
	ctrl0 &= ~CTL0_FN_GSR_MASK;
	ctrl0 |= (GSRV_STOP_SF & CTL0_FN_GSR_MASK);
	reg_write64(sdxi->ctrl_regs + MMIO_CTL0_OFFSET, ctrl0);
}

static struct sdxi_dev *sdxi_device_alloc(struct device *dev)
{
	struct sdxi_dev *sdxi;
	gfp_t gfp_flags;
	unsigned long order;
	int entries;

	sdxi = kzalloc(sizeof(*sdxi), GFP_KERNEL);
	if (!sdxi)
		return NULL;

	gfp_flags = GFP_KERNEL | __GFP_ZERO;
	order = get_order(L2_TABLE_SIZE);
	sdxi->l2_table = (void *)__get_free_pages(gfp_flags, order);
	if (!sdxi->l2_table)
		goto l2_fail;

	/* rkey */
	entries = DEFAULT_RKEY_NUM;
	sdxi->rkey = kzalloc(entries * sizeof(struct rkey_ent), GFP_KERNEL);
	if (!sdxi->rkey)
		goto rkey_fail;
	sdxi->rkey_num = entries;

	/* error log */
	entries = DEFAULT_ERR_LOG_NUM;
	sdxi->err_log = kzalloc(entries * sizeof(struct sdxi_err), GFP_KERNEL);
	if (!sdxi->err_log)
		goto err_log_fail;
	sdxi->err_log_num = entries;

	spin_lock_init(&sdxi->ctxt_lock);
	INIT_LIST_HEAD(&sdxi->ctxt_list);
	list_add_tail(&sdxi->list, &sdxi_device_list);

	return sdxi;

err_log_fail:
	kfree(sdxi->rkey);
rkey_fail:
	free_pages((unsigned long)sdxi->l2_table, order);
l2_fail:
	kfree(sdxi);
	return NULL;
}

static void sdxi_device_free(struct sdxi_dev *sdxi)
{
	list_del(&sdxi->list);
	kfree(sdxi->err_log);
	kfree(sdxi->rkey);
	free_pages((unsigned long)sdxi->l2_table, get_order(L2_TABLE_SIZE));
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
	int rc = 0;

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
MODULE_LICENSE("Dual BSD/GPL");
module_init(sdxi_module_init);
module_exit(sdxi_module_exit);
MODULE_SOFTDEP("pre: iommu_v2");
