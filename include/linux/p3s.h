/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_P3S_H
#define _LINUX_P3S_H

#include <linux/types.h>
#include <linux/mm_types.h>

#define PAGE_SHIFT_4KB		12
#define PAGE_SIZE_4KB		(1UL << PAGE_SHIFT_4KB)
#define PAGE_MASK_4KB		(~(PAGE_SIZE_4KB - 1))

#define VA_BITS_4KB		39

#define P3S_SLICE_SHIFT		(PAGE_SHIFT - PAGE_SHIFT_4KB)
#define P3S_SLICES_PER_PAGE	(1UL << P3S_SLICE_SHIFT)
#define P3S_SLICE_MASK		(P3S_SLICES_PER_PAGE - 1)

static inline bool p3s_mm_is_4kb(const struct mm_struct *mm)
{
#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
	return mm ? (mm->pte_shift == PAGE_SHIFT_4KB) : false;
#else
	return false;
#endif
}

#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
static inline void vma_set_slice_off(struct vm_area_struct *vma, unsigned int val)
{
	vma->vm_slice_off = val;
}
static inline unsigned int vma_slice_off(const struct vm_area_struct *vma)
{
	return vma ? vma->vm_slice_off : 0;
}
#else
static inline void vma_set_slice_off(struct vm_area_struct *vma, unsigned int val) {}
static inline unsigned int vma_slice_off(const struct vm_area_struct *vma)
{
	return 0;
}
#endif

static inline pgoff_t vma_linear_page_index(const struct vm_area_struct *vma,
					    unsigned long address)
{
#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
	if (p3s_mm_is_4kb(vma->vm_mm)) {
		/* For anonymous VMAs, there is no page cache layout to conform to. */
		if (!vma->vm_ops)
			return vma->vm_pgoff + ((address - vma->vm_start) >> PAGE_SHIFT_4KB);

		/*
		 * For file-backed VMAs in compat processes, the VMA offset starts at a
		 * host page boundary (vm_pgoff) but may be offset internally by a
		 * number of subpage slices (vm_slice_off).
		 */
		pgoff_t temp = ((address - vma->vm_start) >> PAGE_SHIFT_4KB) + vma_slice_off(vma);

		return vma->vm_pgoff + (temp >> P3S_SLICE_SHIFT);
	}
#endif
	return vma->vm_pgoff + ((address - vma->vm_start) >> PAGE_SHIFT);
}

#endif /* _LINUX_P3S_H */
