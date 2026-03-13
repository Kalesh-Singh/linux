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

#endif /* _LINUX_PTSHARE_H */
