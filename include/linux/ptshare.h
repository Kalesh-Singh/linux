#ifndef _LINUX_PTSHARE_H
#define _LINUX_PTSHARE_H

#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/mm_types.h>
#include <linux/refcount.h>

struct ptshare_desc {
	/* The headless shadow MM for this domain */
	struct mm_struct *ptshare_mm;
	refcount_t refcount;
};

static inline bool vma_shares_pagetables(const struct vm_area_struct *vma)
{
	return vma->vm_flags & VM_PT_SHARED;
}

static inline bool has_map_hugetlb_flag(unsigned long flags)
{
	return flags & MAP_HUGETLB && !IS_ENABLED(CONFIG_PTSHARE);
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
static inline struct ptshare_desc *vma_ptshare_desc(const struct vm_area_struct *vma)
{
	/*
	 * Shadow VMAs belong to a ptshare_mm, and do NOT have VM_PT_SHARED set.
	 * Client VMAs are those that have VM_PT_SHARED set.
	 */
	BUG_ON(!vma_shares_pagetables(vma));

	return vma->__vm_ptshare_desc;
}

static inline void vma_set_ptshare_desc(struct vm_area_struct *vma,
					struct ptshare_desc *desc)
{
	BUG_ON(!vma_shares_pagetables(vma));

	vma->__vm_ptshare_desc = desc;
}

static inline refcount_t *vma_ptshare_refcount(struct vm_area_struct *vma)
{
	/* Shadow VMAs are in a ptshare_mm and do NOT have VM_PT_SHARED set */
	BUG_ON(vma_shares_pagetables(vma));

	return &vma->__vm_ptshare_refcount;
}

struct ptshare_desc *ptshare_alloc_desc(void);

void ptshare_put_desc(struct ptshare_desc *desc);


void ptshare_get_desc(struct ptshare_desc *desc);

int ptshare_validate_mmap(struct file *file, unsigned long addr,
			  unsigned long len, unsigned long prot,
			  unsigned long flags, vm_flags_t vm_flags,
			  unsigned long pgoff);

void ptshare_get_vma(struct vm_area_struct *vma, struct ptshare_desc *desc);

void ptshare_put_vma(struct vm_area_struct *vma, struct ptshare_desc *desc);

unsigned long ptshare_install_vma(struct mm_struct *mm, unsigned long addr);

vm_fault_t ptshare_handle_mm_fault(struct vm_fault *vmf);

bool ptshare_vma_skip_zap_pte_range(struct vm_area_struct *vma);

bool ptshare_free_pte_range(pgtable_t ptdesc);
#else /* !CONFIG_PTSHARE */
static inline struct ptshare_desc *vma_ptshare_desc(const struct vm_area_struct *vma)
{
	return NULL;
}

static inline void vma_set_ptshare_desc(struct vm_area_struct *vma,
					struct ptshare_desc *desc)
{
}

static inline refcount_t *vma_ptshare_refcount(struct vm_area_struct *vma)
{
	return NULL;
}

static inline struct ptshare_desc *ptshare_alloc_desc(void)
{
	return NULL;
}

static inline void ptshare_put_desc(struct ptshare_desc *desc)
{
}

static inline void ptshare_get_desc(struct ptshare_desc *desc)
{
}

static inline int ptshare_validate_mmap(struct file *file, unsigned long addr,
					unsigned long len, unsigned long prot,
					unsigned long flags, vm_flags_t vm_flags,
					unsigned long pgoff)
{
	return 0;
}

static inline void ptshare_get_vma(struct vm_area_struct *vma,
					struct ptshare_desc *desc)
{
}

static inline void ptshare_put_vma(struct vm_area_struct *vma,
					struct ptshare_desc *desc)
{
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

static inline bool ptshare_free_pte_range(pgtable_t ptdesc)
{
	return false;
}
#endif /* CONFIG_PTSHARE */

#endif /* _LINUX_PTSHARE_H */
