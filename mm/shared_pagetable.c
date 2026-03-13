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
	.flexible_array	= MM_STRUCT_FLEXIBLE_ARRAY_INIT,
	INIT_MM_CONTEXT(ptshare_mm)
};
