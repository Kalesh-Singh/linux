// SPDX-License-Identifier: GPL-2.0-only
/*
 * Share page table entries when possible to reduce the amount of extra
 * memory consumed by page tables
 *
 * Copyright (C) 2022 Oracle Corp. All rights reserved.
 * Authors:	Khalid Aziz <khalid.aziz@oracle.com>
 *		Matthew Wilcox <willy@infradead.org>
 */

#include <linux/mm.h>
#include <linux/fs.h>
#include <asm/pgalloc.h>
#include "internal.h"

/*
 * Create a new mm struct that will hold the shared PTEs. Pointer to
 * this new mm is stored in the data structure ptshare_data which also
 * includes a refcount for any current references to PTEs in this new
 * mm. This refcount is used to determine when the mm struct for shared
 * PTEs can be deleted.
 */
int
ptshare_new_mm(struct file *file, struct vm_area_struct *vma)
{
	struct mm_struct *new_mm;
	struct ptshare_data *info = NULL;
	int retval = 0;
	unsigned long start = vma->vm_start;
	unsigned long len = vma->vm_end - vma->vm_start;

	new_mm = mm_alloc();
	if (!new_mm) {
		retval = -ENOMEM;
		goto err_free;
	}
	new_mm->mmap_base = start;
	new_mm->task_size = len;
	if (!new_mm->task_size)
		new_mm->task_size--;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info) {
		retval = -ENOMEM;
		goto err_free;
	}
	info->mm = new_mm;
	info->start = start;
	info->size = len;
	refcount_set(&info->refcnt, 1);
	file->f_mapping->ptshare_data = info;

	return retval;

err_free:
	if (new_mm)
		mmput(new_mm);
	kfree(info);
	return retval;
}

/*
 * insert vma into mm holding shared page tables
 */
int
ptshare_insert_vma(struct mm_struct *mm, struct vm_area_struct *vma)
{
	struct vm_area_struct *new_vma;
	int err = 0;

	new_vma = vm_area_dup(vma);
	if (!new_vma)
		return -ENOMEM;

	new_vma->vm_file = NULL;
	/*
	 * This new vma belongs to host mm, so clear the VM_SHARED_PT
	 * flag on this so we know this is the host vma when we clean
	 * up page tables. Do not use THP for page table shared regions
	 */
	vm_flags_clear(new_vma, (VM_SHARED | VM_SHARED_PT));
	vm_flags_set(new_vma, VM_NOHUGEPAGE);
	new_vma->vm_mm = mm;

	err = insert_vm_struct(mm, new_vma);
	if (err)
		return -ENOMEM;

	return err;
}

/*
 * Free the mm struct created to hold shared PTEs and associated data
 * structures
 */
static inline void
free_ptshare_mm(struct ptshare_data *info)
{
	mmput(info->mm);
	kfree(info);
}

/*
 * This function is called when a reference to the shared PTEs in mm
 * struct is dropped. It updates refcount and checks to see if last
 * reference to the mm struct holding shared PTEs has been dropped. If
 * so, it cleans up the mm struct and associated data structures
 */
void
ptshare_del_mm(struct vm_area_struct *vma)
{
	struct ptshare_data *info;
	struct file *file = vma->vm_file;

	if (!file || (!file->f_mapping))
		return;
	info = file->f_mapping->ptshare_data;
	WARN_ON(!info);
	if (!info)
		return;

	if (refcount_dec_and_test(&info->refcnt)) {
		free_ptshare_mm(info);
		file->f_mapping->ptshare_data = NULL;
	}
}
