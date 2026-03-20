// SPDX-License-Identifier: GPL-2.0
/*
 * VMA Management for Shared Page Table Support
 *
 * Copyright (c) 2026, Google LLC.
 * Author: Kalesh Singh <kaleshsingh@google.com>
 */

#include <linux/ptshare_vma.h>
#include <linux/ptshare.h>

#include <linux/mmap_lock.h>
#include <linux/xarray.h>

static DEFINE_XARRAY(ptshare_vma_refs);

int ptshare_vma_refcount_init(struct vm_area_struct *vma)
{
	return xa_err(xa_store(&ptshare_vma_refs, vma->vm_start, xa_mk_value(1), GFP_KERNEL));
}

void ptshare_vma_refcount_destroy(struct vm_area_struct *vma)
{
	xa_erase(&ptshare_vma_refs, vma->vm_start);
}

static inline void ptshare_vma_refcount_destroy_locked(struct vm_area_struct *vma)
{
	__xa_erase(&ptshare_vma_refs, vma->vm_start);
}

static inline void ptshare_vma_refcount_lock(void)
{
	xa_lock(&ptshare_vma_refs);
}

static inline void ptshare_vma_refcount_unlock(void)
{
	xa_unlock(&ptshare_vma_refs);
}

static inline int ptshare_vma_get_refcount_locked(struct vm_area_struct *vma)
{
	void *entry = xa_load(&ptshare_vma_refs, vma->vm_start);

	if (!entry)
		return -EINVAL;

	return xa_to_value(entry);
}

static inline int ptshare_vma_set_refcount_locked(struct vm_area_struct *vma, int count)
{
	void *ret = __xa_store(&ptshare_vma_refs, vma->vm_start, xa_mk_value(count), GFP_ATOMIC);

	if (xa_err(ret))
		return xa_err(ret);

	return count;
}

static inline int ptshare_vma_refcount_add_locked(struct vm_area_struct *vma,
						int count)
{
	int refs = ptshare_vma_get_refcount_locked(vma);

	if (refs < 0)
		return refs;

	refs += count;

	return ptshare_vma_set_refcount_locked(vma, refs);
}

static inline int ptshare_vma_refcount_add(struct vm_area_struct *vma, int count)
{
	int ret;

	ptshare_vma_refcount_lock();
	ret = ptshare_vma_refcount_add_locked(vma, count);
	ptshare_vma_refcount_unlock();

	return ret;
}

static inline int ptshare_vma_refcount_dec_locked(struct vm_area_struct *vma)
{
	return ptshare_vma_refcount_add_locked(vma, -1);
}

static inline int ptshare_vma_refcount_inc(struct vm_area_struct *vma)
{
	return ptshare_vma_refcount_add(vma, 1);
}

static inline void ptshare_remove_vma_locked(struct vm_area_struct *vma)
{
	ptshare_vma_refcount_destroy_locked(vma);

	/* Destroy the shadow VMA in ptshare_mm */
	mmap_write_lock_nested(ptshare_mm, SINGLE_DEPTH_NESTING);
	BUG_ON(do_munmap(ptshare_mm, vma->vm_start, vma->vm_end - vma->vm_start, NULL));
	mmap_write_unlock(ptshare_mm);
}

void ptshare_get_vma(struct vm_area_struct *vma)
{
	int ret = ptshare_vma_refcount_inc(vma);

	BUG_ON(ret < 0);
}

void ptshare_put_vma(struct vm_area_struct *vma)
{
	int refs;

	ptshare_vma_refcount_lock();

	refs = ptshare_vma_refcount_dec_locked(vma);
	BUG_ON(refs < 0);

	if (!refs)
		ptshare_remove_vma_locked(vma);

	ptshare_vma_refcount_unlock();
}
