#ifndef _LINUX_PTSHARE_H
#define _LINUX_PTSHARE_H

#include <linux/mm_types.h>
#include <linux/mm.h>

extern struct mm_struct *ptshare_mm;

static inline bool vma_shares_pagetables(const struct vm_area_struct *vma)
{
	return vma->vm_flags & VM_PT_SHARED;
}

#ifdef CONFIG_PTSHARE
int ptshare_validate_mmap(struct file *file, unsigned long addr,
			  unsigned long len, unsigned long prot,
			  unsigned long flags, vm_flags_t vm_flags,
			  unsigned long pgoff);
#else /* !CONFIG_PTSHARE */
static inline int ptshare_validate_mmap(struct file *file, unsigned long addr,
					unsigned long len, unsigned long prot,
					unsigned long flags, vm_flags_t vm_flags,
					unsigned long pgoff)
{
	return 0;
}
#endif /* CONFIG_PTSHARE */
#endif /* _LINUX_PTSHARE_H */