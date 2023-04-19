/*
 * Some sort of Copyright
 */

#ifndef __QEMU_IVSHMEM_PCI_HELPERS__
#define __QEMU_IVSHMEM_PCI_HELPERS__

#include <uk/bus/pci.h>
#include <uk/plat/common/cpu.h>

#define PCI_BAR_SIZE 0x4
#define PCI_STATUS_CAP_LIST 0x10
#define PCI_CAP_MSIX_ID 		       0x11
#define PCI_CAP_MSIX_CTRL 		       0x2
#define PCI_CAP_MSIX_CTRL_TABLE_SIZE_MASK      0x3ff
#define PCI_CAP_MSIX_CTRL_FUNCTION_MASK	       0x4000
#define PCI_CAP_MSIX_CTRL_ENABLE	       0x8000
#define PCI_CAP_MSIX_BIR_TABLE_OFFSET	       0x4
#define PCI_CAP_MSIX_BIR_TABLE_OFFSET_BIR_MASK 0x7

enum pci_bar_type {
	PCI_BAR_TYPE_MEMORY,
	PCI_BAR_TYPE_IO,
};

enum pci_membar_locatable {
	PCI_MEMBAR_LOCATABLE_32,
	PCI_MEMBAR_LOCATABLE_1M,
	PCI_MEMBAR_LOCATABLE_64,
};

struct pci_bar_info {
	__paddr_t paddr;
	size_t size;
	enum pci_bar_type type;
	enum pci_membar_locatable locatable;
	int prefetchable;
};

struct pci_cap_header {
	__u8 id;
	__u8 next_ptr;
};

struct msix_vector {
	volatile __u64 addr;
	volatile __u32 data;
#define MSIX_VECTOR_CTRL_MASKED 0x1
	volatile __u32 ctrl;
};

static inline void pci_writel(struct pci_address bdf, __u8 offset, __u32 val)
{
	__u32 addr = (PCI_ENABLE_BIT)
		     | (bdf.bus << PCI_BUS_SHIFT)
		     | (bdf.devid << PCI_DEVICE_SHIFT)
		     | (bdf.function << PCI_FUNCTION_SHIFT)
		     | offset;
	outl(PCI_CONFIG_ADDR, addr);
	outl(PCI_CONFIG_DATA, val);
}

static inline void pci_writew(struct pci_address bdf, __u8 offset, __u16 val)
{
	__u32 addr = (PCI_ENABLE_BIT)
		     | (bdf.bus << PCI_BUS_SHIFT)
		     | (bdf.devid << PCI_DEVICE_SHIFT)
		     | (bdf.function << PCI_FUNCTION_SHIFT)
		     | offset;
	outl(PCI_CONFIG_ADDR, addr);
	outw(PCI_CONFIG_DATA, val);
}

static inline void pci_writeb(struct pci_address bdf, __u8 offset, __u8 val)
{
	__u32 addr = (PCI_ENABLE_BIT)
		     | (bdf.bus << PCI_BUS_SHIFT)
		     | (bdf.devid << PCI_DEVICE_SHIFT)
		     | (bdf.function << PCI_FUNCTION_SHIFT)
		     | offset;
	outl(PCI_CONFIG_ADDR, addr);
	outb(PCI_CONFIG_DATA, val);
}

static inline __u32 pci_readl(struct pci_address bdf, __u8 offset)
{
	__u32 addr = (PCI_ENABLE_BIT)
		     | (bdf.bus << PCI_BUS_SHIFT)
		     | (bdf.devid << PCI_DEVICE_SHIFT)
		     | (bdf.function << PCI_FUNCTION_SHIFT)
		     | offset;
	outl(PCI_CONFIG_ADDR, addr);
	return inl(PCI_CONFIG_DATA);
}

static inline __u16 pci_readw(struct pci_address bdf, __u8 offset)
{
	__u32 addr = (PCI_ENABLE_BIT)
		     | (bdf.bus << PCI_BUS_SHIFT)
		     | (bdf.devid << PCI_DEVICE_SHIFT)
		     | (bdf.function << PCI_FUNCTION_SHIFT)
		     | offset;
	outl(PCI_CONFIG_ADDR, addr);
	return inw(PCI_CONFIG_DATA);
}

static inline __u8 pci_readb(struct pci_address bdf, __u8 offset)
{
	__u32 addr = (PCI_ENABLE_BIT)
		     | (bdf.bus << PCI_BUS_SHIFT)
		     | (bdf.devid << PCI_DEVICE_SHIFT)
		     | (bdf.function << PCI_FUNCTION_SHIFT)
		     | offset;
	outl(PCI_CONFIG_ADDR, addr);
	return inb(PCI_CONFIG_DATA);
}

/**
 * Retrieves information about a BAR.
 * @param dev Pointer to the PCI device owning the BAR
 * @param bar_offset Offset of the BAR in the PCI configuration space
 * @param info pci_bar_info struct to fill with BAR infos
 * @return 0 on success, a negative errno value on errors
 */
static int pci_get_bar_info(struct pci_device *dev, __u8 bar_offset,
		struct pci_bar_info *info)
{
	__u32 bar_next = 0, bar = pci_readl(dev->addr, bar_offset);
	__u32 mask;

	if (!info || bar_offset < PCI_BASE_ADDRESS_0
	    || bar_offset > PCI_BASE_ADDRESS_5
	    || (bar_offset & (PCI_BAR_SIZE - 1)))
		return -EINVAL;

	if (bar & PCI_CONF_BAR_TYPE_IO) {
		info->type = PCI_BAR_TYPE_IO;
		mask = PCI_CONF_IOBAR_MASK;
	} else {
		info->type = PCI_BAR_TYPE_MEMORY;
		if (bar & PCI_CONF_MEMBAR_64)
			info->locatable = PCI_MEMBAR_LOCATABLE_64;
		else if (bar & PCI_CONF_MEMBAR_1M)
			info->locatable = PCI_MEMBAR_LOCATABLE_1M;
		else
			info->locatable = PCI_MEMBAR_LOCATABLE_32;
		info->prefetchable = bar & PCI_CONF_MEMBAR_PREFETCH;
		mask = PCI_CONF_MEMBAR_MASK;
	}

	__u64 addr = bar & mask;
	if (info->type == PCI_BAR_TYPE_MEMORY
	    && info->locatable == PCI_MEMBAR_LOCATABLE_64) {
		/* The address can also span the next BAR, unless this is BAR5 */
		if (bar_offset == PCI_BASE_ADDRESS_5)
			return -EINVAL;
		bar_next = pci_readl(dev->addr, bar_offset + PCI_BAR_SIZE);
		addr |= (__u64)bar_next << 32;
	}

	info->paddr = addr;

	__u32 size_lo = 0, size_hi = 0xffffffff;
	__u16 command = pci_readw(dev->addr, PCI_COMMAND_OFFSET);
	command &= ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY);
	pci_writew(dev->addr, PCI_COMMAND_OFFSET, command);

	pci_writel(dev->addr, bar_offset, 0xffffffff);
	size_lo = pci_readl(dev->addr, bar_offset) & mask;
	pci_writel(dev->addr, bar_offset, bar);

	if (info->type == PCI_BAR_TYPE_MEMORY
	    && info->locatable == PCI_MEMBAR_LOCATABLE_64) {
		pci_writel(dev->addr, bar_offset + PCI_BAR_SIZE, 0xffffffff);
		size_hi = pci_readl(dev->addr, bar_offset + PCI_BAR_SIZE);
		pci_writel(dev->addr, bar_offset + PCI_BAR_SIZE, bar_next);

		info->size = ~(size_lo | ((__u64)size_hi << 32)) + 1;

	} else {
		info->size = ~size_lo + 1;
	}

	command |= PCI_COMMAND_IO | PCI_COMMAND_MEMORY;
	pci_writew(dev->addr, PCI_COMMAND_OFFSET, command);

	return 0;
}

#endif /* __QEMU_IVSHMEM_PCI_HELPERS__ */