// SPDX-License-Identifier: GPL-2.0
/*
 * Shared Page Table Support
 *
 * Copyright (c) 2026, Google LLC.
 * Author: Kalesh Singh <kaleshsingh@google.com>
 */
#include "linux/kconfig.h"
#include <linux/ptshare.h>
#include <linux/ptshare_vma.h>

#include <linux/init.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/mman.h>
#include <linux/mmu_notifier.h>
#include <linux/sched/mm.h>
#include <linux/smp.h>
#include <linux/xarray.h>

#include <asm/tlbflush.h>

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
	if (unlikely(!file)) {
		ptshare_mm_err(current->mm, "ptshare_validate_mmap: file is NULL\n");
		return -EINVAL;
	}

	/*
	 * Writable mappings must be shared; private writable mappings
	 * would require complex CoW logic for the shared page tables
	 * themselves.
	 */
	if (unlikely((prot & PROT_WRITE) && !(flags & MAP_SHARED))) {
		ptshare_mm_err(current->mm, "ptshare_validate_mmap: writable mapping must be MAP_SHARED\n");
		return -EINVAL;
	}

	/*
	 * Precise address control is required to guarantee the
	 * mapping covers the entire PMD range.
	 */
	if (unlikely(!(flags & MAP_FIXED))) {
		ptshare_mm_err(current->mm, "ptshare_validate_mmap: MAP_FIXED not set\n");
		return -EINVAL;
	}

	/* Require PMD alignment of both base and size */
	if (unlikely(!IS_ALIGNED(addr, PMD_SIZE) ||
		     !IS_ALIGNED(len, PMD_SIZE))) {
		ptshare_mm_err(current->mm, "ptshare_validate_mmap: addr (0x%lx) or len (0x%lx) not PMD aligned (PMD_SIZE: 0x%lx)\n",
				addr, len, PMD_SIZE);
		return -EINVAL;
	}

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
	if (vma) {
		ptshare_mm_err(current->mm, "ptshare_validate_mmap: intersection with existing VMA [0x%lx, 0x%lx)\n",
				vma->vm_start, vma->vm_end);
		ret = -EINVAL;
	}

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

vm_fault_t ptshare_handle_mm_fault(struct vm_area_struct *ptshare_vma,
				 struct vm_fault *vmf, unsigned int flags)
{
	struct vm_fault ptshare_vmf = *vmf;
	vm_fault_t ret;
	pgd_t *pgd;
	p4d_t *p4d;
	struct ptdesc *ptdesc;

	vma_assert_locked(ptshare_vma);
	assert_fault_locked(vmf);

	ptshare_vmf.flags = flags;
	pgd = pgd_offset(ptshare_mm, vmf->address);
	p4d = p4d_alloc(ptshare_mm, pgd, vmf->address);
	if (!p4d)
		return VM_FAULT_OOM;

	ptshare_vmf.pud = pud_alloc(ptshare_mm, p4d, vmf->address);
	if (!ptshare_vmf.pud)
		return VM_FAULT_OOM;

	ptshare_vmf.pmd = pmd_alloc(ptshare_mm, ptshare_vmf.pud, vmf->address);
	if (!ptshare_vmf.pmd)
		return VM_FAULT_OOM;

	ptshare_vmf.vma = ptshare_vma;
	ptshare_vmf.prealloc_pte = NULL;
	ptshare_vmf.real_address = vmf->real_address;

	ret = handle_pte_fault(&ptshare_vmf);

	/*
	 * PMD Splicing:
	 * The ptshare_mm now has a populated PTE page table.
	 * We "splice" this shared PTE page table directly into the
	 * faulting process's PMD. We increment the ptdesc refcount
	 * so the shared page table is not freed prematurely when a
	 * single sharing process unmaps the region.
	 */
	if (!(ret & VM_FAULT_ERROR) && (ret & VM_FAULT_COMPLETED) && !pmd_none(*ptshare_vmf.pmd)) {
		spinlock_t *ptl = pmd_lock(vmf->vma->vm_mm, vmf->pmd);

		if (pmd_none(*vmf->pmd)) {
			ptdesc = page_ptdesc(pmd_page(*ptshare_vmf.pmd));
			ptdesc_get(ptdesc);
			set_pmd_at(vmf->vma->vm_mm, vmf->address, vmf->pmd, *ptshare_vmf.pmd);
			mm_inc_nr_ptes(vmf->vma->vm_mm);
		}

		spin_unlock(ptl);
	}

	if (ret & (VM_FAULT_RETRY | VM_FAULT_COMPLETED)) {
		/*
		 * handle_pte_fault has dropped the ptshare_mm lock.
		 * Now we must drop the faulting process's lock.
		 */
		release_fault_lock(vmf);

		return ret;
	}

	return ret;
}

vm_fault_t ptshare_do_page_fault(struct vm_fault *vmf)
{
	unsigned int flags = vmf->flags	& ~FAULT_FLAG_VMA_LOCK;
	struct vm_area_struct *ptshare_vma;
	vm_fault_t ret;

	assert_fault_locked(vmf);

	/*
	 * Phase 1: Optimistic Per-VMA Lock
	 *
	 * Attempt to acquire the VMA lock for the shared manager address space.
	 * This is the fast path that avoids the global mmap_lock.
	 */
	ptshare_vma = lock_vma_under_rcu(ptshare_mm, vmf->address);
	if (ptshare_vma) {
		ret = ptshare_handle_mm_fault(ptshare_vma, vmf,
					      flags | FAULT_FLAG_VMA_LOCK);

		/*
		 * If handle_pte_fault() returned RETRY or COMPLETED, it
		 * already dropped the VMA lock.
		 */
		if (!(ret & (VM_FAULT_RETRY | VM_FAULT_COMPLETED)))
			vma_end_read(ptshare_vma);

		return ret;
	}

	/*
	 * Phase 2: Fallback to mmap_lock
	 *
	 * Use the nested helper to safely look up and lock the shared VMA
	 * while already holding the faulting process's lock.
	 */
	ptshare_vma = lock_mm_and_find_vma_nested(ptshare_mm, vmf->address,
						  NULL, SINGLE_DEPTH_NESTING);
	if (!ptshare_vma)
		return VM_FAULT_SIGSEGV;

	ret = ptshare_handle_mm_fault(ptshare_vma, vmf,
				      flags & ~FAULT_FLAG_VMA_LOCK);

	/*
	 * If handle_pte_fault() returned RETRY, it already dropped the
	 * mmap_lock.
	 */
	if (!(ret & (VM_FAULT_RETRY | VM_FAULT_COMPLETED)))
		mmap_read_unlock(ptshare_mm);

	return ret;
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
 * @mm: The mm_struct where the PTE range is being freed.
 * @token: The page table page being freed.
 *
 * During unmapping, we must ensure that shared page tables are not
 * accidentally reclaimed while still in use by other processes.
 *
 * If the page table is shared (refcount > 1), we decrement its reference
 * count and return true to indicate that the caller (guest MM) should NOT
 * free the physical page. The last participant (often the manager MM) will
 * handle the actual reclamation.
 */
bool ptshare_free_pte_range(struct mm_struct *mm, pgtable_t token)
{
	struct ptdesc *pt;

	/* The manager MM itself follows standard reclamation */
	if (mm == ptshare_mm)
		return false;

	pt = page_ptdesc(token);

	if (ptdesc_refcount(pt) > 1) {
		ptdesc_put(pt);
		return true;
	}

	return false;
}

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
	if (IS_ENABLED(CONFIG_X86))
		on_each_cpu(ptshare_x86_flush_tlb_range_ipi, (void *)range, 1);
	else if (IS_ENABLED(CONFIG_ARM64))
		ptshare_arm64_flush_tlb_range(range);
}

static const struct mmu_notifier_ops ptshare_mmu_notifier_ops = {
	.invalidate_range_start = ptshare_invalidate_range_start,
	.invalidate_range_end = ptshare_invalidate_range_end,
};

static struct mmu_notifier ptshare_mmu_notifier = {
	.ops = &ptshare_mmu_notifier_ops,
};

static int __init ptshare_init(void)
{
	ptshare_mm = mm_alloc();
	if (!ptshare_mm)
		return -ENOMEM;

	return mmu_notifier_register(&ptshare_mmu_notifier, ptshare_mm);
}
core_initcall(ptshare_init);
