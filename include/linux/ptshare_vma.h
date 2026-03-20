#ifndef _LINUX_PTSHARE_VMA_H
#define _LINUX_PTSHARE_VMA_H

#include <linux/mm_types.h>

#ifdef CONFIG_PTSHARE
int ptshare_vma_refcount_init(struct vm_area_struct *vma);

void ptshare_vma_refcount_destroy(struct vm_area_struct *vma);

void ptshare_get_vma(struct vm_area_struct *vma);

void ptshare_put_vma(struct vm_area_struct *vma);
#else /* !CONFIG_PTSHARE */
static inline int ptshare_vma_refcount_init(struct vm_area_struct *vma)
{
	return 0;
}

static inline void ptshare_vma_refcount_destroy(struct vm_area_struct *vma)
{
}

static inline void ptshare_get_vma(struct vm_area_struct *vma)
{
}

static inline void ptshare_put_vma(struct vm_area_struct *vma)
{
}
#endif /* CONFIG_PTSHARE */
#endif /* _LINUX_PTSHARE_VMA_H */