# SPDX-License-Identifier: GPL-2.0

ccflags-$(CONFIG_SDXI_DEBUG) += -DDEBUG

obj-$(CONFIG_SDXI) += sdxi.o

sdxi-objs += \
	context.o     \
	device.o      \
	dma.o         \
	error.o       \
	ioctl.o       \
	process.o     \
	trace.o       \
	util.o

sdxi-$(CONFIG_PCI_MSI) += pci.o
