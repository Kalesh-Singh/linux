/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_P3S_USER_PAGES_H
#define _LINUX_P3S_USER_PAGES_H

#include <asm/p3s.h>

#if defined(CONFIG_ARM64_PER_PROCESS_PAGE_SIZE) && !defined(__ASSEMBLY__)

#undef PAGE_SHIFT
#undef PAGE_SIZE
#undef PAGE_MASK

#define PAGE_SHIFT      p3s_get_dynamic_page_shift()
#define PAGE_SIZE       (1UL << PAGE_SHIFT)
#define PAGE_MASK       (~(PAGE_SIZE - 1))

#undef PFN_PHYS
#undef PHYS_PFN

#define PFN_PHYS(x)	((phys_addr_t)(x) << CONFIG_PAGE_SHIFT)
#define PHYS_PFN(x)	((unsigned long)((x) >> CONFIG_PAGE_SHIFT))

#endif /* CONFIG_ARM64_PER_PROCESS_PAGE_SIZE */

#endif /* _LINUX_P3S_USER_PAGES_H */
