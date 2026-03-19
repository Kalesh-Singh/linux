// SPDX-License-Identifier: GPL-2.0
/*
 * Shared Page Table Support
 *
 * The ptshare infrastructure provides a way for processes to share
 * page tables (PTE-level) for non-CoWable file-backed mappings.
 * This is achieved by using a headless shadow MM (ptshare_mm) to
 * host the shared page tables.
 *
 * Copyright (c) 2026, Google LLC.
 * Author: Kalesh Singh <kaleshsingh@google.com>
 */
#include <linux/ptshare.h>

#include <linux/file.h>
#include <linux/mm_inline.h>
#include <linux/mmap_lock.h>
#include <linux/slab.h>
#include <linux/sched/mm.h>

static inline void mmap_lock_set_ptshare_class(struct mm_struct *mm)
{
	static struct lock_class_key ptshare_mmap_lock_key;

	lockdep_set_class(&mm->mmap_lock, &ptshare_mmap_lock_key);
}

struct ptshare_desc *ptshare_alloc_desc(void)
{
	struct ptshare_desc *desc;

	desc = kzalloc_obj(struct ptshare_desc);
	if (!desc)
		return NULL;

	desc->ptshare_mm = mm_alloc();
	if (!desc->ptshare_mm) {
		kfree(desc);
		return NULL;
	}

	mmap_lock_set_ptshare_class(desc->ptshare_mm);

	refcount_set(&desc->refcount, 1);

	return desc;
}

/*
 * Once the descriptor's refcount reaches zero, the current thread
 * has exclusive ownership. This occurs either because:
 * 1. The original owning process (the only one capable of creating
 *    new VMAs in this domain) has exited.
 * 2. The last VMA using this domain has been destroyed. Since VMAs
 *    are the only path for other processes to join the domain (e.g.,
 *    via fork inheritance), no new references can be created.
 *
 * Therefore, no further synchronization is needed while we tear down
 * the internal state.
 */
static void ptshare_free_desc(struct ptshare_desc *desc)
{
	mmput(desc->ptshare_mm);
	kfree(desc);
}

void ptshare_put_desc(struct ptshare_desc *desc)
{
	if (!desc)
		return;

	if (refcount_dec_and_test(&desc->refcount))
		ptshare_free_desc(desc);
}

void ptshare_get_desc(struct ptshare_desc *desc)
{
	refcount_inc(&desc->refcount);
}

/*
 * ptshare_validate_mmap - Verify if mmap request is valid in current context.
 *
 * Only support sharing of PTE-level page table pages for file-backed
 * mappings. To ensure consistency and avoid complex CoW scenarios,
 * enforce the following constraints:
 * 1. The mapping must be backed by a file.
 * 2. If writable, it must be MAP_SHARED (to avoid CoW of page tables).
 * 3. MAP_FIXED must be set to ensure precise virtual address control.
 * 4. Both address and length must be PMD-aligned.
 * 5. The requested range must not overlap with any existing mapping in
 *    the ptshare_mm.
 */
int ptshare_validate_mmap(struct file *file, unsigned long addr,
			  unsigned long len, unsigned long prot,
			  unsigned long flags, vm_flags_t vm_flags,
			  unsigned long pgoff)
{
	struct vm_area_struct *vma;
	struct ptshare_desc *desc;
	int ret = 0;

	if (!(vm_flags & VM_PT_SHARED))
		return 0;

	/* Only support file-backed mappings */
	if (unlikely(!file))
		return -EINVAL;

	/*
	 * Writable mappings must be shared; private writable mappings
	 * would require complex CoW logic for the shared page tables
	 * themselves.
	 */
	if (unlikely((prot & PROT_WRITE) && !(flags & MAP_SHARED)))
		return -EINVAL;

	/*
	 * Precise address control is required to guarantee the
	 * mapping covers the entire PMD range.
	 */
	if (unlikely(!(flags & MAP_FIXED)))
		return -EINVAL;

	/* Require PMD alignment of both base and size */
	if (unlikely(!IS_ALIGNED(addr, PMD_SIZE) ||
		     !IS_ALIGNED(len, PMD_SIZE)))
		return -EINVAL;

	desc = current->mm->ptshare_desc;
	if (!desc) {
		/*
		 * If the caller doesn't have a ptshare_desc, allocate it now.
		 */
		desc = ptshare_alloc_desc();
		if (!desc)
			return -ENOMEM;

		current->mm->ptshare_desc = desc;

		/*
		 * Since we were not previously part of any shared page table
		 * domain, we can allow the mapping without further checks.
		 */
		return 0;
	}

	/* The shared MM should never be the caller of this syscall */
	BUG_ON(current->mm == desc->ptshare_mm);

	/*
	 * Check for virtual address space collisions in the shared manager.
	 * Since each shared region must have a unique virtual address across
	 * all participating processes in the same domain.
	 */
	mmap_read_lock(desc->ptshare_mm);
	vma = find_vma_intersection(desc->ptshare_mm, addr, addr + len);
	mmap_read_unlock(desc->ptshare_mm);

	/*
	 * If there is an existing mapping that overlaps with the requested
	 * range, reject the request to prevent conflicts in the shared
	 * page tables.
	 */
	if (vma)
		ret = -EINVAL;

	return ret;
}

static inline void ptshare_remove_vma(struct vm_area_struct *vma,
				      struct ptshare_desc *desc)
{
	struct mm_struct *ptshare_mm = desc->ptshare_mm;

	/* Destroy the shadow VMA in ptshare_mm */
	mmap_write_lock(ptshare_mm);
	BUG_ON(do_munmap(ptshare_mm, vma->vm_start, vma->vm_end - vma->vm_start, NULL));
	mmap_write_unlock(ptshare_mm);
}

void ptshare_get_vma(struct vm_area_struct *vma, struct ptshare_desc *desc)
{
	unsigned long addr = vma->vm_start;
	struct vm_area_struct *shadow_vma;

	ptshare_get_desc(desc);

	mmap_read_lock(desc->ptshare_mm);
	shadow_vma = vma_lookup(desc->ptshare_mm, addr);
	BUG_ON(!shadow_vma);
	refcount_inc(vma_ptshare_refcount(shadow_vma));
	mmap_read_unlock(desc->ptshare_mm);
}

/**
 * ptshare_put_vma - Release a reference to a shared VMA.
 * @vma: The guest VMA.
 * @desc: The shared page table descriptor.
 *
 * This function implements a decoupled locking protocol to safely manage
 * the lifecycle of shared VMAs:
 *
 * 1. Exclusive Ownership: If the reference count drops to zero, the current
 *    thread acquires exclusive responsibility for destroying the shadow VMA.
 * 2. Decoupled Destruction: It is safe to perform the high-latency unmapping
 *    operations using the shadow MM's mmap_lock (see below).
 * 3. Collision Prevention: During the window between refcount drop and
 *    the final do_munmap(), any concurrent attempt to install an overlapping
 *    shared VMA will be rejected by ptshare_validate_mmap() because the
 *    VMA remains in the shadow MM tree until the unmap completes.
 *
 * Note: Since the refcount is atomic, we can potentially optimize this
 * by using per-VMA read locks for lookups and refcount manipulation.
 * This is left for a subsequent optimization.
 */
void ptshare_put_vma(struct vm_area_struct *vma, struct ptshare_desc *desc)
{
	unsigned long addr = vma->vm_start;
	struct vm_area_struct *shadow_vma;
	bool should_remove = false;

	mmap_read_lock(desc->ptshare_mm);
	shadow_vma = vma_lookup(desc->ptshare_mm, addr);
	BUG_ON(!shadow_vma);

	if (refcount_dec_and_test(vma_ptshare_refcount(shadow_vma)))
		should_remove = true;

	mmap_read_unlock(desc->ptshare_mm);

	if (should_remove)
		ptshare_remove_vma(vma, desc);

	ptshare_put_desc(desc);
}

/*
 * Sanitize the shadow VMA.
 *
 * Since we are cloning a guest VMA, we must ensure it is isolated
 * from the guest's reverse mapping, locking structures, and
 * process-specific metadata.
 */
static inline void ptshare_sanitize_vma(struct vm_area_struct *ptshare_vma)
{
	/*
	 * Clear the shared page table flag, this will be used by
	 * rmap walks to determine that ptshare_mm VMAs are candidate
	 * for rmap lookups.
	 */
	vm_flags_clear(ptshare_vma, VM_PT_SHARED);

	ptshare_vma->anon_vma = NULL;
	INIT_LIST_HEAD(&ptshare_vma->anon_vma_chain);

	/* Reset per-VMA lock state for the ptshare_mm context */
	vma_lock_init(ptshare_vma, true);

	/*
	 * TODO: Delete below here once we have a better understanding
	 * of what metadata needs to be sanitized for the shared VMAs.
	 */
#ifdef CONFIG_ANON_VMA_NAME
	/* Sever name and NUMA policy links to the guest MM */
	free_anon_vma_name(ptshare_vma);
	ptshare_vma->anon_name = NULL;
#endif

#ifdef CONFIG_NUMA
	ptshare_vma->vm_policy = NULL;
#endif

#ifdef CONFIG_NUMA_BALANCING
	/* Reset NUMA balancing state */
	vma_numab_state_free(ptshare_vma);
	ptshare_vma->numab_state = NULL;
#endif
}

static inline int __ptshare_install_vma(struct vm_area_struct *vma)
{
	unsigned long len = vma->vm_end - vma->vm_start;
	unsigned long addr = vma->vm_start;
	struct vm_area_struct *ptshare_vma;
	struct ptshare_desc *desc = vma_ptshare_desc(vma);
	struct mm_struct *ptshare_mm = desc->ptshare_mm;
	int ret = 0;

	mmap_assert_write_locked(vma->vm_mm);
	mmap_write_lock(ptshare_mm);

	/* Re-check for overlap while holding the write lock */
	ptshare_vma = find_vma_intersection(ptshare_mm, addr, addr + len);
	if (ptshare_vma) {
		ret = -EINVAL;
		goto unlock;
	}

	ptshare_vma = vm_area_dup(vma);
	if (!ptshare_vma) {
		ret = -ENOMEM;
		goto unlock;
	}

	ptshare_vma->vm_mm = ptshare_mm;

	ptshare_sanitize_vma(ptshare_vma);

	/*
	 * Increment the file reference count and notify any device-specific
	 * VMA open hooks to ensure the shared VMA is properly tracked.
	 */
	if (ptshare_vma->vm_file)
		get_file(ptshare_vma->vm_file);

	if (ptshare_vma->vm_ops && ptshare_vma->vm_ops->open)
		ptshare_vma->vm_ops->open(ptshare_vma);

	refcount_set(vma_ptshare_refcount(ptshare_vma), 1);

	ret = insert_vm_struct(ptshare_mm, ptshare_vma);
	if (ret)
		goto put_file;

	vm_stat_account(ptshare_mm, ptshare_vma->vm_flags, vma_pages(ptshare_vma));
	goto unlock;

put_file:
	if (ptshare_vma->vm_file)
		fput(ptshare_vma->vm_file);

	vm_area_free(ptshare_vma);
unlock:
	mmap_write_unlock(ptshare_mm);

	return ret;
}

unsigned long ptshare_install_vma(struct mm_struct *mm, unsigned long addr)
{
	int install_err, unmap_err;
	struct vm_area_struct *vma;
	unsigned long len;

	if (IS_ERR_VALUE(addr))
		return addr;

	vma = vma_lookup(mm, addr);

	/* This was just installed and the write lock is still held*/
	BUG_ON(!vma);
	mmap_assert_write_locked(vma->vm_mm);

	if (!vma_shares_pagetables(vma))
		return addr;

	/* Initialize VMA's descriptor pointer */
	vma_set_ptshare_desc(vma, mm->ptshare_desc);

	/* mm must refer to the private guest MM*/
	BUG_ON(mm == vma_ptshare_desc(vma)->ptshare_mm);

	install_err = __ptshare_install_vma(vma);
	if (!install_err) {
		ptshare_get_desc(vma_ptshare_desc(vma));
		return addr;
	}

	len = vma->vm_end - vma->vm_start;
	unmap_err = do_munmap(mm, addr, len, NULL);

	if (!unmap_err)
		return install_err;

	/*
	 * If installing the VMA into the shared manager fails, attempt to
	 * unmap the guest VMA to clean up any partial state.
	 *
	 * If the unmap also fails, clear VM_PT_SHARED from the guest VMA
	 * and return success to fail gracefully.
	 *
	 * This leaves the guest VMA in place without shared page tables,
	 * which is a valid albeit non-shared configuration.
	 *
	 * The user can detect this by checking the vm_flags from smaps.
	 */
	vm_flags_clear(vma, VM_PT_SHARED);

	return addr;
}
