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

static inline void ptshare_remove_vma(struct vm_area_struct *vma)
{
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

/**
 * ptshare_put_vma - Release a reference to a shared VMA.
 * @vma: The guest VMA.
 *
 * This function implements a decoupled locking protocol to safely manage
 * the lifecycle of shared VMAs:
 *
 * 1. Atomic Synchronization: We use the XArray's internal spinlock (xa_lock)
 *    to protect the reference count.
 * 2. Exclusive Ownership: If the reference count drops to zero under the
 *    xa_lock, the current thread acquires exclusive responsibility for
 *    destroying the shadow VMA.
 * 3. Preventing Races: By calling ptshare_vma_refcount_destroy_locked()
 *    (xa_erase) while still holding the spinlock, we ensure that any
 *    concurrent thread attempting to look up the VMA to increment its
 *    refs will fail.
 * 4. decoupled Destruction: Once the VMA is erased from the XArray, it is
 *    mathematically impossible for another thread to find it and resurrect
 *    the refcount. Therefore, it is safe to drop the spinlock (avoiding
 *    atomic vs. sleeping lock violations) and proceed to perform the
 *    high-latency unmapping operations (which may sleep) using the
 *    ptshare_mm's mmap_lock.
 * 5. Collision Prevention: During the window between XArray erasure and
 *    the final do_munmap(ptshare_mm), any concurrent attempt to install
 *    an overlapping shared VMA will be rejected by ptshare_validate_mmap()
 *    because the VMA remains in the ptshare_mm tree until the unmap
 *    operation completes.
 */
void ptshare_put_vma(struct vm_area_struct *vma)
{
	int refs;
	bool should_remove = false;

	ptshare_vma_refcount_lock();

	refs = ptshare_vma_refcount_dec_locked(vma);
	BUG_ON(refs < 0);

	if (!refs) {
		ptshare_vma_refcount_destroy_locked(vma);
		should_remove = true;
	}

	ptshare_vma_refcount_unlock();

	if (should_remove)
		ptshare_remove_vma(vma);
}
