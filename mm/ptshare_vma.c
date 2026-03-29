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
#include <linux/pgalloc.h>
#include <linux/hugetlb.h>
#include <linux/xarray.h>
#include <linux/rmap.h>

#include <asm-generic/tlb.h>
#include <asm/tlbflush.h>

static inline void ptshare_remove_vma(struct vm_area_struct *vma)
{
	struct ptshare_desc *desc = vma->vm_ptshare;
	struct mm_struct *ptshare_mm = desc->ptshare_mm;

	/* Destroy the shadow VMA in ptshare_mm */
	mmap_write_lock_nested(ptshare_mm, SINGLE_DEPTH_NESTING);
	BUG_ON(do_munmap(ptshare_mm, vma->vm_start, vma->vm_end - vma->vm_start, NULL));
	mmap_write_unlock(ptshare_mm);
}

int ptshare_vma_refcount_init(struct vm_area_struct *vma)
{
	refcount_set(vma_ptshare_refcount(vma), 1);
	return 0;
}

void ptshare_vma_refcount_destroy(struct vm_area_struct *vma)
{
}

void ptshare_get_vma(struct vm_area_struct *vma, struct ptshare_desc *desc)
{
	unsigned long addr = vma->vm_start;
	struct vm_area_struct *shadow_vma;

	mmap_read_lock_nested(desc->ptshare_mm, SINGLE_DEPTH_NESTING);
	shadow_vma = vma_lookup(desc->ptshare_mm, addr);
	BUG_ON(!shadow_vma);
	refcount_inc(vma_ptshare_refcount(shadow_vma));
	mmap_read_unlock(desc->ptshare_mm);
}

/**
 * ptshare_put_vma - Release a reference to a shared VMA.
 * @vma: The guest VMA.
 * @desc: The sharing domain descriptor.
 *
 * This function implements a decoupled locking protocol to safely manage
 * the lifecycle of shared VMAs:
 *
 * 1. Exclusive Ownership: If the reference count drops to zero, the current
 *    thread acquires exclusive responsibility for destroying the shadow VMA.
 * 2. decouple Destruction: It is safe to perform the high-latency unmapping
 *    operations (which may sleep) using the ptshare_mm's mmap_lock.
 * 3. Collision Prevention: During the window between refcount drop and
 *    the final do_munmap(ptshare_mm), any concurrent attempt to install
 *    an overlapping shared VMA will be rejected by ptshare_validate_mmap()
 *    because the VMA remains in the ptshare_mm tree until the unmap
 *    operation completes.
 */
void ptshare_put_vma(struct vm_area_struct *vma, struct ptshare_desc *desc)
{
	unsigned long addr = vma->vm_start;
	struct vm_area_struct *shadow_vma;
	bool should_remove = false;

	mmap_read_lock_nested(desc->ptshare_mm, SINGLE_DEPTH_NESTING);
	shadow_vma = vma_lookup(desc->ptshare_mm, addr);
	BUG_ON(!shadow_vma);

	if (refcount_dec_and_test(vma_ptshare_refcount(shadow_vma)))
		should_remove = true;

	mmap_read_unlock(desc->ptshare_mm);

	if (should_remove)
		ptshare_remove_vma(vma);
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
	struct ptshare_desc *desc = vma->vm_ptshare;
	unsigned long addr;
	int ret = 0;

	mmap_assert_write_locked(mm);

	if (!vma_shares_pagetables(vma))
		return 0;

	BUG_ON(!desc);

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
		if (!new_ptdesc) {
			ret = -ENOMEM;
			return ret;
		}

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

	/*
	 * Transition the VMA to a normal private mapping.
	 */
	vm_flags_clear(vma, VM_PT_SHARED);

	/* Drop the reference to the manager's shadow VMA */
	ptshare_put_vma(vma, desc);

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
