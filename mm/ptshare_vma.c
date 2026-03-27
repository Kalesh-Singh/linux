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
	int ret = 0;

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
	ptshare_put_vma(vma);

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
