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
