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
 * Get the PUD entry for a given address in an mm_struct.
 * Allocates intermediate levels (PGD, P4D) if necessary.
 */
static pud_t *get_pud(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;

	pgd = pgd_offset(mm, addr);
	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d)) {
		p4d = p4d_alloc(mm, pgd, addr);
		if (!p4d)
			return NULL;
	}

	pud = pud_offset(p4d, addr);
	return pud;
}

/*
 * Get the PMD table (as a pmd_t *) for a given address in an mm_struct.
 * Allocates the PMD table if it does not exist.
 */
static pmd_t *get_pmd_table(struct mm_struct *mm, unsigned long addr)
{
	pud_t *pud;
	pmd_t *pmd;

	pud = get_pud(mm, addr);
	if (!pud)
		return NULL;

	if (pud_none(*pud)) {
		pmd = pmd_alloc(mm, pud, addr);
		if (!pmd)
			return NULL;
	}

	return pmd_offset(pud, addr);
}

/*
 * Find the shared page tables in hosting mm struct and install those in
 * the guest mm struct. This links the PMD level of the host MM into the
 * guest MM, so that subsequent PTE-level faults in the guest will
 * populate the shared PMD table.
 */
vm_fault_t
find_shared_vma(struct vm_area_struct **vmap, unsigned long *addrp,
			unsigned int flags)
{
	struct ptshare_data *info;
	struct mm_struct *host_mm;
	struct vm_area_struct *guest_vma = *vmap;
	pud_t *guest_pud;
	pmd_t *host_pmd_table;

	if ((!guest_vma->vm_file) || (!guest_vma->vm_file->f_mapping))
		return 0;

	info = guest_vma->vm_file->f_mapping->ptshare_data;
	if (!info)
		return 0;

	host_mm = info->mm;

	/*
	 * We need to link the host's PMD table into the guest's PUD.
	 * Intermediate host levels are synchronized by host_mm->mmap_lock.
	 */
	mmap_read_lock(host_mm);
	host_pmd_table = get_pmd_table(host_mm, *addrp);
	if (!host_pmd_table) {
		mmap_read_unlock(host_mm);
		return VM_FAULT_OOM;
	}

	guest_pud = get_pud(guest_vma->vm_mm, *addrp);
	if (!guest_pud) {
		mmap_read_unlock(host_mm);
		return VM_FAULT_OOM;
	}

	/*
	 * If the guest doesn't have this PMD table linked yet, link it now.
	 */
	if (pud_none(*guest_pud)) {
		spinlock_t *guest_ptl;

		/*
		 * Increments refcount on the PMD table page so it remains
		 * valid even if the host MM is destroyed (though our
		 * global host MM is static).
		 */
		get_page(virt_to_page(host_pmd_table));

		guest_ptl = pud_lockptr(guest_vma->vm_mm, guest_pud);
		spin_lock(guest_ptl);
		if (pud_none(*guest_pud)) {
			pud_populate(guest_vma->vm_mm, guest_pud,
				     (pmd_t *)((unsigned long)host_pmd_table & PAGE_MASK));
		} else {
			put_page(virt_to_page(host_pmd_table));
		}
		spin_unlock(guest_ptl);

		mmap_read_unlock(host_mm);
		return VM_FAULT_NOPAGE; /* Restart fault to use the new PMD */
	}

	mmap_read_unlock(host_mm);

	/*
	 * Swapping vm_mm ensures handle_mm_fault() continues its work
	 * using the host MM context for the PTE level.
	 */
	guest_vma->vm_mm = host_mm;

	return 0;
}
