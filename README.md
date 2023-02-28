# Linux SDXI

## Table of contents

1. [What is SDXI](#intro)
2. [Installation](#install)
3. [DMA Test with SDXI](#dmatest)
4. [User Space Test with SDXI](#usertest)

## What is SDXI? <a name="intro"></a>

SDXI (Smart Data Stream Accelerator) is an industry standard, developed
and managed by SNIA TWG, for data movement and transformation acclerators.
SDXI device offers an architectural interface that addresses many limitations
of existing DMA accelerators. The SDXI specification can be found at [SNIA website](https://www.snia.org/sdxi).

This project implements a Linux driver for SDXI with various functionalities
in-mind. The most important one is an interface for user-space libraries and
applications to manage and offload operations directly to SDXI hardware. Like
other generic DMA engine drivers, it also provides an extension into Linux DMA
engine API. 

## Installation <a name="host"></a>

Linux SDXI assumes a host with SDXI support as PCIe end-point devices. To verify
that your system have SDXI devices, search for "SNIA Smart Data Accelerator
Interface (SDXI) controller" in the output of `lspci -vvv` command.

Before compiling the kernel, make sure CONFIG_SDXI is enabled in the kernel
configure file. After the SDXI driver is loaded, you should see the following 
output: 

```
[ 12.749769] sdxi 0000:c4:00.1: enabling device (0000 -> 0002)
[ 12.756612] SDXI function [sfunc=0x0801, vf=0]:
[ 12.761673]   max contexts:     64
[ 12.765471]   max ring entries: 0x100000000
[ 12.770141]   max rkeys/akeys:  65536/65536
[ 12.774819]   op group cap:     0x0018
[ 12.779008]   err log size:     131072
```
The device driver also create a new IOCTL interface at `/dev/sdxi`.

## DMA Test with SDXI <a name="dmatest"></a>
DMA engine support is disabled by default. In order to enable it, supply
`dma_engine=1` while loading the SDXI device driver.

```
# insmod sdxi.ko dma_engine=1
```
After that, you should see the following DMA engine devices created:

```
# ls /sys/class/dma/
dma0chan0  dma1chan0  dma2chan0  dma3chan0
```
We can then leverage dmatest driver to stress test the newly created DMA engine devices:
```
# insmod drivers/dma/dmatest.ko polled=true noverify=false channel=dma0chan0 iterations=10 run=1 test_buf_size=8192 alignment=13 timeout=-1
# dmesg
[ 87.696064] dmatest: Added 1 threads using dma0chan0
[ 87.701878] dmatest: Started 1 threads using dma0chan0
[ 87.703612] dmatest: dma0chan0-copy0: summary 10 tests, 0 failures 6587.61 iops
```

## User Space Test with SDXI <a name="usertest"></a>
SDXI driver offers a user-space IOCTL interface. Please refer to ./include/uapi/linux/sdxi.h
for details. This interface allows user-space applications to utilize SDXI device directly
using user-space address. More examples are available in the libsdxi software release.
