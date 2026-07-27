/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Kernel compatibility for elc_ethercat.
 *
 * Policy: if the EtherLab master builds and runs on a kernel, this transport
 * aims to support that kernel. Version-specific differences belong here so
 * elc_ethercat_main.c stays free of LINUX_VERSION_CODE soup.
 *
 * Floor (compile-time): Linux 4.19 — common RT / EtherLab baseline in the
 * field. Lower floors need an explicit kcompat extension and a build smoke
 * on that kernel. Primary hardware acceptance remains the documented 6.1 RT
 * target; older kernels are supported for build and progressive validation.
 *
 * When adding a new kernel API:
 *   1. Prefer a small helper or macro in this file.
 *   2. Call the helper from main code with no #if LINUX_VERSION_CODE there.
 *   3. Note the kernel version and rationale in a short comment here.
 *   4. Smoke-build on the oldest and newest kernels you care about.
 */

#ifndef ELC_KCOMPAT_H
#define ELC_KCOMPAT_H

#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 19, 0)
#error "elc_ethercat requires Linux >= 4.19 (raise only with kcompat + smoke build)"
#endif

#include <linux/compat.h>
#include <linux/fs.h>
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

/*
 * Scheduler types: 5.9+ needs the UAPI header for struct sched_attr used by
 * sched_setattr_nocheck. Older trees provide struct sched_param via
 * linux/sched/types.h for sched_setscheduler_nocheck.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
#include <uapi/linux/sched/types.h>
#else
#include <linux/sched/types.h>
#endif

/* ---- cyclic task FIFO priority ----------------------------------------- */

/**
 * elc_set_fifo_priority - apply SCHED_FIFO to a kthread (nocheck variants)
 * @task: thread from kthread_create (not yet running is OK)
 * @priority: 1..MAX_RT_PRIO-1; caller validates module parameter range
 *
 * 5.9+: sched_setattr_nocheck. Older: sched_setscheduler_nocheck.
 */
static inline int elc_set_fifo_priority(struct task_struct *task, int priority)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
	struct sched_attr attr = {
		.size = sizeof(attr),
		.sched_policy = SCHED_FIFO,
		.sched_priority = priority,
	};

	return sched_setattr_nocheck(task, &attr);
#else
	struct sched_param param = {
		.sched_priority = priority,
	};

	return sched_setscheduler_nocheck(task, SCHED_FIFO, &param);
#endif
}

/* ---- compat ioctl ------------------------------------------------------ */

/*
 * compat_ptr_ioctl() exists from 5.0. Before that, provide a same-named
 * local that maps the pointer then calls the unlocked handler. UAPI
 * structures are fixed-layout with no embedded user pointers.
 *
 * Usage in main:
 *   ELC_DEFINE_COMPAT_IOCTL(elc_ioctl)
 *   ...
 *   .unlocked_ioctl = elc_ioctl,
 *   ELC_FOP_COMPAT_IOCTL
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)

#define ELC_DEFINE_COMPAT_IOCTL(unlocked_fn) /* kernel compat_ptr_ioctl */
#define ELC_FOP_COMPAT_IOCTL .compat_ioctl = compat_ptr_ioctl,

#elif defined(CONFIG_COMPAT)

#define ELC_DEFINE_COMPAT_IOCTL(unlocked_fn)				\
static long elc_compat_ioctl(struct file *file, unsigned int cmd,	\
			     unsigned long arg)				\
{									\
	return (unlocked_fn)(file, cmd,					\
			     (unsigned long)compat_ptr(arg));		\
}

#define ELC_FOP_COMPAT_IOCTL .compat_ioctl = elc_compat_ioctl,

#else /* < 5.0, no CONFIG_COMPAT */

#define ELC_DEFINE_COMPAT_IOCTL(unlocked_fn)
#define ELC_FOP_COMPAT_IOCTL

#endif

#endif /* ELC_KCOMPAT_H */
