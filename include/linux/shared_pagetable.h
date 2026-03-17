#ifndef _LINUX_SHARED_PAGETABLE_H
#define _LINUX_SHARED_PAGETABLE_H

#include <linux/mm_types.h>
#include <linux/sched.h>
#include <linux/string.h>

extern struct mm_struct *ptshare_mm;

#define shpt_err(fmt, ...) \
do { \
	if (!strcmp(current->comm, "ptshare_test")) \
		pr_err("SHPT_DEBUG: [%i (%s)]: " fmt, task_pid_nr(current), current->comm, ## __VA_ARGS__); \
} while (0)

#define shpt_info(fmt, ...) \
do { \
	if (!strcmp(current->comm, "ptshare_test")) \
		pr_info("SHPT_DEBUG: [%i (%s)]: " fmt, task_pid_nr(current), current->comm, ## __VA_ARGS__); \
} while (0)

#define shpt_mm_err(mm, fmt, ...) \
do { \
	if (!strcmp(current->comm, "ptshare_test")) \
		pr_err("SHPT_DEBUG: [%i (%s)] [%s]: " fmt, task_pid_nr(current), current->comm, \
		       (mm == ptshare_mm ? "ptshare_mm" : "private_mm"), ## __VA_ARGS__); \
} while (0)

#define shpt_mm_info(mm, fmt, ...) \
do { \
	if (!strcmp(current->comm, "ptshare_test")) \
		pr_info("SHPT_DEBUG: [%i (%s)] [%s]: " fmt, task_pid_nr(current), current->comm, \
		       (mm == ptshare_mm ? "ptshare_mm" : "private_mm"), ## __VA_ARGS__); \
} while (0)

#ifdef CONFIG_SHARED_PAGETABLE
vm_fault_t shpt_handle_fault(struct vm_fault *vmf);
int shpt_validate_mmap(struct file *file, unsigned long addr, unsigned long len,
		       unsigned long prot, unsigned long flags,
		       vm_flags_t vm_flags, unsigned long pgoff);
int shpt_install_vma(struct mm_struct *mm, unsigned long addr,
		      unsigned long len, vm_flags_t vm_flags);
void shpt_vma_get(struct vm_area_struct *vma);
void shpt_vma_put(struct vm_area_struct *vma);
int shpt_unshare_vma(struct vm_area_struct *vma);
int shpt_unshare_remote_vma(struct mm_struct *mm, unsigned long addr, bool write);
int shpt_unshare_madvise_range(struct mm_struct *mm, unsigned long start,
			      unsigned long len, int behavior);
int shpt_unshare_madvise_vector_range(struct mm_struct *mm, struct iov_iter *iter,
				      int behavior);
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
static inline int shpt_install_vma(struct mm_struct *mm, unsigned long addr,
				    unsigned long len, vm_flags_t vm_flags)
{
	return 0;
}
static inline void shpt_vma_get(struct vm_area_struct *vma)
{
}
static inline void shpt_vma_put(struct vm_area_struct *vma)
{
}
static inline int shpt_unshare_vma(struct vm_area_struct *vma)
{
	return 0;
}
static inline int shpt_unshare_remote_vma(struct mm_struct *mm, unsigned long addr, bool write)
{
	return 0;
}
static inline int shpt_unshare_madvise_range(struct mm_struct *mm, unsigned long start,
					     unsigned long len, int behavior)
{
	return 0;
}
static inline int shpt_unshare_madvise_vector_range(struct mm_struct *mm,
						    struct iov_iter *iter,
						    int behavior)
{
	return 0;
}
#endif

#endif /* _LINUX_SHARED_PAGETABLE_H */
