/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_P3S_H
#define __ASM_P3S_H

#ifndef __ASSEMBLY__

#include <linux/compiler.h>

struct mm_struct;

/*
 * Global fallback pointer.
 * Initialized to NULL, read-only. Exists purely to be shadowed.
 */
extern const struct mm_struct * const __p3s_shadow_mm;

#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
unsigned long p3s_dynamic_page_shift(const struct mm_struct *shadow_mm);

static __always_inline unsigned long p3s_get_dynamic_page_shift(void)
{
	return p3s_dynamic_page_shift(__p3s_shadow_mm);
}
#else
static __always_inline unsigned long p3s_get_dynamic_page_shift(void)
{
	return CONFIG_PAGE_SHIFT;
}
#endif

/*
 * P3S_CONTEXT_REMOTE_MM: Injects a local variable that shadows the global.
 * The `(void)__p3s_shadow_mm` forces a statement, preventing this from 
 * being used illegally directly after an `if` without braces.
 */
#define P3S_CONTEXT_REMOTE_MM(remote_mm) \
	const struct mm_struct *__p3s_shadow_mm = (remote_mm); \
	(void)__p3s_shadow_mm

#endif /* !__ASSEMBLY__ */

#endif /* __ASM_P3S_H */
