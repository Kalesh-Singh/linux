#ifndef _LINUX_SHARED_PAGETABLE_H
#define _LINUX_SHARED_PAGETABLE_H

#include <linux/mm_types.h>

extern struct mm_struct ptshare_mm;

#ifdef CONFIG_SHARED_PAGETABLE
vm_fault_t shpt_handle_fault(struct vm_fault *vmf);
int shpt_validate_mmap(struct file *file, unsigned long addr, unsigned long len,
		       unsigned long prot, unsigned long flags,
		       vm_flags_t vm_flags, unsigned long pgoff);
void shpt_install_vma(struct mm_struct *mm, unsigned long addr,
		      unsigned long len, vm_flags_t vm_flags);
#else
static inline vm_fault_t shpt_handle_fault(struct vm_fault *vmf)
{
	return VM_FAULT_SIGBUS;
}
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
