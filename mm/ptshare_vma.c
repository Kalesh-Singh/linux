// SPDX-License-Identifier: GPL-2.0
/*
 * VMA Management for Shared Page Table Support
 *
 * Copyright (c) 2026, Google LLC.
 * Author: Kalesh Singh <kaleshsingh@google.com>
 */

#include <linux/ptshare_vma.h>

#include <linux/xarray.h>

static DEFINE_XARRAY(ptshare_vma_refs);

int ptshare_vma_refcount_init(struct vm_area_struct *vma)
{
	return xa_err(xa_store(&ptshare_vma_refs, vma->vm_start, xa_mk_value(1), GFP_KERNEL));
}

void ptshare_vma_refcount_destroy(struct vm_area_struct *vma)
{
	xa_erase(&ptshare_vma_refs, vma->vm_start);
}