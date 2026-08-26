/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __VDSO_PAGE_H
#define __VDSO_PAGE_H

#include <uapi/linux/const.h>
#include <asm/p3s.h>

/*
 * 1. Define the absolute kernel constant (pinned to host page size, e.g. 16KB)
 */
#define KERNEL_PAGE_SHIFT   CONFIG_PAGE_SHIFT
#define KERNEL_PAGE_SIZE    (_AC(1, UL) << KERNEL_PAGE_SHIFT)
#define KERNEL_PAGE_MASK    (~(KERNEL_PAGE_SIZE - 1))

/*
 * PAGE_SHIFT determines the default compile-time page size across the kernel.
 */
#define PAGE_SHIFT          KERNEL_PAGE_SHIFT
#define PAGE_SIZE           (_AC(1, UL) << PAGE_SHIFT)

#if !defined(CONFIG_64BIT)
#define PAGE_MASK	(~((1 << CONFIG_PAGE_SHIFT) - 1))
#else
#define PAGE_MASK	(~(PAGE_SIZE - 1))
#endif

#endif	/* __VDSO_PAGE_H */
