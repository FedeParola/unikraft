/* SPDX-License-Identifier: ISC */
/*
 * Author(s): Dan Williams <djwillia@us.ibm.com>
 *            Martin Lucina <martin.lucina@docker.com>
 *            Simon Kuenzer <simon.kuenzer@neclab.eu>
 *
 * Copyright (c) 2016, IBM
 *           (c) 2016-2017 Docker, Inc.
 *           (c) 2017-2018, NEC Europe Ltd.
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose with or without fee is hereby granted, provided
 * that the above copyright notice and this permission notice appear
 * in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <uk/arch/limits.h> /* for __PAGE_SIZE */
#include <uk/plat/common/common.lds.h>

PHDRS
{
	text PT_LOAD FLAGS(PHDRS_PF_RX);
	rodata PT_LOAD FLAGS(PHDRS_PF_R);
	data PT_LOAD;
	tls PT_TLS;
	tls_load PT_LOAD;
	stack PT_GNU_STACK FLAGS(PHDRS_PF_RW);
#ifdef CONFIG_LIBUNIMSG_MEMORY_PROTECTION
	text_unimsg PT_LOAD FLAGS(PHDRS_PF_RX);
	rodata_unimsg PT_LOAD FLAGS(PHDRS_PF_R);
	data_unimsg PT_LOAD FLAGS(PHDRS_PF_RW);
#endif
}

SECTIONS
{
	. = 0x100000;

	_base_addr = .;		/* Symbol to represent the load base address */

	/* Code */
	_text = .;
	.text :
	{
		/* prevent linker gc from removing multiboot header */
		KEEP (*(.data.boot))
		KEEP (*(.data.boot.*))
		*(.text.boot)
		*(.text.boot.*)

		*(.text)
		*(.text.*)
	} :text

	_etext = .;

	/* Read-only data */
	. = ALIGN(__PAGE_SIZE);
	_rodata = .;
	.rodata :
	{
		*(.rodata)
		*(.rodata.*)
	} :rodata

	_erodata = .;

	EXCEPTION_SECTIONS

	CTORTAB_SECTION

	INITTAB_SECTION

	EVENTTAB_SECTION

	/* Constructor tables (read-only) */
	. = ALIGN(0x8);
	_ctors = .;
	.preinit_array : {
		PROVIDE_HIDDEN (__preinit_array_start = .);
		KEEP (*(.preinit_array))
		PROVIDE_HIDDEN (__preinit_array_end = .);
	}

	. = ALIGN(0x8);
	.init_array : {
		PROVIDE_HIDDEN (__init_array_start = .);
		KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
		KEEP (*(.init_array .ctors))
		PROVIDE_HIDDEN (__init_array_end = .);
	}
	_ectors = .;

	TLS_SECTIONS

	DATA_SECTIONS

#ifdef CONFIG_LIBUNIMSG_MEMORY_PROTECTION
	. = ALIGN(__PAGE_SIZE);
	_unimsg_access = .;

	.text_unimsg :
	{
		*(.text_unimsg)
	} :text_unimsg

	.rodata_unimsg ALIGN(__PAGE_SIZE) :
	{
		*(.rodata_unimsg)
	} :rodata_unimsg

	.data_unimsg ALIGN(__PAGE_SIZE) :
	{
		*(.data_unimsg)
	} :data_unimsg

	.bss_unimsg :
	{
		*(.bss_unimsg)
	}

	. = ALIGN(__PAGE_SIZE);
	_end_unimsg_access = .;
	_unimsg_write = .;
	.interrupt_unimsg :
	{
		*(.interrupt_unimsg)
	}
	. = ALIGN(__PAGE_SIZE);
	_end_unimsg_write = .;
#endif

	_end = .;

	.comment       0 : { *(.comment) }
	DEBUG_SYMBOLS

	DISCARDS
}
