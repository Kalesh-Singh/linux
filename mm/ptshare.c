// SPDX-License-Identifier: GPL-2.0
/*
 * Shared Page Table Support
 *
 * Copyright (c) 2026, Google LLC.
 * Author: Kalesh Singh <kaleshsingh@google.com>
 */
#include <linux/ptshare.h>

#include <linux/init.h>
#include <linux/mman.h>
#include <linux/sched/mm.h>

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

static int __init ptshare_init(void)
{
	ptshare_mm = mm_alloc();
	if (!ptshare_mm)
		return -ENOMEM;

	return 0;
}
core_initcall(ptshare_init);
