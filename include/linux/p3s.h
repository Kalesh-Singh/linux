/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_P3S_H
#define _LINUX_P3S_H

#include <linux/const.h>

#define PAGE_SHIFT_4KB		12
#define PAGE_SIZE_4KB		(_AC(1, UL) << PAGE_SHIFT_4KB)
#define PAGE_MASK_4KB		(~(PAGE_SIZE_4KB - 1))

#define VA_BITS_4KB		39

#define LEVEL_SHIFT_4KB		(PAGE_SHIFT_4KB - 3)

#define PMD_SHIFT_4KB		(PAGE_SHIFT_4KB + LEVEL_SHIFT_4KB)
#define PUD_SHIFT_4KB		(PMD_SHIFT_4KB + LEVEL_SHIFT_4KB)
#define P4D_SHIFT_4KB		(PUD_SHIFT_4KB + LEVEL_SHIFT_4KB)

#if CONFIG_PGTABLE_LEVELS == 2
#define PGD_SHIFT_4KB		PMD_SHIFT_4KB
#elif CONFIG_PGTABLE_LEVELS == 3
#define PGD_SHIFT_4KB		PUD_SHIFT_4KB
#elif CONFIG_PGTABLE_LEVELS == 4
#define PGD_SHIFT_4KB		P4D_SHIFT_4KB
#elif CONFIG_PGTABLE_LEVELS == 5
#define PGD_SHIFT_4KB		(P4D_SHIFT_4KB + LEVEL_SHIFT_4KB)
#endif

#define PTRS_PER_PTE_4KB	(_AC(1, UL) << LEVEL_SHIFT_4KB)
#define PTRS_PER_PMD_4KB	(_AC(1, UL) << LEVEL_SHIFT_4KB)
#define PTRS_PER_PUD_4KB	(_AC(1, UL) << LEVEL_SHIFT_4KB)
#define PTRS_PER_P4D_4KB	(_AC(1, UL) << LEVEL_SHIFT_4KB)
#define PTRS_PER_PGD_4KB	(_AC(1, UL) << (VA_BITS_4KB - PGD_SHIFT_4KB))

#define PMD_SIZE_4KB		(_AC(1, UL) << PMD_SHIFT_4KB)
#define PMD_MASK_4KB		(~(PMD_SIZE_4KB - 1))
#define PUD_SIZE_4KB		(_AC(1, UL) << PUD_SHIFT_4KB)
#define PUD_MASK_4KB		(~(PUD_SIZE_4KB - 1))
#define PGDIR_SIZE_4KB		(_AC(1, UL) << PGD_SHIFT_4KB)
#define PGDIR_MASK_4KB		(~(PGDIR_SIZE_4KB - 1))

#define PTE_ADDR_LOW_4KB \
	(((_AC(1, ULL) << (50 - PAGE_SHIFT_4KB)) - 1) << PAGE_SHIFT_4KB)

#define P3S_SLICE_SHIFT		(PAGE_SHIFT - PAGE_SHIFT_4KB)
#define P3S_SLICES_PER_PAGE	(_AC(1, UL) << P3S_SLICE_SHIFT)
#define P3S_SLICE_MASK		(P3S_SLICES_PER_PAGE - 1)

#ifndef __ASSEMBLY__

#include <linux/types.h>
#include <linux/mm_types.h>
#include <linux/sched.h>

#define IS_KERNEL_ADDR(addr)	((long)(addr) < 0)

#define __MM_ADDR_EVAL(addr, kern_val, user_4k_val, user_native_val) \
	(IS_KERNEL_ADDR(addr) ? (kern_val) : (mm_is_4kb(current ? current->mm : NULL) ? (user_4k_val) : (user_native_val)))

#define mm_addr_page_shift(addr) \
	__MM_ADDR_EVAL(addr, PAGE_SHIFT, PAGE_SHIFT_4KB, PAGE_SHIFT)

#define mm_addr_ptrs_per_pte(addr) \
	__MM_ADDR_EVAL(addr, PTRS_PER_PTE, PTRS_PER_PTE_4KB, PTRS_PER_PTE)

#define mm_addr_pmd_shift(addr) \
	__MM_ADDR_EVAL(addr, PMD_SHIFT, PMD_SHIFT_4KB, PMD_SHIFT)

#define mm_addr_ptrs_per_pmd(addr) \
	__MM_ADDR_EVAL(addr, PTRS_PER_PMD, PTRS_PER_PMD_4KB, PTRS_PER_PMD)

#define mm_addr_pmd_size(addr) \
	__MM_ADDR_EVAL(addr, PMD_SIZE, PMD_SIZE_4KB, PMD_SIZE)

#define mm_addr_pmd_mask(addr) \
	__MM_ADDR_EVAL(addr, PMD_MASK, PMD_MASK_4KB, PMD_MASK)

#define mm_addr_pud_shift(addr) \
	__MM_ADDR_EVAL(addr, PUD_SHIFT, PUD_SHIFT_4KB, PUD_SHIFT)

#define mm_addr_ptrs_per_pud(addr) \
	__MM_ADDR_EVAL(addr, PTRS_PER_PUD, PTRS_PER_PUD_4KB, PTRS_PER_PUD)

#define mm_addr_pgd_shift(addr) \
	__MM_ADDR_EVAL(addr, PGDIR_SHIFT, PGD_SHIFT_4KB, PGDIR_SHIFT)

#define mm_addr_ptrs_per_pgd(addr) \
	__MM_ADDR_EVAL(addr, PTRS_PER_PGD, PTRS_PER_PGD_4KB, PTRS_PER_PGD)

#define mm_addr_pgdir_size(addr) \
	__MM_ADDR_EVAL(addr, PGDIR_SIZE, PGDIR_SIZE_4KB, PGDIR_SIZE)

#define mm_addr_pgdir_mask(addr) \
	__MM_ADDR_EVAL(addr, PGDIR_MASK, PGDIR_MASK_4KB, PGDIR_MASK)

static __always_inline unsigned int mm_pte_shift(const struct mm_struct *mm)
{
#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
	return (mm && mm->pte_shift) ? mm->pte_shift : PAGE_SHIFT;
#else
	return PAGE_SHIFT;
#endif
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

static inline bool mm_is_4kb(const struct mm_struct *mm)
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

static inline unsigned long vma_nr_slices(const struct vm_area_struct *vma)
{
	return (vma->vm_end - vma->vm_start) >> mm_pte_shift(vma->vm_mm);
}

static inline pgoff_t vma_native_pages(const struct vm_area_struct *vma)
{
	bool is_compat = mm_is_4kb(vma->vm_mm);
	bool is_anon = !vma->vm_ops;
	unsigned long nr_slices = vma_nr_slices(vma);

	if (!is_compat || is_anon)
		return nr_slices;

	return (vma_slice_off(vma) + nr_slices) >> P3S_SLICE_SHIFT;
}

static inline unsigned int vma_slice_offset(const struct vm_area_struct *vma,
					    unsigned long addr)
{
	unsigned long total_slices;

	if (!mm_is_4kb(vma->vm_mm))
		return 0;

	if (!vma->vm_ops)
		return 0;

	total_slices = ((addr - vma->vm_start) >> PAGE_SHIFT_4KB) +
				vma_slice_off(vma);

	return total_slices & P3S_SLICE_MASK;
}

static inline pgoff_t vma_pgoff_offset(const struct vm_area_struct *vma,
				       unsigned long addr)
{
	if (!mm_is_4kb(vma->vm_mm))
		return vma->vm_pgoff + ((addr - vma->vm_start) >> PAGE_SHIFT);

	if (!vma->vm_ops)
		return vma->vm_pgoff + ((addr - vma->vm_start) >> PAGE_SHIFT_4KB);

	pgoff_t temp = ((addr - vma->vm_start) >> PAGE_SHIFT_4KB) + vma_slice_off(vma);
	return vma->vm_pgoff + (temp >> P3S_SLICE_SHIFT);
}

#define clear_pte_slice_offset(pte)					\
	__pte(pte_val(pte) & ~((PAGE_SIZE - 1) & ~(PAGE_SIZE_4KB - 1)))

static inline unsigned int address_to_slice(struct mm_struct *mm,
					    unsigned long address,
					    unsigned long vm_start,
					    unsigned int vm_slice_off)
{
	if (!mm_is_4kb(mm))
		return 0;

	return (((address - vm_start) >> PAGE_SHIFT_4KB) + vm_slice_off) & P3S_SLICE_MASK;
}

static inline unsigned int vma_address_to_slice(const struct vm_area_struct *vma,
						unsigned long address)
{
	/* Anonymous VMAs have no subpage slices */
	if (!vma->vm_ops)
		return 0;

	return address_to_slice(vma->vm_mm, address, vma->vm_start, vma_slice_off(vma));
}

static inline bool p3s_vma_validate_uffd_alignment(const struct vm_area_struct *vma,
						   unsigned long start, unsigned long end)
{
	if (vma->vm_mm && mm_is_4kb(vma->vm_mm)) {
		return IS_ALIGNED(start, PAGE_SIZE_4KB) &&
		       IS_ALIGNED(end, PAGE_SIZE_4KB);
	}
	return true;
}

#endif /* !__ASSEMBLY__ */

#endif /* _LINUX_P3S_H */
