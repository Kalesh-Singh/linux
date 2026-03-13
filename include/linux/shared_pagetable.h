#ifndef _LINUX_SHARED_PAGETABLE_H
#define _LINUX_SHARED_PAGETABLE_H

#include <linux/mm_types.h>

extern struct mm_struct ptshare_mm;

#ifdef CONFIG_SHARED_PAGETABLE
int shpt_validate_mmap(struct file *file, unsigned long addr, unsigned long len,
		       unsigned long prot, unsigned long flags,
		       vm_flags_t vm_flags, unsigned long pgoff);
void shpt_install_vma(struct mm_struct *mm, unsigned long addr,
		      unsigned long len, vm_flags_t vm_flags);
#else
static inline int shpt_validate_mmap(struct file *file, unsigned long addr,
				     unsigned long len, unsigned long prot,
				     unsigned long flags, vm_flags_t vm_flags,
				     unsigned long pgoff)
{
	return 0;
}
static inline void shpt_install_vma(struct mm_struct *mm, unsigned long addr,
				    unsigned long len, vm_flags_t vm_flags)
{
}
#endif

#endif /* _LINUX_SHARED_PAGETABLE_H */
