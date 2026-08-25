/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_P3S_INLINE_H
#define _LINUX_P3S_INLINE_H

#include <linux/p3s.h>

#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE

static __always_inline unsigned int mm_pte_shift(const struct mm_struct *mm)
{
	return (mm && mm->pte_shift) ? mm->pte_shift : PAGE_SHIFT;
}
#define mm_pte_shift mm_pte_shift

static __always_inline unsigned long mm_pte_size(const struct mm_struct *mm)
{
	return 1UL << mm_pte_shift(mm);
}
#define mm_pte_size mm_pte_size

static __always_inline unsigned long mm_pte_mask(const struct mm_struct *mm)
{
	return ~(mm_pte_size(mm) - 1);
}
#define mm_pte_mask mm_pte_mask

static __always_inline unsigned long mm_offset_in_pte(const struct mm_struct *mm, unsigned long addr)
{
	return addr & (mm_pte_size(mm) - 1);
}
#define mm_offset_in_pte mm_offset_in_pte

static __always_inline unsigned long mm_pte_align(const struct mm_struct *mm, unsigned long val)
{
	return ALIGN(val, mm_pte_size(mm));
}
#define mm_pte_align mm_pte_align

static __always_inline unsigned long mm_pte_align_down(const struct mm_struct *mm, unsigned long val)
{
	return ALIGN_DOWN(val, mm_pte_size(mm));
}
#define mm_pte_align_down mm_pte_align_down

static __always_inline bool mm_pte_aligned(const struct mm_struct *mm, unsigned long val)
{
	return !(val & (mm_pte_size(mm) - 1));
}
#define mm_pte_aligned mm_pte_aligned

#endif /* CONFIG_ARM64_PER_PROCESS_PAGE_SIZE */

#endif /* _LINUX_P3S_INLINE_H */
