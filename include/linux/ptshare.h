#ifndef _LINUX_PTSHARE_H
#define _LINUX_PTSHARE_H

#include <linux/mm_types.h>
#include <linux/mman.h>
#include <linux/mm.h>
#include <linux/mmu_notifier.h>

extern struct mm_struct *ptshare_mm;
struct ptshare_desc {
	struct mm_struct *ptshare_mm;		/* The headless shadow MM for this domain */
	struct mmu_notifier mmu_notifier;	/* Notifier for TLB consistency */
};

static inline bool has_map_hugetlb_flag(unsigned long flags)
{
	return flags & MAP_HUGETLB && !IS_ENABLED(CONFIG_PTSHARE);
}

static inline bool vma_shares_pagetables(const struct vm_area_struct *vma)
{
	return vma->vm_flags & VM_PT_SHARED;
}

/**
 * ptdesc_get - Increment reference count on a page table descriptor
 * @pt: The page table descriptor.
 */
static inline void ptdesc_get(struct ptdesc *pt)
{
	folio_get(ptdesc_folio(pt));
}

/**
 * ptdesc_put - Decrement reference count on a page table descriptor
 * @pt: The page table descriptor.
 */
static inline void ptdesc_put(struct ptdesc *pt)
{
	folio_put(ptdesc_folio(pt));
}

/**
 * ptdesc_refcount - Return the reference count on a page table descriptor
 * @pt: The page table descriptor.
 */
static inline int ptdesc_refcount(struct ptdesc *pt)
{
	return folio_ref_count(ptdesc_folio(pt));
}

#ifdef CONFIG_PTSHARE
int ptshare_validate_mmap(struct file *file, unsigned long addr,
			  unsigned long len, unsigned long prot,
			  unsigned long flags, vm_flags_t vm_flags,
			  unsigned long pgoff);

unsigned long ptshare_install_vma(struct mm_struct *mm, unsigned long addr);

vm_fault_t ptshare_handle_mm_fault(struct vm_fault *vmf);
bool ptshare_vma_skip_zap_pte_range(struct vm_area_struct *vma);
bool ptshare_free_pte_range(struct mm_struct *mm, pgtable_t token);
#else /* !CONFIG_PTSHARE */
static inline int ptshare_validate_mmap(struct file *file, unsigned long addr,
					unsigned long len, unsigned long prot,
					unsigned long flags, vm_flags_t vm_flags,
					unsigned long pgoff)
{
	return 0;
}

static inline unsigned long ptshare_install_vma(struct mm_struct *mm,
						unsigned long addr)
{
	return addr;
}

static inline vm_fault_t ptshare_handle_mm_fault(struct vm_fault *vmf)
{
	return VM_FAULT_SIGBUS;
}

static inline bool ptshare_vma_skip_zap_pte_range(struct vm_area_struct *vma)
{
	return false;
}

static inline bool ptshare_free_pte_range(struct mm_struct *mm, pgtable_t token)
{
	return false;
}
#endif /* CONFIG_PTSHARE */
#endif /* _LINUX_PTSHARE_H */