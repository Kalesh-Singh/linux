// SPDX-License-Identifier: GPL-2.0-only
/* arch/arm64/mm/p3s.c */

#include <linux/module.h>
#include <linux/export.h>
#include <linux/sched.h>
#include <linux/mm_types.h>
#include <asm/page.h>
#include <asm/p3s.h>

/* 
 * P3S: Global dummy variable for the Shadow Context Pattern.
 * Must always remain NULL and read-only.
 */
struct mm_struct * const __p3s_shadow_mm = NULL;
EXPORT_SYMBOL(__p3s_shadow_mm);

unsigned long p3s_dynamic_page_shift(struct mm_struct *shadow_mm)
{
	if (shadow_mm && shadow_mm->pte_shift)
		return shadow_mm->pte_shift;

	if (current && current->mm && current->mm->pte_shift)
		return current->mm->pte_shift;

	return CONFIG_PAGE_SHIFT;
}
EXPORT_SYMBOL(p3s_dynamic_page_shift);
