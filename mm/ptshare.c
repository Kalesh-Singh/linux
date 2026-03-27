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
#include <linux/hugetlb.h>
#include <linux/mm_inline.h>
#include <linux/mmap_lock.h>
#include <linux/mmu_notifier.h>
#include <linux/pgalloc.h>
#include <linux/rmap.h>
#include <linux/slab.h>
#include <linux/sched/mm.h>

#include <asm/tlbflush.h>
#include <asm-generic/tlb.h>

#include "internal.h"

#ifdef CONFIG_X86
static void ptshare_x86_flush_tlb_range_ipi(void *data)
{
	count_vm_tlb_event(NR_TLB_LOCAL_FLUSH_ALL);

	/*
	 * If INVPCID is available, use it to flush all non-global
	 * mappings across all PCIDs. This avoids unnecessary
	 * flushing of kernel (global) mappings.
	 */
	if (static_cpu_has(X86_FEATURE_INVPCID)) {
		invpcid_flush_all_nonglobals();
	} else {
		/* Fallback to flushing everything if INVPCID is not supported */
		__flush_tlb_all();
	}
}
#else
static void ptshare_x86_flush_tlb_range_ipi(void *data)
{
}
#endif

#ifdef CONFIG_ARM64
static void ptshare_arm64_flush_tlb_range(const struct mmu_notifier_range *range)
{
	unsigned long start = range->start;
	unsigned long end = range->end;
	unsigned long stride = PAGE_SIZE;
	unsigned long pages;

	start = round_down(start, stride);
	end = round_up(end, stride);
	pages = (end - start) >> PAGE_SHIFT;

	if (__flush_tlb_range_limit_excess(start, end, pages, stride)) {
		flush_tlb_all();
		return;
	}

	dsb(ishst);
	__flush_tlb_range_op(vaae1is, start, pages, stride, 0,
			     TLBI_TTL_UNKNOWN, false, lpa2_is_enabled());
	__tlbi_sync_s1ish();
	isb();
}
#else
static void ptshare_arm64_flush_tlb_range(const struct mmu_notifier_range *range)
{
}
#endif

static int ptshare_invalidate_range_start(struct mmu_notifier *mn,
				       const struct mmu_notifier_range *range)
{
	return 0;
}

static void ptshare_invalidate_range_end(struct mmu_notifier *mn,
				       const struct mmu_notifier_range *range)
{
	/*
	 * Global TLB Broadcast (IPI):
	 *
	 * Since ptshare_mm is a "headless" address space, we must broadcast
	 * invalidations to all CPUs to reach guest processes.
	 */
	if (IS_ENABLED(CONFIG_X86))
		on_each_cpu(ptshare_x86_flush_tlb_range_ipi, (void *)range, 1);
	else if (IS_ENABLED(CONFIG_ARM64))
		ptshare_arm64_flush_tlb_range(range);
}

static const struct mmu_notifier_ops ptshare_mmu_notifier_ops = {
	.invalidate_range_start = ptshare_invalidate_range_start,
	.invalidate_range_end = ptshare_invalidate_range_end,
};


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
		goto out_err;

	desc->ptshare_mm = mm_alloc();
	if (!desc->ptshare_mm)
		goto free_desc;

	mmap_lock_set_ptshare_class(desc->ptshare_mm);

	refcount_set(&desc->refcount, 1);

	/* Initialize mmu_notifier */
	desc->mmu_notifier.ops = &ptshare_mmu_notifier_ops;
	if (mmu_notifier_register(&desc->mmu_notifier, desc->ptshare_mm))
		goto free_mm;

	return desc;

free_mm:
	mmput(desc->ptshare_mm);
free_desc:
	kfree(desc);
out_err:
	return NULL;
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
	mmu_notifier_unregister(&desc->mmu_notifier, desc->ptshare_mm);
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

/**
 * ptshare_vma_skip_zap_pte_range - Check if zapping should be skipped.
 * @vma: The VMA being zapped.
 *
 * If this VMA shares page tables, we must NOT call zap_pte_range().
 * zap_pte_range() would unmap all 512 PTEs and drop the page
 * mapcounts, which would maliciously affect all other processes
 * sharing these same page tables.
 *
 * Instead, we skip the PTE-level walk entirely. The PMD entry
 * itself will be cleared later by free_pgtables(), detaching
 * this process from the shared page table without destroying
 * the shared mappings for others.
 */
bool ptshare_vma_skip_zap_pte_range(struct vm_area_struct *vma)
{
	return unlikely(vma_shares_pagetables(vma));
}

/**
 * ptshare_free_pte_range - Safely reclaim shared page tables.
 * @ptdesc: Descriptor for the page table page being freed.
 *
 * During unmapping or process teardoen, we must ensure that shared
 * page tables are not accidentally reclaimed while still in use by
 * other processes.
 *
 * If the page table is shared (refcount > 1), we decrement its reference
 * count and return true to indicate that the caller (guest MM) should NOT
 * free the physical page. The last participant (often the manager MM) will
 * handle the actual reclamation.
 */
bool ptshare_free_pte_range(pgtable_t ptdesc)
{
	struct ptdesc *pt = page_ptdesc(ptdesc);

	if (ptdesc_refcount(pt) > 1) {
		ptdesc_put(pt);
		return true;
	}

	return false;
}

vm_fault_t ptshare_handle_mm_fault(struct vm_fault *vmf)
{
	struct ptshare_desc *desc = vma_ptshare_desc(vmf->vma);
	struct mm_struct *ptshare_mm = desc->ptshare_mm;
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
		ptshare_vma = lock_mm_and_find_vma(ptshare_mm, vmf->address,
							NULL);
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

/**
 * ptshare_unshare_vma_locked - Fully sever a VMA from ptshare infrastructure.
 * @vma: The guest VMA.
 * @locked: indicates whether the mmap read lock is already held (from GUP).
 *
 * This function iterates through all PMDs in the VMA, allocates private
 * page tables for the faulting process, copies the shared PTEs into them,
 * and increments the page reference and mapcounts so the new private mapping
 * is correctly tracked by rmap.
 *
 * Context: Caller MUST hold the mmap_write_lock on vma->vm_mm.
 *
 * Return: 0 on success, or -errno. If successful, the VMA is no longer shared.
 */
int ptshare_unshare_vma_locked(struct vm_area_struct *vma)
{
	struct mm_struct *mm = vma->vm_mm;
	unsigned long addr;

	mmap_assert_write_locked(mm);

	if (!vma_shares_pagetables(vma))
		return 0;

	for (addr = vma->vm_start; addr < vma->vm_end; addr += PMD_SIZE) {
		pgd_t *pgd;
		p4d_t *p4d;
		pud_t *pud;
		pmd_t *pmd;
		pmd_t pmdval;
		spinlock_t *ptl;
		struct ptdesc *new_ptdesc, *old_ptdesc;
		pte_t *old_ptep, *new_ptep;
		int i;

		pgd = pgd_offset(mm, addr);
		if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd)))
			continue;
		p4d = p4d_offset(pgd, addr);
		if (p4d_none(*p4d) || unlikely(p4d_bad(*p4d)))
			continue;
		pud = pud_offset(p4d, addr);
		if (pud_none(*pud) || unlikely(pud_bad(*pud)))
			continue;
		pmd = pmd_offset(pud, addr);
		pmdval = pmdp_get_lockless(pmd);

		/* Skip if the PMD is empty, huge, or not present */
		if (pmd_none(pmdval) || !pmd_present(pmdval) || pmd_trans_huge(pmdval))
			continue;

		/* Allocate the new private page table */
		new_ptdesc = page_ptdesc(pte_alloc_one(mm));
		if (!new_ptdesc)
			return -ENOMEM;

		ptl = pmd_lock(mm, pmd);
		if (unlikely(!pmd_same(*pmd, pmdval))) {
			/* PMD changed under us, abort and retry */
			spin_unlock(ptl);
			pte_free(mm, ptdesc_page(new_ptdesc));
			continue;
		}

		old_ptdesc = page_ptdesc(pmd_page(*pmd));

		old_ptep = pte_offset_map(pmd, addr);
		new_ptep = (pte_t *)page_address(ptdesc_page(new_ptdesc));

		/* Copy PTEs and elevate refcounts so rmap sees the new mapping */
		for (i = 0; i < PTRS_PER_PTE; i++) {
			pte_t pte = ptep_get(old_ptep + i);

			if (pte_present(pte)) {
				struct page *page = pte_page(pte);
				struct folio *folio = page_folio(page);

				folio_get(folio);
				folio_add_file_rmap_pte(folio, page, vma);
			}

			set_pte_at(mm, addr + i * PAGE_SIZE, new_ptep + i, pte);
		}

		/* Atomically detach the old shared page table from the PMD */
		pmd_clear(pmd);
		/* Install the newly populated private page table */
		pmd_populate(mm, pmd, ptdesc_page(new_ptdesc));

		spin_unlock(ptl);
		pte_unmap(old_ptep);

		/*
		 * Synchronization Protocol (Clear-then-Sync):
		 * To safely synchronize with gup_fast(), we must ensure that no
		 * CPU is locklessly walking the old shared page tables before
		 * we drop the reference:
		 *
		 * 1. Detach: By calling pmd_populate() above, we have already
		 *    replaced the entry. New walkers will now only see the
		 *    new private table.
		 * 2. Synchronize: We now call tlb_remove_table_sync_one() to
		 *    broadcast an IPI. Any CPU executing gup_fast() cannot
		 *    acknowledge the IPI until it re-enables interrupts,
		 *    guaranteeing it has finished its walk if it already
		 *    dereferenced the old PMD.
		 * 3. Free/Put: Now it is safe to drop the reference to the
		 *    old shared page table.
		 */
		tlb_remove_table_sync_one();

		/* Drop the reference we held from the initial PMD splice */
		ptdesc_put(old_ptdesc);
	}

	/* Drop the reference to the manager's shadow VMA */
	ptshare_put_vma(vma, vma_ptshare_desc(vma));

	/*
	 * Transition the VMA to a normal private mapping.
	 *
	 * This must be the last step after the ptshare_put_vma() above.
	 */
	vm_flags_clear(vma, VM_PT_SHARED);

	return 0;
}

/**
 * ptshare_unshare_vma - Sever a VMA from shared page tables (handles GUP lock upgrade).
 * @vma: The guest VMA.
 * @mmap_locked: Pointer to the mmap_lock status (from GUP).
 *
 * Context: Called from __get_user_pages() when FOLL_WRITE is requested on a
 * shared VMA. It handles the transition from mmap_read_lock to mmap_write_lock.
 *
 * Return: 0 on success, or -errno.
 */
int ptshare_unshare_vma_on_gup(struct vm_area_struct *vma, int mmap_locked)
{
	struct mm_struct *mm = vma->vm_mm;
	int ret = 0;

	if (!vma_shares_pagetables(vma))
		return 0;

	if (mmap_locked)
		mmap_read_unlock(mm);

	ret = mmap_write_lock_killable(mm);
	if (ret)
		goto out;

	mmap_assert_write_locked(mm);

	ret = ptshare_unshare_vma_locked(vma);

	mmap_write_unlock(mm);
out:
	/* Restore the read lock for the caller */
	if (mmap_locked)
		mmap_read_lock(mm);

	return ret;
}
