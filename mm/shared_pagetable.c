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
#include "internal.h"

#ifndef INIT_MM_CONTEXT
#define INIT_MM_CONTEXT(name)
#endif

struct mm_struct ptshare_mm = {
	.mm_mt		= MTREE_INIT_EXT(mm_mt, MM_MT_FLAGS, ptshare_mm.mmap_lock),
	.pgd		= NULL,
	.mm_users	= ATOMIC_INIT(2),
	.mm_count	= ATOMIC_INIT(1),
	.write_protect_seq = SEQCNT_ZERO(ptshare_mm.write_protect_seq),
	MMAP_LOCK_INITIALIZER(ptshare_mm)
	.page_table_lock =  __SPIN_LOCK_UNLOCKED(ptshare_mm.page_table_lock),
	.arg_lock	=  __SPIN_LOCK_UNLOCKED(ptshare_mm.arg_lock),
	.mmlist		= LIST_HEAD_INIT(ptshare_mm.mmlist),
#ifdef CONFIG_PER_VMA_LOCK
	.vma_writer_wait = __RCUWAIT_INITIALIZER(ptshare_mm.vma_writer_wait),
	.mm_lock_seq	= SEQCNT_ZERO(ptshare_mm.mm_lock_seq),
#endif
	.user_ns	= &init_user_ns,
#ifdef CONFIG_SCHED_MM_CID
	.mm_cid.lock = __RAW_SPIN_LOCK_UNLOCKED(ptshare_mm.mm_cid.lock),
#endif
#ifdef CONFIG_SHARED_PAGETABLE
	.shpt_mm	= NULL,
#endif
	.flexible_array	= MM_STRUCT_FLEXIBLE_ARRAY_INIT,
	INIT_MM_CONTEXT(ptshare_mm)
};

/*
 * We only allow sharing of page tables for read-only file-backed
 * mappings to avoid having to deal with Copy-on-Write (CoW) of
 * the shared page tables.
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

	if (unlikely(!file || (prot & PROT_WRITE)))
		return -EINVAL;

	if (unlikely(!(flags & MAP_FIXED)))
		return -EINVAL;

	if (unlikely(!IS_ALIGNED(addr, PMD_SIZE) ||
		     !IS_ALIGNED(len, PMD_SIZE)))
		return -EINVAL;

	BUG_ON(current->mm == &ptshare_mm);

	mmap_read_lock_nested(&ptshare_mm, SINGLE_DEPTH_NESTING);
	vma = find_vma_intersection(&ptshare_mm, addr, addr + len);
	if (vma)
		ret = -EINVAL;
	mmap_read_unlock(&ptshare_mm);

	return ret;
}

void shpt_install_vma(struct mm_struct *mm, unsigned long addr,
		      unsigned long len, vm_flags_t vm_flags)
{
	struct vm_area_struct *vma, *new_vma;
	int ret;

	if (IS_ERR_VALUE(addr))
		return;

	if (!(vm_flags & VM_SHARED_PT))
		return;

	BUG_ON(mm == &ptshare_mm);

	vma = vma_lookup(mm, addr);
	BUG_ON(!vma);

	if (!mm->shpt_mm)
		mm->shpt_mm = &ptshare_mm;

	new_vma = vm_area_dup(vma);
	if (!new_vma)
		return;

	new_vma->vm_mm = &ptshare_mm;
	vm_flags_clear(new_vma, VM_SHARED_PT);

	mmap_write_lock_nested(&ptshare_mm, SINGLE_DEPTH_NESTING);
	ret = insert_vm_struct(&ptshare_mm, new_vma);
	if (ret) {
		mmap_write_unlock(&ptshare_mm);
		vm_area_free(new_vma);
		return;
	}
	vm_stat_account(&ptshare_mm, new_vma->vm_flags, vma_pages(new_vma));
	mmap_write_unlock(&ptshare_mm);
}

vm_fault_t shpt_handle_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *ptshare_vma;
	struct vm_fault ptshare_vmf = *vmf;
	vm_fault_t ret;
	pgd_t *pgd;
	p4d_t *p4d;
	spinlock_t *ptl;
	struct ptdesc *ptdesc;

	ptshare_vma = lock_vma_under_rcu(&ptshare_mm, vmf->address);
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

		if (mmap_read_lock_killable_nested(&ptshare_mm, SINGLE_DEPTH_NESTING)) {
			release_fault_lock(vmf);
			return VM_FAULT_RETRY;
		}

		ptshare_vma = vma_lookup(&ptshare_mm, vmf->address);
		if (!ptshare_vma) {
			mmap_read_unlock(&ptshare_mm);
			return VM_FAULT_SIGSEGV;
		}
		ptshare_vmf.flags &= ~FAULT_FLAG_VMA_LOCK;
	} else {
		ptshare_vmf.flags |= FAULT_FLAG_VMA_LOCK;
	}

	pgd = pgd_offset(&ptshare_mm, vmf->address);
	p4d = p4d_alloc(&ptshare_mm, pgd, vmf->address);
	if (!p4d) {
		ret = VM_FAULT_OOM;
		goto out;
	}

	ptshare_vmf.pud = pud_alloc(&ptshare_mm, p4d, vmf->address);
	if (!ptshare_vmf.pud) {
		ret = VM_FAULT_OOM;
		goto out;
	}

	ptshare_vmf.pmd = pmd_alloc(&ptshare_mm, ptshare_vmf.pud, vmf->address);
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
			mmap_read_unlock(&ptshare_mm);
	}

	return ret;

out:
	if (ptshare_vmf.flags & FAULT_FLAG_VMA_LOCK)
		vma_end_read(ptshare_vma);
	else
		mmap_read_unlock(&ptshare_mm);
	return ret;
}
