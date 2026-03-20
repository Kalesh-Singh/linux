// SPDX-License-Identifier: GPL-2.0
/*
 * Shared Page Table Support
 *
 * Copyright (c) 2026, Google LLC.
 * Author: Kalesh Singh <kaleshsingh@google.com>
 */
#include <linux/ptshare.h>
#include <linux/ptshare_vma.h>

#include <linux/init.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/mman.h>
#include <linux/sched/mm.h>
#include <linux/xarray.h>

#include "internal.h"

struct mm_struct *ptshare_mm;

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

	/* The global shared MM should never be the caller of this syscall */
	BUG_ON(current->mm == ptshare_mm);

	/*
	 * Check for virtual address space collisions in the shared manager.
	 * Since ptshare_mm is a global repository, each shared region
	 * must have a unique virtual address across all participating
	 * processes.
	 */
	mmap_read_lock_nested(ptshare_mm, SINGLE_DEPTH_NESTING);
	vma = find_vma_intersection(ptshare_mm, addr, addr + len);
	mmap_read_unlock(ptshare_mm);

	/*
	 * If there is an existing mapping that overlaps with the requested
	 * range, reject the request to prevent conflicts in the shared
	 * page tables.
	 */
	if (vma)
		ret = -EINVAL;

	return ret;
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

	/* Sever name and NUMA policy links to the guest MM */
	free_anon_vma_name(ptshare_vma);
#ifdef CONFIG_ANON_VMA_NAME
	ptshare_vma->anon_name = NULL;
#endif

#ifdef CONFIG_NUMA
	ptshare_vma->vm_policy = NULL;
#endif

	/* Reset NUMA balancing state */
	vma_numab_state_free(ptshare_vma);
#ifdef CONFIG_NUMA_BALANCING
	ptshare_vma->numab_state = NULL;
#endif

	/* Reset per-VMA lock state for the ptshare_mm context */
	vma_lock_init(ptshare_vma, true);
}

static inline int __ptshare_install_vma(struct vm_area_struct *vma)
{
	unsigned long len = vma->vm_end - vma->vm_start;
	unsigned long addr = vma->vm_start;
	struct vm_area_struct *ptshare_vma;
	int ret = 0;

	mmap_assert_write_locked(vma->vm_mm);
	mmap_write_lock_nested(ptshare_mm, SINGLE_DEPTH_NESTING);

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

	ret = ptshare_vma_refcount_init(ptshare_vma);
	if (ret)
		goto put_file;

	ret = insert_vm_struct(ptshare_mm, ptshare_vma);
	if (ret)
		goto destroy_vma_refcount;

	vm_stat_account(ptshare_mm, ptshare_vma->vm_flags, vma_pages(ptshare_vma));
	goto unlock;

destroy_vma_refcount:
	ptshare_vma_refcount_destroy(ptshare_vma);
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

	/* mm must refer to the private guest MM*/
	BUG_ON(mm == ptshare_mm);

	install_err = __ptshare_install_vma(vma);
	if (!install_err)
		return addr;

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

vm_fault_t ptshare_handle_mm_fault(struct vm_fault *vmf)
{
	unsigned int flags = vmf->flags	& ~FAULT_FLAG_VMA_LOCK;
	struct vm_fault ptshare_vmf = *vmf;
	struct vm_area_struct *ptshare_vma;
	struct ptdesc *ptdesc;
	vm_fault_t ret;
	pgd_t *pgd;
	p4d_t *p4d;

	assert_fault_locked(vmf);

	/*
	 * Optimistic Per-VMA Lock
	 *
	 * Attempt to acquire the VMA lock for the shared manager address space.
	 * This is the fast path that avoids the global mmap_lock.
	 */
	ptshare_vma = lock_vma_under_rcu(ptshare_mm, vmf->address);
	if (!ptshare_vma) {
		/*
		 * Fallback to mmap_lock
		 *
		 * Use the nested helper to safely look up and lock the shared VMA
		 * while already holding the faulting process's lock.
		 */
		ptshare_vma = lock_mm_and_find_vma_nested(ptshare_mm, vmf->address,
							NULL, SINGLE_DEPTH_NESTING);
		if (!ptshare_vma)
			return VM_FAULT_SIGSEGV;
	} else {
		flags |= FAULT_FLAG_VMA_LOCK;
	}

	ptshare_vmf.flags = flags;
	ptshare_vmf.vma = ptshare_vma;
	ptshare_vmf.prealloc_pte = NULL;
	ptshare_vmf.real_address = vmf->real_address;

	assert_fault_locked(&ptshare_vmf);

	pgd = pgd_offset(ptshare_mm, vmf->address);
	p4d = p4d_alloc(ptshare_mm, pgd, vmf->address);
	if (!p4d) {
		ret = VM_FAULT_OOM;
		goto out;
	}

	ptshare_vmf.pud = pud_alloc(ptshare_mm, p4d, vmf->address);
	if (!ptshare_vmf.pud) {
		ret = VM_FAULT_OOM;
		goto out;
	}

	ptshare_vmf.pmd = pmd_alloc(ptshare_mm, ptshare_vmf.pud, vmf->address);
	if (!ptshare_vmf.pmd) {
		ret = VM_FAULT_OOM;
		goto out;
	}

	ret = handle_pte_fault(&ptshare_vmf);

	/*
	 * PMD Splicing:
	 * The ptshare_mm now has a populated PTE page table.
	 * We "splice" this shared PTE page table directly into the
	 * faulting process's PMD.
	 *
	 * Safety Analysis:
	 * 1. Structural Integrity: It is safe to access vmf->pmd here because
	 *    the caller (arch fault handler) holds the guest MM's read-side
	 *    lock (either per-VMA or mmap_lock). Any operation that would
	 *    free the PMD page (munmap, exit_mmap) requires the mmap_write_lock.
	 *    Even if a writer acquires the mmap_write_lock, they are blocked
	 *    from actually modifying or detaching this VMA (via vma_start_write)
	 *    until all per-VMA readers have finished.
	 * 2. Atomic Population: We acquire the guest's PMD spinlock only for
	 *    the actual splice. This avoids holding the spinlock while
	 *    performing potentially sleeping operations in the manager MM.
	 * 3. Race Prevention: The double-check (pmd_none) inside the lock
	 *    ensures that if multiple threads fault on the same PMD, only
	 *    one performs the splicing and refcount increment.
	 */
	if (!(ret & (VM_FAULT_ERROR | VM_FAULT_RETRY)) && !pmd_none(*ptshare_vmf.pmd)) {
		spinlock_t *ptl = pmd_lock(vmf->vma->vm_mm, vmf->pmd);

		if (pmd_none(*vmf->pmd)) {
			ptdesc = page_ptdesc(pmd_page(*ptshare_vmf.pmd));
			ptdesc_get(ptdesc);
			set_pmd_at(vmf->vma->vm_mm, vmf->address, vmf->pmd, *ptshare_vmf.pmd);
			mm_inc_nr_ptes(vmf->vma->vm_mm);
		}

		spin_unlock(ptl);
	}

out:
	/*
	 * If handle_pte_fault() returned RETRY or COMPLETED, it already
	 * dropped the manager lock; we must also drop the guest lock to
	 * follow the fault handler contract.
	 *
	 * Otherwise, we release the manager lock we acquired and let the
	 * caller release the guest lock.
	 */
	if (ret & (VM_FAULT_RETRY | VM_FAULT_COMPLETED))
		release_fault_lock(vmf);
	else
		release_fault_lock(&ptshare_vmf);

	return ret;
}

static int __init ptshare_init(void)
{
	ptshare_mm = mm_alloc();
	if (!ptshare_mm)
		return -ENOMEM;

	return 0;
}
core_initcall(ptshare_init);
