// This software is licensed under either the MIT or the GPLv2 licenses below.
//
// ----------------------------------------------------------------------------
// MIT license:
//
// Copyright 2023 Peralex Electronics(Pty) Ltd
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the “Software”), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
//
// ----------------------------------------------------------------------------
// GPLv2 license:
//
// Copyright(C) 2023 Peralex Electronics(Pty) Ltd
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation; version 2 of the License.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, write to the Free Software Foundation, Inc.,
// 59 Temple Place, Suite 330, Boston, MA 02111 - 1307 USA
//

#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/cdev.h>
#include <linux/semaphore.h>

#define __SURGE_DEV_IOCTLS_DRIVER_DEFS__
#include "ioctls.h"
#include "VersionSurge.h"

#define MAX_DMA_BUFF_SIZE	(4096 * 136 * 4)

#define SURGE_DRIVER "surge_driver"
#define SURGE_DEVICE_NAME "surge"
#define SURGE_DEVICE_CLASS "surge-class"
#define SURGE_DEVICE_MAX_NUM_INSTANCES 8
static struct class *g_pSurgeDriverClass;
static dev_t g_Surge_dev_num;
static unsigned int g_uNumDevices = 0;
static unsigned int g_uDriverVersion = 0;

#define PCIE_OFFSET 0x14
#define DCSR_OFFSET 0x4 + PCIE_OFFSET // Device Control and Status Register
#define WRITE_ADDR_OFFSET 0x8 + PCIE_OFFSET // Write DMA TLP Address Register
#define READ_ADDR_OFFSET 0x1C + PCIE_OFFSET // Read  DMA TLP Address Register
#define PCIE_IRQ_CTL_STAT_OFFSET 0x48 + PCIE_OFFSET // PCI-E IRQ CTL STAT register


//Supports device with VID = 0x10EE, and PID = 0x0007
static struct pci_device_id SurgeDriverDeviceIdTable[] = {
	{PCI_DEVICE(0x10EE, 0x0007)},
	{
		0,
	}};

MODULE_DEVICE_TABLE(pci, SurgeDriverDeviceIdTable);

static int SurgeDriverProbe(struct pci_dev *pdev, const struct pci_device_id *ent);
static void SurgeDriverRemove(struct pci_dev *pdev);
static int SurgeDriverOpen(struct inode *inode, struct file *file);
static int SurgeDriverRelease(struct inode *inode, struct file *file);
static long SurgeDriverIoctl(struct file *file, unsigned int cmd, unsigned long arg);
static irqreturn_t SurgeDriverIrqHandler(int irq, void *dev_id);

//Driver registration structure
static struct pci_driver Surge_driver = {
	.name = SURGE_DRIVER,
	.id_table = SurgeDriverDeviceIdTable,
	.probe = SurgeDriverProbe,
	.remove = SurgeDriverRemove};

//File oprations structure declaration
struct file_operations Surge_driver_fops = {
	.unlocked_ioctl = SurgeDriverIoctl,
	.open = SurgeDriverOpen,
	.release = SurgeDriverRelease,
	.owner = THIS_MODULE
};

//Driver device instance private data. Contains device instance-specific
//resources and information that should be passed between driver's functions.
struct cSurgeDeviceInstance
{
	u8 __iomem *m_pBAR0_Mem;
	struct cdev m_Surge_cdev;
	unsigned int m_uDeviceID;
	struct cReg32Access m_RegAccessTemp;
	struct cDmaAccess m_DmaAccessTemp;
	unsigned m_uIrq;
	struct semaphore *m_semIrq;
	void *m_pDmaBuffHost;
	dma_addr_t m_pDmaBuffDevice;
};

static int SurgeDriverUevent(struct device *dev, struct kobj_uevent_env *env)
{
	add_uevent_var(env, "DEVMODE=%#o", 0666);
	return 0;
}

static int __init SurgeDriverInit(void)
{
	printk("SurgeDriverInit\n");

	g_uDriverVersion = ((uVersionMajor & 0xFF) << 16) | ((uVersionRelease & 0xFF) << 8) | (uVersionMinor & 0xFF);

	//Request for a device major and SURGE_DEVICE_MAX_NUM_INSTANCES minors
	alloc_chrdev_region(&g_Surge_dev_num, 0, SURGE_DEVICE_MAX_NUM_INSTANCES, SURGE_DEVICE_NAME);
	//Create our device class, visible in /sys/class
	g_pSurgeDriverClass = class_create(THIS_MODULE, SURGE_DEVICE_CLASS);
	g_pSurgeDriverClass->dev_uevent = SurgeDriverUevent;

	//Register new PCI driver
	return pci_register_driver(&Surge_driver);
}

static void __exit SurgeDriverExit(void)
{
	printk("SurgeDriverExit\n");

	//Unregister the driver
	pci_unregister_driver(&Surge_driver);

	//Unregister the driver class
	class_unregister(g_pSurgeDriverClass);
	class_destroy(g_pSurgeDriverClass);
	unregister_chrdev_region(g_Surge_dev_num, SURGE_DEVICE_MAX_NUM_INSTANCES);
}

void ReleaseDevice(struct pci_dev *pdev)
{
	printk("ReleaseDevice\n");

	//Free the memory mapped BARs
	pci_release_region(pdev, pci_select_bars(pdev, IORESOURCE_MEM));

	//Disable bus mastering
	pci_clear_master(pdev);

	//Disable the pci device
	pci_disable_device(pdev);
}

static int SurgeDriverProbe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	int bar, err, nIrqs, irqVec;
	u16 vendor, device;
	unsigned long mmio_start, mmio_len;
	struct cSurgeDeviceInstance *pDevInstance;
	unsigned int uiFWVersion;
	dev_t curr_dev;

	//Read data from the PCI device configuration registers
	pci_read_config_word(pdev, PCI_VENDOR_ID, &vendor);
	pci_read_config_word(pdev, PCI_DEVICE_ID, &device);

	printk(KERN_INFO "Device vid: 0x%X pid: 0x%X\n", vendor, device);

	//Request IO BAR
	bar = pci_select_bars(pdev, IORESOURCE_MEM);

	//Enable device memory
	err = pci_enable_device_mem(pdev);

	if(err) {
		printk("pci_enable_device_mem : err=%d\n", err);
		return err;
	}

	//Enable bus mastering
	pci_set_master(pdev);

	//Request memory region for the BAR
	err = pci_request_region(pdev, bar, SURGE_DRIVER);

	if(err) {
		printk("pci_request_region : err=%d\n", err);
		pci_disable_device(pdev);
		return err;
	}

	//Get start and stop memory offsets for device BAR0
	mmio_start = pci_resource_start(pdev, 0);
	mmio_len = pci_resource_len(pdev, 0);

	//Allocate memory for the driver device instance data
	pDevInstance = kzalloc(sizeof(struct cSurgeDeviceInstance), GFP_KERNEL);

	if(!pDevInstance) {
		printk("kzalloc failed");
		ReleaseDevice(pdev);
		return -ENOMEM;
	}

	//Remap BAR0 to a local pointer
	pDevInstance->m_pBAR0_Mem = ioremap(mmio_start, mmio_len);

	if(!pDevInstance->m_pBAR0_Mem) {
		printk("ioremap failed");
		ReleaseDevice(pdev);
		return -EIO;
	}

	//Allocate a kernel semaphore used for signalling in the DMA complete interrupt
	pDevInstance->m_semIrq = kzalloc(sizeof(struct semaphore), GFP_KERNEL);
	if(pDevInstance->m_semIrq) {
		sema_init(pDevInstance->m_semIrq, 0);
		dev_info(&pdev->dev, "Created irq semaphore");
	}

	//Allocate kernel memory buffer for device DMA usage
	pDevInstance->m_pDmaBuffHost = pci_alloc_consistent(pdev, MAX_DMA_BUFF_SIZE, &pDevInstance->m_pDmaBuffDevice);
	dev_info(&pdev->dev, "Allocated pci consistent memory buffer: CPU addr=%llu, DMA handle=%llu", (unsigned long long)pDevInstance->m_pDmaBuffHost, (unsigned long long)pDevInstance->m_pDmaBuffDevice);

	dev_info(&pdev->dev, "Device num = %d", g_uNumDevices);
	//Maintain a device instance count for each matching device found in the probe
	pDevInstance->m_uDeviceID = g_uNumDevices++;

	//Configure interrupts
	nIrqs = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_ALL_TYPES);
	if(nIrqs < 0) {
		dev_warn(&pdev->dev, "Failed to allocate irq vectors, error = %d", nIrqs);
	}
	else {
		dev_info(&pdev->dev, "Successfully allocated %d irq vectors, msix_enabled=%d, msi_enabled=%d", nIrqs, (int)pdev->msix_enabled, (int)pdev->msi_enabled);
		irqVec = pci_irq_vector(pdev, 0);
		if(irqVec < 0) {
			dev_warn(&pdev->dev, "Failed to obtain irq vector: err=%d", irqVec);
		}
		else {
			dev_info(&pdev->dev, "IRQ vector = %d", irqVec);
			pDevInstance->m_uIrq = irqVec;
			err = devm_request_irq(&pdev->dev, pDevInstance->m_uIrq, SurgeDriverIrqHandler, 0, SURGE_DEVICE_NAME, (void *)pDevInstance);
			if(err) {
				dev_warn(&pdev->dev, "Failed to request IRQ: err=%d", err);
			}
			else {
				dev_info(&pdev->dev, "Successfully installed interrupt handler");
			}
		}
	}

	//Finally, create char device and /dev node
	//Bind file_operations to the cdev
	cdev_init(&pDevInstance->m_Surge_cdev, &Surge_driver_fops);
	pDevInstance->m_Surge_cdev.owner = THIS_MODULE;
	//Device number to use to add cdev to the core
	curr_dev = MKDEV(MAJOR(g_Surge_dev_num), MINOR(g_Surge_dev_num) + pDevInstance->m_uDeviceID);
	//Make the device live for the users to access
	cdev_add(&pDevInstance->m_Surge_cdev, curr_dev, 1);
	//Create a node for each device
	device_create(g_pSurgeDriverClass,
		NULL,
		curr_dev,
		pDevInstance,
		SURGE_DEVICE_NAME "%d",
		pDevInstance->m_uDeviceID);

	//Set driver private device instance data
	//Now we can access mapped "m_pBAR0_Mem" from any of the driver's functions
	pci_set_drvdata(pdev, pDevInstance);

	uiFWVersion = ioread32(pDevInstance->m_pBAR0_Mem + 0);
	dev_info(&pdev->dev, "Firmware version = %d.%d\n", (unsigned int)((uiFWVersion >> 8) & 0xFF), (unsigned int)(uiFWVersion & 0xFF));

	return 0;
}

static int SurgeDriverOpen(struct inode *inode, struct file *file)
{
	struct cSurgeDeviceInstance *pDevInstance = container_of(inode->i_cdev, struct cSurgeDeviceInstance, m_Surge_cdev);
	file->private_data = pDevInstance;
	printk("SurgeDriverOpen\n");
	return 0;
}

static int SurgeDriverRelease(struct inode *inode, struct file *file)
{
	printk("SurgeDriverRelease\n");
	return 0;
}

irqreturn_t SurgeDriverIrqHandler(int irq, void *dev_id)
{
	unsigned u32Mask = 0x10000;
	unsigned uStatus = 0x0;
	struct cSurgeDeviceInstance *pDevInstance = (struct cSurgeDeviceInstance *)dev_id;

	uStatus = ioread32(pDevInstance->m_pBAR0_Mem + PCIE_IRQ_CTL_STAT_OFFSET);
	if((uStatus & 0xFFFF0000) == 0)
		return IRQ_NONE;

	//Clear latched interrupt
	iowrite32(0xffff, pDevInstance->m_pBAR0_Mem + PCIE_IRQ_CTL_STAT_OFFSET);
	iowrite32(0xffffffff, pDevInstance->m_pBAR0_Mem + PCIE_IRQ_CTL_STAT_OFFSET);
	//Read FW register to flush preceding writes
	ioread32(pDevInstance->m_pBAR0_Mem);

	//Unmask interrupts
	iowrite32(0xffff0000, pDevInstance->m_pBAR0_Mem + PCIE_IRQ_CTL_STAT_OFFSET);

	//Check if the interrupt source is from DMA
	if(uStatus & u32Mask) {
		//printk("SurgeDriverIrqHandler: Signalling DMA complete interrupt\n");
		up(pDevInstance->m_semIrq);
	}

	return IRQ_HANDLED;
}

static long SurgeDriverIoctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct cSurgeDeviceInstance *pDevInstance = file->private_data;

	switch(cmd) {
	case CTL_READ_REG32:
		if(copy_from_user(&pDevInstance->m_RegAccessTemp, (struct cReg32Access *)arg, sizeof(struct cReg32Access)) == 0) {
			pDevInstance->m_RegAccessTemp.m_uValue = ioread32(pDevInstance->m_pBAR0_Mem + pDevInstance->m_RegAccessTemp.m_uRegAddress);
			if(copy_to_user((struct cReg32Access *)arg, &pDevInstance->m_RegAccessTemp, sizeof(struct cReg32Access))) {
				pr_err("cReg32Access copy_to_user : Err!\n");
				return -EINVAL;
			}
		}
		else {
			pr_err("cReg32Access copy_from_user : Err!\n");
			return -EINVAL;
		}
		break;

	case CTL_WRITE_REG32:
		if(copy_from_user(&pDevInstance->m_RegAccessTemp, (struct cReg32Access *)arg, sizeof(struct cReg32Access)) == 0) {
			iowrite32(pDevInstance->m_RegAccessTemp.m_uValue, pDevInstance->m_pBAR0_Mem + pDevInstance->m_RegAccessTemp.m_uRegAddress);
		}
		else {
			pr_err("cReg32Access copy_from_user : Err!\n");
			return -EINVAL;
		}
		break;

	case CTL_DMA_TRANSFER:
		if(copy_from_user(&pDevInstance->m_DmaAccessTemp, (struct cDmaAccess *)arg, sizeof(struct cDmaAccess)) == 0) {
			//Limit the max DMA size
			if(pDevInstance->m_DmaAccessTemp.m_uSize > MAX_DMA_BUFF_SIZE)
				pDevInstance->m_DmaAccessTemp.m_uSize = MAX_DMA_BUFF_SIZE;
			//Setup buffer pointers
			if(pDevInstance->m_DmaAccessTemp.m_uDMAReadDir == 0) {
				//For DMA write, copy user data to DMA buffer
				if (copy_from_user(pDevInstance->m_pDmaBuffHost, pDevInstance->m_DmaAccessTemp.m_pvUserDataBuffer, pDevInstance->m_DmaAccessTemp.m_uSize) != 0) {
					pr_err("pDevInstance->m_DmaAccessTemp.m_pvUserDataBuffer copy_from_user : Err!\n");
					return -EINVAL;
				}
				//Setup write address (read as seen from device's perspective)
				iowrite32(pDevInstance->m_pDmaBuffDevice, pDevInstance->m_pBAR0_Mem + READ_ADDR_OFFSET);
				//Start DMA write
				iowrite32(0x10000, pDevInstance->m_pBAR0_Mem + DCSR_OFFSET);
			}
			else {
				//For DMA read, setup read address (write as seen from device's perspective)
				iowrite32(pDevInstance->m_pDmaBuffDevice, pDevInstance->m_pBAR0_Mem + WRITE_ADDR_OFFSET);
				//Start DMA read
				iowrite32(0x1, pDevInstance->m_pBAR0_Mem + DCSR_OFFSET);
			}

			//Wait for the DMA to complete
			if(pDevInstance->m_DmaAccessTemp.m_uTimeout_ms == 0) {
				if(down_interruptible(pDevInstance->m_semIrq) != 0) {
					pr_err("down_interruptible was interrupted!\n");
					return -EINTR;
				}
			}
			else {
				if(down_timeout(pDevInstance->m_semIrq, pDevInstance->m_DmaAccessTemp.m_uTimeout_ms) != 0) {
					return -ETIME;
				}
			}

			if(pDevInstance->m_DmaAccessTemp.m_uDMAReadDir != 0) {
				if(copy_to_user(pDevInstance->m_DmaAccessTemp.m_pvUserDataBuffer, pDevInstance->m_pDmaBuffHost, pDevInstance->m_DmaAccessTemp.m_uSize)) {
					pr_err("pDevInstance->m_DmaAccessTemp.m_pvUserDataBuffer copy_to_user : Err!\n");
					return -EINVAL;
				}
			}

			if(copy_to_user((struct cDmaAccess *)arg, &pDevInstance->m_DmaAccessTemp, sizeof(struct cDmaAccess)) != 0) {
				pr_err("cDmaAccess copy_to_user : Err!\n");
				return -EINVAL;
			}
		}
		else {
			pr_err("cDmaAccess copy_from_user : Err!\n");
			return -EINVAL;
		}
		break;

	case CTL_READ_DRIVER_VERSION:
		if(copy_to_user((unsigned *)arg, &g_uDriverVersion, sizeof(g_uDriverVersion)) != 0) {
			pr_err("cDmaAccess copy_to_user : Err!\n");
			return -EINVAL;
		}
		break;

	default:
		return -ENOTTY;
	}
	return 0;
}

// Clean up and release resources on driver removal
static void SurgeDriverRemove(struct pci_dev *pdev)
{
	struct cSurgeDeviceInstance *pDevInstance = pci_get_drvdata(pdev);

	if(pDevInstance) {
		devm_free_irq(&pdev->dev, pDevInstance->m_uIrq, (void *)pDevInstance);

		if(pDevInstance->m_semIrq) {
			kfree(pDevInstance->m_semIrq);
		}

		device_destroy(g_pSurgeDriverClass,
			MKDEV(MAJOR(g_Surge_dev_num), (MINOR(g_Surge_dev_num) + pDevInstance->m_uDeviceID)));
		cdev_del(&pDevInstance->m_Surge_cdev);

		if(pDevInstance->m_pBAR0_Mem) {
			iounmap(pDevInstance->m_pBAR0_Mem);
		}

		pci_free_irq_vectors(pdev);

		pci_free_consistent(pdev, MAX_DMA_BUFF_SIZE, pDevInstance->m_pDmaBuffHost, pDevInstance->m_pDmaBuffDevice);

		kfree(pDevInstance);
	}

	ReleaseDevice(pdev);
}

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("Peralex Electronics (Pty) Ltd");
MODULE_DESCRIPTION("Surge device driver");

module_init(SurgeDriverInit);
module_exit(SurgeDriverExit);
