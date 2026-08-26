// SPDX-License-Identifier: GPL-2.0
#include <linux/sched.h>
#include <linux/personality.h>
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/mm.h>
#include <linux/binfmts.h>
#include <linux/string.h>
#include <linux/p3s.h>
#include <linux/highmem.h>
#include <asm/memory.h>
#include "internal.h"

#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
unsigned long mm_task_size64(void)
{
	struct mm_struct *mm = current_pgtable_mm();

	if (!mm)
		return (1UL << vabits_actual);

	if (mm_is_4kb(mm))
		return 1UL << VA_BITS_4KB;

	return 1UL << vabits_actual;
}
EXPORT_SYMBOL(mm_task_size64);

unsigned long mm_task_size64_of(struct mm_struct *mm)
{
	if (!mm)
		return (1UL << vabits_actual);

	if (mm_is_4kb(mm))
		return 1UL << VA_BITS_4KB;

	return 1UL << vabits_actual;
}
EXPORT_SYMBOL(mm_task_size64_of);

unsigned long mm_default_map_window64(void)
{
	struct mm_struct *mm = current_pgtable_mm();

	if (!mm)
		return (1UL << VA_BITS_MIN);

	if (mm_is_4kb(mm))
		return 1UL << VA_BITS_4KB;

	return 1UL << VA_BITS_MIN;
}
EXPORT_SYMBOL(mm_default_map_window64);
#endif
