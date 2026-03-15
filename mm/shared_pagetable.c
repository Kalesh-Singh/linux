// SPDX-License-Identifier: GPL-2.0
#include <linux/mm_types.h>
#include <linux/maple_tree.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/cpumask.h>
#include <linux/mman.h>
#include <linux/pgtable.h>
#include <linux/atomic.h>
#include <linux/user_namespace.h>
#include <asm/mmu.h>
#include <linux/mm.h>
#include <linux/shared_pagetable.h>
#include <linux/mmu_notifier.h>
#include <linux/smp.h>
#include "internal.h"
#include <linux/xarray.h>
#include <linux/pgalloc.h>
#include <linux/hugetlb.h>
#include <asm-generic/tlb.h>

#if defined(CONFIG_X86) || defined(CONFIG_ARM64)
#include <asm/tlbflush.h>
#endif

#ifndef INIT_MM_CONTEXT
#define INIT_MM_CONTEXT(name)
#endif

static DEFINE_XARRAY(shpt_refcounts);

struct mm_struct *ptshare_mm;

/*
 * We only allow sharing of page tables for file-backed mappings.
 * If the mapping is writable, it must be MAP_SHARED to avoid
 * having to deal with Copy-on-Write (CoW) of the shared page tables
 * when the underlying pages are CoW'ed.
 *
 * To simplify the implementation, we only allow sharing of
 * whole page tables (PMD-level sharing), so the mapping must
 * be PMD-aligned and a multiple of PMD_SIZE. This also requires
 * MAP_FIXED to ensure the address is exactly what the user
 * requested.
 */
int shpt_validate_mmap(struct file *file, unsigned long addr, unsigned long len,
		       unsigned long prot, unsigned long flags,
		       vm_flags_t vm_flags, unsigned long pgoff)
{
	struct vm_area_struct *vma;
	int ret = 0;

	if (!(vm_flags & VM_SHARED_PT))
		return 0;

	if (unlikely(!file))
		return -EINVAL;

	if (unlikely((prot & PROT_WRITE) && !(flags & MAP_SHARED)))
		return -EINVAL;

	if (unlikely(!(flags & MAP_FIXED)))
		return -EINVAL;

	if (unlikely(!IS_ALIGNED(addr, PMD_SIZE) ||
		     !IS_ALIGNED(len, PMD_SIZE)))
		return -EINVAL;

	BUG_ON(current->mm == ptshare_mm);

	mmap_read_lock_nested(ptshare_mm, SINGLE_DEPTH_NESTING);
	vma = find_vma_intersection(ptshare_mm, addr, addr + len);
	if (vma)
		ret = -EINVAL;
	mmap_read_unlock(ptshare_mm);

	return ret;
}

static void shpt_vma_init_refcount(struct vm_area_struct *vma);

int shpt_install_vma(struct mm_struct *mm, unsigned long addr,
		      unsigned long len, vm_flags_t vm_flags)
{
	struct vm_area_struct *vma, *new_vma;
	int ret;

	if (IS_ERR_VALUE(addr))
		return 0;

	if (!(vm_flags & VM_SHARED_PT))
		return 0;

	BUG_ON(mm == ptshare_mm);

	vma = vma_lookup(mm, addr);
	BUG_ON(!vma);

	if (!mm->shpt_mm)
		mm->shpt_mm = ptshare_mm;

	new_vma = vm_area_dup(vma);
	if (!new_vma)
		return -ENOMEM;

	new_vma->vm_mm = ptshare_mm;
	vm_flags_clear(new_vma, VM_SHARED_PT);

	mmap_write_lock_nested(ptshare_mm, SINGLE_DEPTH_NESTING);
	ret = insert_vm_struct(ptshare_mm, new_vma);
	if (ret) {
		mmap_write_unlock(ptshare_mm);
		vm_area_free(new_vma);
		return ret;
	}
	vm_stat_account(ptshare_mm, new_vma->vm_flags, vma_pages(new_vma));
	mmap_write_unlock(ptshare_mm);

	shpt_vma_init_refcount(new_vma);

	return 0;
}

static void shpt_vma_init_refcount(struct vm_area_struct *vma)
{
	xa_store(&shpt_refcounts, vma->vm_start, (void *)1, GFP_KERNEL);
}

void shpt_vma_get(struct vm_area_struct *vma)
{
	unsigned long ref;

	xa_lock(&shpt_refcounts);
	ref = (unsigned long)xa_load(&shpt_refcounts, vma->vm_start);
	__xa_store(&shpt_refcounts, vma->vm_start, (void *)(ref + 1), GFP_ATOMIC);
	xa_unlock(&shpt_refcounts);
}

void shpt_vma_put(struct vm_area_struct *vma)
{
	unsigned long ref;

	xa_lock(&shpt_refcounts);
	ref = (unsigned long)xa_load(&shpt_refcounts, vma->vm_start);
	ref--;
	if (ref == 0) {
		__xa_erase(&shpt_refcounts, vma->vm_start);
		xa_unlock(&shpt_refcounts);
		/* Schedule destruction of the shadow VMA in ptshare_mm */
		mmap_write_lock_nested(ptshare_mm, SINGLE_DEPTH_NESTING);
		do_munmap(ptshare_mm, vma->vm_start, vma->vm_end - vma->vm_start, NULL);
		mmap_write_unlock(ptshare_mm);
	} else {
		__xa_store(&shpt_refcounts, vma->vm_start, (void *)ref, GFP_ATOMIC);
		xa_unlock(&shpt_refcounts);
	}
}

/*
 * shpt_unshare_vma - Fully sever a VMA from the shared page table infrastructure.
 *
 * This function iterates through all PMDs in the VMA, allocates private
 * page tables for the faulting process, copies the shared PTEs into them,
 * and increments the page reference and mapcounts so the new private mapping
 * is correctly tracked by rmap.
 *
 * It must be called with the mm->mmap_lock held for writing, as it modifies
 * vma->vm_flags and safely manipulates page table structures.
 */
int shpt_unshare_vma(struct vm_area_struct *vma)
{
	struct mm_struct *mm = vma->vm_mm;
	unsigned long addr;

	if (!vma_shares_pagetable(vma))
		return 0;

	/* We must be in a write-locked context to modify vma->vm_flags */
	mmap_assert_write_locked(mm);

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

				folio_get(page_folio(page));
				folio_add_file_rmap_pte(page_folio(page), page, vma);
			}

			set_pte_at(mm, addr + i * PAGE_SIZE, new_ptep + i, pte);
		}

		/* Atomically detach the old shared page table from the PMD */
		pmd_clear(pmd);
		/* Install the newly populated private page table */
		pmd_populate(mm, pmd, ptdesc_page(new_ptdesc));

		spin_unlock(ptl);
		pte_unmap(old_ptep);

		/* Drop the reference we held from the initial PMD splice */
		ptdesc_put(old_ptdesc);
	}

	/*
	 * Explicit IPI broadcast synchronization.
	 *
	 * Because gup_fast() locklessly walks page tables with local
	 * interrupts disabled, we cannot safely consider the unshare complete
	 * until we guarantee no CPU is currently walking the old shared page
	 * tables through this process's page directory.
	 *
	 * tlb_remove_table_sync_one() broadcasts an IPI. Any CPU executing
	 * gup_fast() cannot acknowledge the IPI until it re-enables interrupts,
	 * meaning it has finished its lockless walk.
	 */
	tlb_remove_table_sync_one();

	/*
	 * Remove the VM_SHARED_PT flag. From this point forward, the VMA is
	 * a normal private file-backed mapping, and rmap will no longer skip it.
	 */
	vm_flags_clear(vma, VM_SHARED_PT);

	/* Drop our reference to the ptshare_mm shadow VMA */
	shpt_vma_put(vma);

	return 0;
}

#ifdef CONFIG_X86
static void shpt_flush_tlb_ipi(void *data)
{
	count_vm_tlb_event(NR_TLB_LOCAL_FLUSH_ALL);
	__flush_tlb_all();
}
#endif

#ifdef CONFIG_ARM64
static void shpt_flush_tlb_arm64(const struct mmu_notifier_range *range)
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
#endif

static int shpt_invalidate_range_start(struct mmu_notifier *mn,
				       const struct mmu_notifier_range *range)
{
	if (!mmu_notifier_range_blockable(range))
		return -EAGAIN;

#ifdef CONFIG_X86
	on_each_cpu(shpt_flush_tlb_ipi, (void *)range, 1);
#elif defined(CONFIG_ARM64)
	shpt_flush_tlb_arm64(range);
#endif

	return 0;
}

static const struct mmu_notifier_ops shpt_mmu_notifier_ops = {
	.invalidate_range_start = shpt_invalidate_range_start,
};

static struct mmu_notifier shpt_mmu_notifier = {
	.ops = &shpt_mmu_notifier_ops,
};

static int __init shpt_init(void)
{
	ptshare_mm = mm_alloc();
	if (!ptshare_mm)
		return -ENOMEM;

	return mmu_notifier_register(&shpt_mmu_notifier, ptshare_mm);
}
core_initcall(shpt_init);

vm_fault_t shpt_handle_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *ptshare_vma;
	struct vm_fault ptshare_vmf = *vmf;
	vm_fault_t ret;
	pgd_t *pgd;
	p4d_t *p4d;
	spinlock_t *ptl;
	struct ptdesc *ptdesc;

	ptshare_vma = lock_vma_under_rcu(ptshare_mm, vmf->address);
	if (!ptshare_vma) {
		/*
		 * If we are under VMA lock, we don't want to wait for
		 * the shared MM's mmap_lock. Return RETRY so the
		 * architecture can fall back to mmap_lock.
		 */
		if (vmf->flags & FAULT_FLAG_VMA_LOCK) {
			release_fault_lock(vmf);
			return VM_FAULT_RETRY;
		}

		if (mmap_read_lock_killable_nested(ptshare_mm, SINGLE_DEPTH_NESTING)) {
			release_fault_lock(vmf);
			return VM_FAULT_RETRY;
		}

		ptshare_vma = vma_lookup(ptshare_mm, vmf->address);
		if (!ptshare_vma) {
			mmap_read_unlock(ptshare_mm);
			return VM_FAULT_SIGSEGV;
		}
		ptshare_vmf.flags &= ~FAULT_FLAG_VMA_LOCK;
	} else {
		ptshare_vmf.flags |= FAULT_FLAG_VMA_LOCK;
	}

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

	ptshare_vmf.vma = ptshare_vma;
	ptshare_vmf.prealloc_pte = NULL;

	ret = handle_pte_fault(&ptshare_vmf);

	if (ret & VM_FAULT_RETRY) {
		/*
		 * handle_pte_fault has dropped the ptshare_mm lock.
		 * Now we must drop the faulting process's lock.
		 */
		release_fault_lock(vmf);
	} else {
		if (!(ret & VM_FAULT_ERROR) && !pmd_none(*ptshare_vmf.pmd)) {
			/*
			 * PMD Splicing:
			 * The ptshare_mm now has a populated PTE page table.
			 * We "splice" this shared PTE page table directly into the
			 * faulting process's PMD. We increment the ptdesc refcount
			 * so the shared page table is not freed prematurely when a
			 * single sharing process unmaps the region.
			 */
			ptl = pmd_lock(vmf->vma->vm_mm, vmf->pmd);
			if (pmd_none(*vmf->pmd)) {
				ptdesc = page_ptdesc(pmd_page(*ptshare_vmf.pmd));
				ptdesc_get(ptdesc);
				set_pmd_at(vmf->vma->vm_mm, vmf->address, vmf->pmd, *ptshare_vmf.pmd);
				mm_inc_nr_ptes(vmf->vma->vm_mm);
			}
			spin_unlock(ptl);
		}

		/* Normal completion, release the ptshare_mm lock we acquired */
		if (ptshare_vmf.flags & FAULT_FLAG_VMA_LOCK)
			vma_end_read(ptshare_vma);
		else
			mmap_read_unlock(ptshare_mm);
	}

	return ret;

out:
	if (ptshare_vmf.flags & FAULT_FLAG_VMA_LOCK)
		vma_end_read(ptshare_vma);
	else
		mmap_read_unlock(ptshare_mm);
	return ret;
}
