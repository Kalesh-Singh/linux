// SPDX-License-Identifier: GPL-2.0
/*
 * Shared Page Table Support
 *
 * Copyright (c) 2026, Google LLC.
 * Author: Kalesh Singh <kaleshsingh@google.com>
 */
#include <linux/ptshare.h>

#include <linux/init.h>
#include <linux/sched/mm.h>

struct mm_struct *ptshare_mm;

static int __init ptshare_init(void)
{
	ptshare_mm = mm_alloc();
	if (!ptshare_mm)
		return -ENOMEM;

	return 0;
}
core_initcall(ptshare_init);
