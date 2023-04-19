/*
 * Some sort of Copyright
 */

#include <ivshmem.h>
#include <uk/plat/lcpu.h>
#ifdef CONFIG_HAVE_PAGING
#include <uk/plat/paging.h>
#endif
#include "pci_helpers.h"

#define QEMU_IVSHMEM_VENDOR_ID 0x1af4
#define QEMU_IVSHMEM_DEVICE_ID 0x1110
/* TODO: make this a configuration parameter */
#define QEMU_IVSHMEM_MAX_VECTORS 8
/* TODO: find a better way to prevent irq clashes */
#define QEMU_IVSHMEM_BASE_IRQ 1
/* Edge-triggered, de-asserted vector */
#define QEMU_IVSHMEM_MSIX_VECTOR_DATA_BASE (1 << 14)
/* TODO: find a way to get a reliable vaddr to map bars */
#define IVSHMEM_BAR_VADDR_START 0x100000000

struct ivshmem_doorbell {
	__u32 interrupt_mask; /* Unused */
	__u32 interrupt_status; /* Unused */
	/* Unique id of the device in a group of VMs sharing memory */
	__u32 iv_position;
	/* (vector & 0xffff) | ((peer_id & 0xffff) << 16) */
	volatile __u32 doorbell;
	char reserved[240];
} __attribute__((packed));

struct ivshmem_dev {
	struct uk_list_head list;
	/* Use an incremental id to tell multiple ivshmem devices apart.
	 * TODO: think of something better.
	 */
	unsigned id;
	enum qemu_ivshmem_type type;
	void *addr;
	size_t size;
	struct ivshmem_doorbell *doorbell;
	unsigned vectors_count;
	struct msix_vector *vectors;
	struct pci_device *pdev;
};

static struct uk_alloc *driver_allocator;
static UK_LIST_HEAD(ivshmem_devs_list);
static unsigned ivshmem_devs_count = 0;
#ifdef CONFIG_HAVE_PAGING
static __vaddr_t next_bar_vaddr = IVSHMEM_BAR_VADDR_START;
#endif

static struct ivshmem_dev *get_ivshmem(unsigned ivshmem_id)
{
	struct ivshmem_dev *dev, *dev_next;

	uk_list_for_each_entry_safe(dev, dev_next, &ivshmem_devs_list, list) {
		if (ivshmem_id == 0)
			return dev;
		ivshmem_id--;
	}

	return NULL;
}

int qemu_ivshmem_get_info(unsigned ivshmem_id, struct qemu_ivshmem_info *info)
{
	if (!info)
		return -EINVAL;

	struct ivshmem_dev *dev = get_ivshmem(ivshmem_id);
	if (!dev)
		return -ENODEV;

	/* Fill all fields even if they are meaningless, it is up to the user to
	 * discard them
	 */
	info->type = dev->type;
	info->addr = dev->addr;
	info->size = dev->size;
	info->doorbell_id = dev->doorbell ? dev->doorbell->iv_position : 0;
	info->vectors_count = dev->vectors_count;

	return 0;
}

int qemu_ivshmem_set_interrupt_handler(unsigned ivshmem_id, unsigned vector,
				       uk_intctlr_irq_handler_func_t handler,
				       void *arg)
{
	int rc;

	struct ivshmem_dev *dev = get_ivshmem(ivshmem_id);
	if (!dev)
		return -ENODEV;

	if (vector > dev->vectors_count
	    || dev->type != QEMU_IVSHMEM_TYPE_DOORBELL)
		return -EINVAL;

	unsigned long irq = QEMU_IVSHMEM_BASE_IRQ + vector;
	rc = uk_intctlr_irq_register(irq, handler, arg);
	if (rc)
		return rc;

	dev->vectors[vector].data = ((32 + irq) & 0xff)
				    | QEMU_IVSHMEM_MSIX_VECTOR_DATA_BASE;
	dev->vectors[vector].ctrl &= ~MSIX_VECTOR_CTRL_MASKED;

	uk_pr_info("Device %u: registered interrupt handler for vector %u\n",
		   ivshmem_id, vector);

	return 0;
}

int qemu_ivshmem_interrupt_peer(unsigned ivshmem_id, __u16 peer_id,
				__u16 vector) {
	struct ivshmem_dev *dev = get_ivshmem(ivshmem_id);
	if (!dev)
		return -ENODEV;

	if (dev->type != QEMU_IVSHMEM_TYPE_DOORBELL)
		return -EINVAL;

	dev->doorbell->doorbell = vector | ((__u32)peer_id << 16);

	return 0;
}

static int get_bar_vaddr(struct pci_bar_info bar, void **addr)
{
	UK_ASSERT(addr);

#ifdef CONFIG_HAVE_PAGING
	unsigned pages = bar.size == 0 ? 0 : (bar.size - 1) / PAGE_SIZE + 1;
	int rc = ukplat_page_map(ukplat_pt_get_active(), next_bar_vaddr,
				 bar.paddr, pages, PAGE_ATTR_PROT_RW, 0);
	*addr = (void *)next_bar_vaddr;
	next_bar_vaddr += pages * PAGE_SIZE;
	return rc;
#else
	*addr = (void *)bar.paddr;
	return 0;
#endif
}

static int qemu_ivshmem_add_dev(struct pci_device *pci_dev)
{
	struct ivshmem_dev *ivshmem_dev = NULL;

	UK_ASSERT(pci_dev != NULL);

	ivshmem_dev = uk_malloc(driver_allocator, sizeof(*ivshmem_dev));
	if (!ivshmem_dev) {
		uk_pr_err("Failed to allocate QEMU ivshmem device\n");
		return -ENOMEM;
	}

	ivshmem_dev->pdev = pci_dev;

	/* Retrieve shared memory information */
	struct pci_bar_info bar2;
	if (pci_get_bar_info(pci_dev, PCI_BASE_ADDRESS_2, &bar2)) {
		uk_pr_err("Unable to read BAR2\n");
		goto out_free;
	}
	if (bar2.type != PCI_BAR_TYPE_MEMORY) {
		uk_pr_err("Unexpected configuration of BAR2\n");
		goto out_free;
	}

	int rc = get_bar_vaddr(bar2, &ivshmem_dev->addr);
	if (rc) {
		uk_pr_err("Error mapping BAR2 address: %s\n", strerror(-rc));
		goto out_free;
	}
	ivshmem_dev->size = bar2.size;

	ivshmem_dev->type = QEMU_IVSHMEM_TYPE_PLAIN;

	/* Look for interrupts (MSI-X) capability. Implies a doorbell device */
	if (!(pci_readw(pci_dev->addr, PCI_STATUS_OFFSET)
	      & PCI_STATUS_CAP_LIST))
		goto skip_doorbell;

	struct pci_cap_header cap_hdr;
	__u8 cap_ptr = pci_readb(pci_dev->addr, PCI_CAPABILITIES_PTR)
		       & PCI_CAPABILITIES_PTR_MASK;

	while (cap_ptr != 0) {
		*(__u16 *)&cap_hdr = pci_readw(pci_dev->addr, cap_ptr);
		if (cap_hdr.id == PCI_CAP_MSIX_ID)
			break;
		cap_ptr = cap_hdr.next_ptr & PCI_CAPABILITIES_PTR_MASK;
	}

	if (cap_ptr == 0)
		goto skip_doorbell;

	__u16 msg_ctrl = pci_readw(pci_dev->addr, cap_ptr + PCI_CAP_MSIX_CTRL);
	ivshmem_dev->vectors_count =
			(msg_ctrl & PCI_CAP_MSIX_CTRL_TABLE_SIZE_MASK) + 1;

	if (ivshmem_dev->vectors_count > QEMU_IVSHMEM_MAX_VECTORS) {
		uk_pr_warn("The device provides more vectors than supported by "
			   "this driver, using only %d\n",
			   QEMU_IVSHMEM_MAX_VECTORS);
		ivshmem_dev->vectors_count = QEMU_IVSHMEM_MAX_VECTORS;
	}

	__u32 bir_table_offset = pci_readl(pci_dev->addr,
					   cap_ptr
					   + PCI_CAP_MSIX_BIR_TABLE_OFFSET);
	if ((bir_table_offset & PCI_CAP_MSIX_BIR_TABLE_OFFSET_BIR_MASK) != 1) {
		uk_pr_err("Unexpected MSI-X configuration\n");
		goto out_free;
	}

	struct pci_bar_info bar1;
	if (pci_get_bar_info(pci_dev, PCI_BASE_ADDRESS_1, &bar1)) {
		uk_pr_err("Unable to read BAR1\n");
		goto out_free;
	}
	if (bar1.type != PCI_BAR_TYPE_MEMORY
	    || bar1.locatable != PCI_MEMBAR_LOCATABLE_32) {
		uk_pr_err("Unexpected configuration of BAR1\n");
		goto out_free;
	}

	void *addr;
	rc = get_bar_vaddr(bar1, &addr);
	if (rc) {
		uk_pr_err("Error mapping BAR1 address: %s\n", strerror(-rc));
		goto out_free;
	}
	ivshmem_dev->vectors = addr
			       + (bir_table_offset
			          & ~PCI_CAP_MSIX_BIR_TABLE_OFFSET_BIR_MASK);

	/* Mask all vectors */
	for (unsigned i = 0; i < ivshmem_dev->vectors_count; i++)
		ivshmem_dev->vectors[i].ctrl |= MSIX_VECTOR_CTRL_MASKED;

	/* Enable interrupts */
	pci_writew(pci_dev->addr, cap_ptr + PCI_CAP_MSIX_CTRL,
		   msg_ctrl | PCI_CAP_MSIX_CTRL_ENABLE);

	/* Round-robin assing vectors to CPUs: vector N handled by cpu
	 * N % NCPUS
	 */
	for (unsigned vec = 0; vec < ivshmem_dev->vectors_count; vec++)
		ivshmem_dev->vectors[vec].addr = 0xfee00000
						 | ((vec % ukplat_lcpu_count())
						    << 12);

	/* Retrieve doorbell information */
	struct pci_bar_info bar0;
	if (pci_get_bar_info(pci_dev, PCI_BASE_ADDRESS_0, &bar0)) {
		uk_pr_err("Unable to read BAR0\n");
		goto out_free;
	}
	if (bar0.type != PCI_BAR_TYPE_MEMORY
	    || bar0.locatable != PCI_MEMBAR_LOCATABLE_32
	    || bar0.size != sizeof(struct ivshmem_doorbell)) {
		uk_pr_err("Unexpected configuration of BAR0\n");
		goto out_free;
	}

	rc = get_bar_vaddr(bar0, (void **)&ivshmem_dev->doorbell);
	if (rc) {
		uk_pr_err("Error mapping BAR0 address: %s\n", strerror(-rc));
		goto out_free;
	}

	ivshmem_dev->type = QEMU_IVSHMEM_TYPE_DOORBELL;

skip_doorbell:
	ivshmem_dev->id = ivshmem_devs_count++;
	uk_list_add_tail(&ivshmem_dev->list, &ivshmem_devs_list);

	uk_pr_info("Added QEMU ivshmem-%s device: id=%u, addr=%p, size=%lu",
		   ivshmem_dev->type == QEMU_IVSHMEM_TYPE_DOORBELL
		   ? "doorbell" : "plain", ivshmem_dev->id, ivshmem_dev->addr,
		   ivshmem_dev->size);
	if (ivshmem_dev->type == QEMU_IVSHMEM_TYPE_DOORBELL)
		uk_pr_info(", vectors=%u, doorbell_id=%u\n",
			   ivshmem_dev->vectors_count,
			   ivshmem_dev->doorbell->iv_position);
	else
		uk_pr_info("\n");

	return 0;

out_free:
	uk_free(driver_allocator, ivshmem_dev);
	return -ENODEV;
}

static int qemu_ivshmem_drv_init(struct uk_alloc *drv_allocator)
{
	if (!drv_allocator)
		return -EINVAL;

	driver_allocator = drv_allocator;
	return 0;
}

static const struct pci_device_id qemu_ivshmem_ids[] = {
	{PCI_DEVICE_ID(QEMU_IVSHMEM_VENDOR_ID, QEMU_IVSHMEM_DEVICE_ID)},
	/* End of Driver List */
	{PCI_ANY_DEVICE_ID},
};

static struct pci_driver qemu_ivshmem_drv = {
	.device_ids = qemu_ivshmem_ids,
	.init = qemu_ivshmem_drv_init,
	.add_dev = qemu_ivshmem_add_dev
};
PCI_REGISTER_DRIVER(&qemu_ivshmem_drv);