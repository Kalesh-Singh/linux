#ifndef _LINUX_PTSHARE_DEBUG_H
#define _LINUX_PTSHARE_DEBUG_H

#include <linux/mm_types.h>
#include <linux/sched.h>
#include <linux/string.h>

extern struct mm_struct *ptshare_mm;

#define ptshare_err(fmt, ...) \
do { \
	if (!strcmp(current->comm, "ptshare_test")) \
		pr_err("PTSHARE_DEBUG: [%i (%s)]: " fmt, task_pid_nr(current), current->comm, ## __VA_ARGS__); \
} while (0)

#define ptshare_info(fmt, ...) \
do { \
	if (!strcmp(current->comm, "ptshare_test")) \
		pr_info("PTSHARE_DEBUG: [%i (%s)]: " fmt, task_pid_nr(current), current->comm, ## __VA_ARGS__); \
} while (0)

#define ptshare_mm_err(mm, fmt, ...) \
do { \
	if (!strcmp(current->comm, "ptshare_test")) \
		pr_err("PTSHARE_DEBUG: [%i (%s)] [%s]: " fmt, task_pid_nr(current), current->comm, \
			(mm == ptshare_mm ? "ptshare_mm" : "private_mm"), ## __VA_ARGS__); \
} while (0)

#define ptshare_mm_info(mm, fmt, ...) \
do { \
	if (!strcmp(current->comm, "ptshare_test")) \
		pr_info("PTSHARE_DEBUG: [%i (%s)] [%s]: " fmt, task_pid_nr(current), current->comm, \
			(mm == ptshare_mm ? "ptshare_mm" : "private_mm"), ## __VA_ARGS__); \
} while (0)

#endif /* _LINUX_PTSHARE_DEBUG_H */