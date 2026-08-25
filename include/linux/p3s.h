/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_P3S_H
#define _LINUX_P3S_H

#include <linux/types.h>
#include <linux/mm_types.h>

#define PAGE_SHIFT_4KB		12
#define PAGE_SIZE_4KB		(1UL << PAGE_SHIFT_4KB)
#define PAGE_MASK_4KB		(~(PAGE_SIZE_4KB - 1))

#define VA_BITS_4KB		39

#endif /* _LINUX_P3S_H */
