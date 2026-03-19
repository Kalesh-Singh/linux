#ifndef _LINUX_PTSHARE_VMA_H
#define _LINUX_PTSHARE_VMA_H

#include <linux/mm_types.h>

int ptshare_vma_refcount_init(struct vm_area_struct *vma);
void ptshare_vma_refcount_destroy(struct vm_area_struct *vma);

#endif /* _LINUX_PTSHARE_VMA_H */