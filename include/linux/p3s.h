/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_P3S_H
#define _LINUX_P3S_H

#include <linux/types.h>
#include <linux/mm_types.h>

#define PAGE_SHIFT_4KB		12
#define PAGE_SIZE_4KB		(1UL << PAGE_SHIFT_4KB)
#define PAGE_MASK_4KB		(~(PAGE_SIZE_4KB - 1))

#define VA_BITS_4KB		39

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

#endif /* _LINUX_P3S_H */
