/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PTSHARE_VMA_H
#define _LINUX_PTSHARE_VMA_H

#include <linux/mm_types.h>

struct ptshare_desc;

#ifdef CONFIG_PTSHARE
int ptshare_vma_refcount_init(struct vm_area_struct *vma);
void ptshare_vma_refcount_destroy(struct vm_area_struct *vma);

void ptshare_get_vma(struct vm_area_struct *vma, struct ptshare_desc *desc);
void ptshare_put_vma(struct vm_area_struct *vma, struct ptshare_desc *desc);

int ptshare_unshare_vma_locked(struct vm_area_struct *vma);
int ptshare_unshare_vma_on_gup(struct vm_area_struct *vma, int mmap_locked);
#else /* !CONFIG_PTSHARE */
static inline int ptshare_vma_refcount_init(struct vm_area_struct *vma)
{
	return 0;
}

static inline void ptshare_vma_refcount_destroy(struct vm_area_struct *vma)
{
}

static inline void ptshare_get_vma(struct vm_area_struct *vma,
				  struct ptshare_desc *desc)
{
}

static inline void ptshare_put_vma(struct vm_area_struct *vma,
				  struct ptshare_desc *desc)
{
}

static inline int ptshare_unshare_vma_locked(struct vm_area_struct *vma)
{
	return 0;
}

static inline int ptshare_unshare_vma_on_gup(struct vm_area_struct *vma,
					    int mmap_locked)
{
	return 0;
}
#endif /* CONFIG_PTSHARE */
#endif /* _LINUX_PTSHARE_VMA_H */