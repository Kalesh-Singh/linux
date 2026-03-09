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
 * Get the PMD entry for a given address in an mm_struct.
 * Allocates intermediate levels if necessary.
 */
static pmd_t *get_pmd(struct mm_struct *mm, unsigned long addr)
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
 * Get the PTE table (as a pgtable_t) for a given address in an mm_struct.
 * Allocates the PTE table if it does not exist.
 */
static pgtable_t get_pte_table(struct mm_struct *mm, unsigned long addr)
{
	pmd_t *pmd;

	pmd = get_pmd(mm, addr);
	if (!pmd)
		return NULL;

	if (pmd_none(*pmd)) {
		if (__pte_alloc(mm, pmd))
			return NULL;
	}

	return pmd_pgtable(*pmd);
}

/*
 * splice_shared_pte - Link a shared host PTE table into the guest page hierarchy.
 * @vma: Guest VMA
 * @addrp: Pointer to the faulting address
 * @flags: Fault flags
 *
 * This function locates (or allocates) the corresponding PTE table in the global
 * ptshare_host_mm and links it into the guest's PMD entry. This allows the guest
 * to share last-level page tables for system libraries.
 *
 * Returns VM_FAULT_NOPAGE on successful splicing to trigger a fault restart
 * in the guest, or 0 if the VMA is not a candidate for sharing.
 */
vm_fault_t
splice_shared_pte(struct vm_area_struct *vma, unsigned long *addrp,
			unsigned int flags)
{
	struct ptshare_data *info;
	struct mm_struct *host_mm;
	pmd_t *guest_pmd;
	pgtable_t host_pte_table;

	if ((!vma->vm_file) || (!vma->vm_file->f_mapping))
		return 0;

	info = vma->vm_file->f_mapping->ptshare_data;
	if (!info)
		return 0;

	host_mm = info->mm;

	/*
	 * We need to link the host's PTE table into the guest's PMD.
	 * Intermediate host levels are synchronized by host_mm->mmap_lock.
	 */
	mmap_read_lock(host_mm);
	host_pte_table = get_pte_table(host_mm, *addrp);
	if (!host_pte_table) {
		mmap_read_unlock(host_mm);
		return VM_FAULT_OOM;
	}

	guest_pmd = get_pmd(vma->vm_mm, *addrp);
	if (!guest_pmd) {
		mmap_read_unlock(host_mm);
		return VM_FAULT_OOM;
	}

	/*
	 * If the guest doesn't have this PTE table linked yet, link it now.
	 */
	if (pmd_none(*guest_pmd)) {
		spinlock_t *guest_ptl;
		struct page *pte_page = host_pte_table;

		/*
		 * Increments refcount on the PTE table page so it remains
		 * valid even if the host MM is destroyed (though our
		 * global host MM is static).
		 */
		get_page(pte_page);

		guest_ptl = pmd_lockptr(vma->vm_mm, guest_pmd);
		spin_lock(guest_ptl);
		if (pmd_none(*guest_pmd)) {
			mm_inc_nr_ptes(vma->vm_mm);
			pmd_populate(vma->vm_mm, guest_pmd, host_pte_table);
		} else {
			put_page(pte_page);
		}
		spin_unlock(guest_ptl);

		mmap_read_unlock(host_mm);
		return VM_FAULT_NOPAGE; /* Restart fault to use the new PTE table */
	}

	mmap_read_unlock(host_mm);

	return 0;
}
