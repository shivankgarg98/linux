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
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/pci.h>
#include <linux/pci-ats.h>
#include <linux/io.h>
#include <linux/iomap.h>
#include <linux/math64.h>

#include "sdxi.h"
#include "pci.h"
#include "process.h"

static bool enabled = true;
module_param(enabled, bool, 0644);
MODULE_PARM_DESC(enabled, "Enable SDXI feature support (default: false)");

LIST_HEAD(sdxi_device_list);


#define MIN(a, b)	(a > b ? b : a)
static void sdxi_print_err(struct sdxi_dev *sdxi, struct sdxi_err *err)
{
	struct device *dev = &sdxi->pdev->dev;
	int index;
	const char *sub_steps[] = {
		"Other or Internal Error",
		"Address Translation Failure",
		"Data Access Failure",
		"Data Validation Failure",
		"Unknown/Reserved Type",
	};
	const char *reactions[] = {
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
		index = MIN(ARRAY_SIZE(sub_steps) - 1, err->sub_step);
		dev_err(dev, "  sub_step: %s\n", sub_steps[index]);
		index = MIN(ARRAY_SIZE(reactions) - 1, err->re);
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

	read_ptr = ioread64(sdxi->ctrl_regs + MMIO_ERR_RD_OFFSET);
	write_ptr = ioread64(sdxi->ctrl_regs + MMIO_ERR_WRT_OFFSET);

	while (read_ptr < write_ptr) {
		offset = (read_ptr * 64) % ((sdxi->err_log_num + 1) * 4096);
		err_entry = (struct sdxi_err *)sdxi->err_log + offset;
		sdxi_print_err(sdxi, err_entry);
		read_ptr++;
	}

	iowrite64(read_ptr, sdxi->ctrl_regs + MMIO_ERR_RD_OFFSET);
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
	struct sdxi_dev *sdxi = (struct sdxi_dev *)data;
	union mmio_err_sts_reg err_sts;

	err_sts.data = ioread64(sdxi->ctrl_regs + MMIO_ERR_STS_OFFSET);

	while (err_sts.sts) {
		sdxi_handle_err(sdxi);

		/* clear status bit */
		err_sts.data |= 0x1;
		iowrite64(err_sts.data, sdxi->ctrl_regs + MMIO_ERR_STS_OFFSET);

		err_sts.data = ioread64(sdxi->ctrl_regs + MMIO_ERR_STS_OFFSET);
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

	/* NB: alloc and setup cxt_irqs here */
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
	union mmio_cap0_reg cap0;
	union mmio_cap1_reg cap1;

	/* generic properties */
	sdxi->max_pasids = pci_max_pasids(sdxi->pdev);

	/* CAP0 */
	cap0.data = ioread64(sdxi->ctrl_regs + MMIO_CAP0_OFFSET);

	sdxi->sfunc = cap0.sfunc;
	sdxi->is_vf = cap0.vf;
	sdxi->db_stride = 1 << (cap0.db_stride + 12);
	sdxi->max_ring_entries = 1ULL << (cap0.max_ds_ring_sz + 10);
	sdxi->max_rkeys = 1 << (cap0.max_rkey_sz + 8);

	/* CAP1 */
	cap1.data = ioread64(sdxi->ctrl_regs + MMIO_CAP1_OFFSET);

	sdxi->max_buffer = 2ULL << (cap1.max_buffer + 21);
	sdxi->has_rkey = cap1.rkey_cap;
	sdxi->max_err_logs = 2 << (cap1.max_errlog_sz + 7);
	sdxi->max_akeys = 1 << (cap1.max_akey_sz + 8);
	sdxi->max_cxts = cap1.max_cxt + 1;
	sdxi->op_grp_cap = cap1.opb_000_cap;

	pr_info("Device 0x%04x found [cap0=0x%llx, cap1=0x%llx]\n",
		sdxi->sfunc, cap0.data, cap1.data);
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
	union mmio_cxt_l2_reg cxt_l2_reg;
	union mmio_rkey_reg rkey_reg;
	union mmio_err_cfg_reg err_cfg_reg;
	union mmio_err_ctl_reg err_ctl_reg;
	dma_addr_t l2_addr, rkey_addr, err_log_addr;
	u64 ctrl2, status;
	union mmio_ctl0_reg ctl0_reg;

	/* l2 table */
	l2_addr = dma_map_single(dev, sdxi->l2_table, L2_TABLE_SIZE,
				 DMA_TO_DEVICE);
	cxt_l2_reg.ptr = l2_addr >> 12;
	iowrite64(cxt_l2_reg.data, sdxi->ctrl_regs + MMIO_CXT_L2_OFFSET);

	/* rkey */
	rkey_addr = dma_map_single(dev, sdxi->rkey,
				   sdxi->rkey_num * sizeof(struct rkey_ent),
				   DMA_FROM_DEVICE);
	rkey_reg.ptr = rkey_addr >> 12;
	rkey_reg.sz = sdxi->rkey_num >> 8;
	rkey_reg.en = 1;
	iowrite64(rkey_reg.data, sdxi->ctrl_regs + MMIO_RKEY_OFFSET);

	/* err log */
	err_log_addr = dma_map_single(dev, sdxi->err_log,
				      sdxi->err_log_num * sizeof(struct sdxi_err),
				      DMA_FROM_DEVICE);
	err_cfg_reg.ptr = err_log_addr >> 12;
	err_cfg_reg.sz = sdxi->err_log_num >> 6;
	err_cfg_reg.en = 1;
	iowrite64(err_cfg_reg.data, sdxi->ctrl_regs + MMIO_ERR_CFG_OFFSET);

	/* err log intr */
	err_ctl_reg.en = 1;
	iowrite64(err_ctl_reg.data, sdxi->ctrl_regs + MMIO_ERR_CTL_OFFSET);

	/* enable device */
	ctl0_reg.data = ioread64(sdxi->ctrl_regs + MMIO_CTL0_OFFSET);
	ctl0_reg.fn_gsr = GSRV_ACTIVE;
	ctl0_reg.fn_err_intr_en = 1;
	iowrite64(ctl0_reg.data, sdxi->ctrl_regs + MMIO_CTL0_OFFSET);

	ctrl2 = ioread64(sdxi->ctrl_regs + MMIO_CTL2_OFFSET);
	ctrl2 &= 0xFFFFFFFF0000FFFFULL;
	ctrl2 |= (sdxi->max_cxts << 16) & 0x00000000FFFF0000ULL;
	ctrl2 &= 0x00000000FFFFFFFFULL;
	ctrl2 |= (uint64_t)sdxi->op_grp_cap << 32;
	iowrite64(ctrl2, sdxi->ctrl_regs + MMIO_CTL2_OFFSET);

	status = ioread64(sdxi->ctrl_regs + MMIO_STS0_OFFSET);

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
	union mmio_err_ctl_reg err_ctl;
	union mmio_err_sts_reg err_sts;
	union mmio_err_cfg_reg err_cfg;
	union mmio_err_wrt_reg err_wrt;
	union mmio_err_rd_reg err_rd;

	err_ctl.data = ioread64(sdxi->ctrl_regs + MMIO_ERR_CTL_OFFSET);
	err_sts.data = ioread64(sdxi->ctrl_regs + MMIO_ERR_STS_OFFSET);
	err_cfg.data = ioread64(sdxi->ctrl_regs + MMIO_ERR_CFG_OFFSET);
	err_wrt.data = ioread64(sdxi->ctrl_regs + MMIO_ERR_WRT_OFFSET);
	err_rd.data = ioread64(sdxi->ctrl_regs + MMIO_ERR_RD_OFFSET);
}

static void sdxi_pci_disable(struct sdxi_dev *sdxi)
{
	union mmio_ctl0_reg ctl0_reg;

	sdxi_dump_errlog(sdxi);

	/* disable device */
	ctl0_reg.data = ioread64(sdxi->ctrl_regs + MMIO_CTL0_OFFSET);
	ctl0_reg.fn_gsr = GSRV_STOP_SF;
	iowrite64(ctl0_reg.data, sdxi->ctrl_regs + MMIO_CTL0_OFFSET);
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

	spin_lock_init(&sdxi->cxt_lock);
	INIT_LIST_HEAD(&sdxi->cxt_list);
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

	if (!enabled) {
		pr_info("SDXI support disabled by default - please use "
			"\"modprobe sdxi enabled=1\" to turn on\n");
		return rc;
	}

	rc = pci_register_driver(&sdxi_driver);
	if (rc)
		return rc;

	rc = sdxi_chardev_init();

	return rc;
}

static void __exit sdxi_module_exit(void)
{
	if (!enabled)
		return;

	sdxi_chardev_exit();
	pci_unregister_driver(&sdxi_driver);
}

MODULE_AUTHOR("Wei Huang <wei.huang2@amd.com>");
MODULE_DESCRIPTION(SDXI_DRV_DESC);
MODULE_LICENSE("GPL v2");
module_init(sdxi_module_init);
module_exit(sdxi_module_exit);
MODULE_SOFTDEP("pre: iommu_v2");
