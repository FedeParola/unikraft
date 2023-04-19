/*
 * Some sort of Copyright
 */

#ifndef __UKPLAT_QEMU_IVSHMEM__
#define __UKPLAT_QEMU_IVSHMEM__

#include <stddef.h>
#include <uk/plat/irq.h>

enum qemu_ivshmem_type {
	QEMU_IVSHMEM_TYPE_PLAIN,
	QEMU_IVSHMEM_TYPE_DOORBELL,
};

struct qemu_ivshmem_info {
	enum qemu_ivshmem_type type;
	void *addr;
	size_t size;
	/* The following fields are valid only for devices of type DOORBELL */
	unsigned doorbell_id;
	unsigned vectors_count;
};

/**
 * Retrieves information about the device.
 * @param ivshmem_id ID of the ivshmem device
 * @param info struct qemu_ivshmem_info variable to fill with device infos
 * @return 0 on success, a negative errno value on errors
 */
int qemu_ivshmem_get_info(unsigned ivshmem_id, struct qemu_ivshmem_info *info);

/**
 * Registers an interrupt handler for an MSI-X vector of the device. At the
 * moment there is no way to unregister an handler, due to unikraft internal
 * limitation.
 * @param ivshmem_id ID of the ivshmem device
 * @param vector MSI-X interrupt vector number
 * @param func Interrupt function
 * @param arg Extra argument to be handover to interrupt function
 * @return 0 on success, a negative errno value on errors
 */
int qemu_ivshmem_set_interrupt_handler(unsigned ivshmem_id, unsigned vector, 
				       irq_handler_func_t func, void *arg);

/**
 * Sends an interrupt to the specified peer on the specified MSI-X vector.
 * The ivshmem device doesn't provide a mechanism to know it the peer and vector
 * actually exist.
 * @param ivshmem_id ID of the ivshmem device
 * @param peer_id ID of the peer
 * @param vector MSI-X vector of the peer
 * @return 0 on success, a negative errno value on errors
 */
int qemu_ivshmem_interrupt_peer(unsigned ivshmem_id, __u16 peer_id, 
				__u16 vector);

/* Do we need an enable/disable vector function? */

#endif /* __UKPLAT_QEMU_IVSHMEM__ */