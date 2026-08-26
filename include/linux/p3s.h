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
#define P4D_SIZE_4KB		(_AC(1, UL) << P4D_SHIFT_4KB)
#define P4D_MASK_4KB		(~(P4D_SIZE_4KB - 1))
#define PGDIR_SIZE_4KB		(_AC(1, UL) << PGD_SHIFT_4KB)
#define PGDIR_MASK_4KB		(~(PGDIR_SIZE_4KB - 1))

#define PTE_ADDR_LOW_4KB \
	(((_AC(1, ULL) << (50 - PAGE_SHIFT_4KB)) - 1) << PAGE_SHIFT_4KB)

#define P3S_SLICE_SHIFT		(PAGE_SHIFT - PAGE_SHIFT_4KB)
#define P3S_SLICES_PER_PAGE	(_AC(1, UL) << P3S_SLICE_SHIFT)
#define P3S_SLICE_MASK		(P3S_SLICES_PER_PAGE - 1)

#ifndef __ASSEMBLY__

#include <linux/types.h>
#include <asm/current.h>

struct mm_struct;
struct vm_area_struct;
struct task_struct;
struct linux_binprm;

#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
unsigned long mm_task_size64(void);
unsigned long mm_task_size64_of(struct mm_struct *mm);
unsigned long mm_default_map_window64(void);
#define PGTABLE_MM() \
	((current && current->pgtable_mm) ? \
	 current->pgtable_mm : (current ? (current->mm ? current->mm : current->active_mm) : NULL))
#else
#define mm_task_size64()		(1UL << vabits_actual)
#define mm_task_size64_of(mm)		((void)(mm), (1UL << vabits_actual))
#define mm_default_map_window64()	(1UL << VA_BITS_MIN)
#define PGTABLE_MM()			(current ? (current->mm ? current->mm : current->active_mm) : NULL)
#endif

#define current_pgtable_mm()		PGTABLE_MM()

#define IS_KERNEL_ADDR(addr)	((long)(addr) < 0)

#define __MM_ADDR_EVAL(addr, kern_val, user_4k_val, user_native_val) \
	(IS_KERNEL_ADDR(addr) ? (kern_val) : (mm_is_4kb(current_pgtable_mm()) ? (user_4k_val) : (user_native_val)))

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

#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
#define mm_pte_shift(mm)	((mm) && (mm)->pte_shift ? (mm)->pte_shift : PAGE_SHIFT)
#define mm_is_4kb(mm)		((mm) ? ((mm)->pte_shift == PAGE_SHIFT_4KB) : false)
#else
#define mm_pte_shift(mm)	PAGE_SHIFT
#define mm_is_4kb(mm)		false
#endif

#define mm_pte_size(mm)		(1UL << mm_pte_shift(mm))
#define mm_pte_mask(mm)		(~(mm_pte_size(mm) - 1))
#define mm_offset_in_pte(mm, addr)	((addr) & (mm_pte_size(mm) - 1))
#define mm_pte_align(mm, val)		ALIGN(val, mm_pte_size(mm))
#define mm_pte_align_down(mm, val)	ALIGN_DOWN(val, mm_pte_size(mm))
#define mm_pte_aligned(mm, val)		(!((val) & (mm_pte_size(mm) - 1)))
#define MM_PHYS_PFN(mm, x)		((x) >> mm_pte_shift(mm))

#define MM_PAGE_SIZE(mm)	(mm_pte_size(mm))
#define MM_PAGE_MASK(mm)	(mm_pte_mask(mm))
#define MM_PMD_SIZE(mm)		(mm_is_4kb(mm) ? PMD_SIZE_4KB : PMD_SIZE)
#define MM_PMD_MASK(mm)		(mm_is_4kb(mm) ? PMD_MASK_4KB : PMD_MASK)
#define MM_PUD_MASK(mm)		(mm_is_4kb(mm) ? PUD_MASK_4KB : PUD_MASK)
#define MM_P4D_MASK(mm)		(mm_is_4kb(mm) ? P4D_MASK_4KB : P4D_MASK)
#define MM_PGDIR_MASK(mm)	(mm_is_4kb(mm) ? PGDIR_MASK_4KB : PGDIR_MASK)

#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
#define mm_set_pgtable_mm(mm)		do { if (current) current->pgtable_mm = (mm); } while (0)
#define mm_clear_pgtable_mm()		do { if (current) current->pgtable_mm = NULL; } while (0)
#define mm_set_bprm_exec(bprm)		do { if (current) current->bprm_exec = (bprm); } while (0)
#define mm_clear_bprm_exec()		do { if (current) current->bprm_exec = NULL; } while (0)
#define mm_get_bprm_exec()		(current ? current->bprm_exec : NULL)

#define mm_init_pagesize(mm, bprm)	do { \
	if (personality_4kb_pages(current->personality)) \
		(mm)->pte_shift = PAGE_SHIFT_4KB; \
} while (0)
#else
#define mm_set_pgtable_mm(mm)		do { } while (0)
#define mm_clear_pgtable_mm()		do { } while (0)
#define mm_set_bprm_exec(bprm)		do { } while (0)
#define mm_clear_bprm_exec()		do { } while (0)
#define mm_get_bprm_exec()		(NULL)
#define mm_init_pagesize(mm, bprm)	do { } while (0)
#endif

#endif /* !__ASSEMBLY__ */

#endif /* _LINUX_P3S_H */

#if defined(_LINUX_MMAP_LOCK_H) && !defined(_LINUX_P3S_VMA_INDEX_H)
#define _LINUX_P3S_VMA_INDEX_H
#ifndef __ASSEMBLY__

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
#endif /* _LINUX_MMAP_LOCK_H && !_LINUX_P3S_VMA_INDEX_H */
