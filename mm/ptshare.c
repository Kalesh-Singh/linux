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
	if (refcount_dec_and_test(&desc->refcount))
		ptshare_free_desc(desc);
}

void ptshare_get_desc(struct ptshare_desc *desc)
{
	refcount_inc(&desc->refcount);
}
