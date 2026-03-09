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

struct mm_struct *ptshare_host_mm;

static int __init ptshare_init(void)
{
	ptshare_host_mm = mm_alloc();
	if (!ptshare_host_mm)
		panic("Failed to allocate ptshare_host_mm");

#ifdef CONFIG_MEMCG
	ptshare_host_mm->owner = NULL;
#endif

	return 0;
}
core_initcall(ptshare_init);

/*
 */
static pmd_t
*get_pmd(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;

	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd))
		return NULL;

	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d)) {
		p4d = p4d_alloc(mm, pgd, addr);
		if (!p4d)
			return NULL;
	}

	pud = pud_offset(p4d, addr);
	if (pud_none(*pud)) {
		pud = pud_alloc(mm, p4d, addr);
		if (!pud)
			return NULL;
	}

	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd)) {
		pmd = pmd_alloc(mm, pud, addr);
		if (!pmd)
			return NULL;
	}

	return pmd;
}

/*
 * Find the shared pmd entries in host mm struct and install them into
 * guest page tables.
 */
static int
ptshare_copy_pmd(struct mm_struct *host_mm, struct mm_struct *guest_mm,
			struct vm_area_struct *vma, unsigned long addr)
{
	pgd_t *guest_pgd;
	p4d_t *guest_p4d;
	pud_t *guest_pud;
	pmd_t *host_pmd;
	spinlock_t *host_ptl, *guest_ptl;

	guest_pgd = pgd_offset(guest_mm, addr);
	guest_p4d = p4d_offset(guest_pgd, addr);
	if (p4d_none(*guest_p4d)) {
		guest_p4d = p4d_alloc(guest_mm, guest_pgd, addr);
		if (!guest_p4d)
			return 1;
	}

	guest_pud = pud_offset(guest_p4d, addr);
	if (pud_none(*guest_pud)) {
		host_pmd = get_pmd(host_mm, addr);
		if (!host_pmd)
			return 1;

		get_page(virt_to_page(host_pmd));
		host_ptl = pmd_lockptr(host_mm, host_pmd);
		guest_ptl = pud_lockptr(guest_mm, guest_pud);
		spin_lock(host_ptl);
		spin_lock(guest_ptl);
		pud_populate(guest_mm, guest_pud,
			(pmd_t *)((unsigned long)host_pmd & PAGE_MASK));
		put_page(virt_to_page(host_pmd));
		spin_unlock(guest_ptl);
		spin_unlock(host_ptl);
	}

	return 0;
}

/*
 * Find the shared page tables in hosting mm struct and install those in
 * the guest mm struct
 */
vm_fault_t
find_shared_vma(struct vm_area_struct **vmap, unsigned long *addrp,
			unsigned int flags)
{
	struct ptshare_data *info;
	struct mm_struct *host_mm;
	struct vm_area_struct *host_vma, *guest_vma = *vmap;
	unsigned long host_addr;
	pmd_t *guest_pmd, *host_pmd;

	if ((!guest_vma->vm_file) || (!guest_vma->vm_file->f_mapping))
		return 0;
	info = guest_vma->vm_file->f_mapping->ptshare_data;
	if (!info) {
		pr_warn("VM_SHARED_PT vma with NULL ptshare_data");
		dump_stack_print_info(KERN_WARNING);
		return 0;
	}
	host_mm = info->mm;

	mmap_read_lock(host_mm);
	host_addr = *addrp - guest_vma->vm_start + host_mm->mmap_base;
	host_pmd = get_pmd(host_mm, host_addr);
	guest_pmd = get_pmd(guest_vma->vm_mm, *addrp);
	if (!pmd_same(*guest_pmd, *host_pmd)) {
		set_pmd(guest_pmd, *host_pmd);
		mmap_read_unlock(host_mm);
		return VM_FAULT_NOPAGE;
	}

	*addrp = host_addr;
	host_vma = find_vma(host_mm, host_addr);
	if (!host_vma)
		return VM_FAULT_SIGSEGV;

	/*
	 * Point vm_mm for the faulting vma to the mm struct holding shared
	 * page tables so the fault handling will happen in the right
	 * shared context
	 */
	guest_vma->vm_mm = host_mm;

	return 0;
}

/*
 * insert vma into mm holding shared page tables
 */
int
ptshare_insert_vma(struct mm_struct *host_mm, struct vm_area_struct *vma)
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
	new_vma->vm_mm = host_mm;

	err = insert_vm_struct(host_mm, new_vma);
	if (err)
		return -ENOMEM;

	/*
	 * Copy the PMD entries from host mm to guest so they use the
	 * same PTEs
	 */
	err = ptshare_copy_pmd(host_mm, vma->vm_mm, vma, vma->vm_start);

	return err;
}
