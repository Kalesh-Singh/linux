#ifndef _LINUX_PTSHARE_H
#define _LINUX_PTSHARE_H

#include <linux/mm_types.h>
#include <linux/mman.h>
#include <linux/mm.h>
#include <linux/mmu_notifier.h>

/*
 * Use a unique lockdep subclass for ptshare_mm->mmap_lock to allow
 * nesting it under other MM locks (e.g. during fork/dup_mmap).
 *
 * 0: Default (Parent MM in dup_mmap)
 * 1: SINGLE_DEPTH_NESTING (Child MM in dup_mmap)
 * 2: PTSHARE_MMAP_LOCK_NESTING (ptshare_mm)
 */
#define PTSHARE_MMAP_LOCK_NESTING 2

struct ptshare_desc {
	struct mm_struct *ptshare_mm;		/* The headless shadow MM for this domain */
	struct mmu_notifier mmu_notifier;	/* Notifier for TLB consistency */
	refcount_t refcount;			/* Refcount for the descriptor itself */
};

static inline bool has_map_hugetlb_flag(unsigned long flags)
{
	return flags & MAP_HUGETLB && !IS_ENABLED(CONFIG_PTSHARE);
}

static inline bool vma_shares_pagetables(const struct vm_area_struct *vma)
{
	return vma->vm_flags & VM_PT_SHARED;
}

/* For client VMAs */
static inline struct ptshare_desc *vma_ptshare(struct vm_area_struct *vma)
{
#ifdef CONFIG_PTSHARE
	/*
	 * Shadow VMAs belong to a ptshare_mm, which we can identify.
	 * Client VMAs are those that have VM_PT_SHARED set.
	 */
	if (vma_shares_pagetables(vma))
		return vma->vm_ptshare;
#endif
	return NULL;
}

/* For shadow VMAs */
static inline refcount_t *vma_ptshare_refcount(struct vm_area_struct *vma)
{
#ifdef CONFIG_PTSHARE
	/* Shadow VMAs are in a ptshare_mm and do NOT have VM_PT_SHARED set */
	return &vma->vm_ptshare_refcount;
#endif
	return NULL;
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

struct ptshare_desc *ptshare_alloc_desc(void);
void ptshare_put_desc(struct ptshare_desc *desc);
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

static inline struct ptshare_desc *ptshare_alloc_desc(void)
{
	return NULL;
}

static inline void ptshare_put_desc(struct ptshare_desc *desc)
{
}
#endif /* CONFIG_PTSHARE */
#endif /* _LINUX_PTSHARE_H */