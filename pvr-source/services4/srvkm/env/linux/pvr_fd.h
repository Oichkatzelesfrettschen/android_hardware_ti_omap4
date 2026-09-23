/* SPDX-License-Identifier: MIT OR GPL-2.0 */
#ifndef PVR_FD_H
#define PVR_FD_H

#include <linux/version.h>
#include <linux/file.h>
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,7,0))
#include <linux/fcntl.h>
#else
#include <linux/fdtable.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#endif

/*
 * Reserves a descriptor in the caller's table with close-on-exec set, so a
 * sync fence fd does not leak into programs the process executes. Kernels
 * before 3.7 export get_unused_fd() but not alloc_fd(), which their
 * get_unused_fd_flags() macro expands to; there the flag is set in the
 * fdtable under file_lock, as alloc_fd() does.
 */
static inline int PVRGetUnusedFdCloexec(void)
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,7,0))
	return get_unused_fd_flags(O_CLOEXEC);
#else
	struct files_struct *psFiles = current->files;
	int iFd = get_unused_fd();

	if (iFd >= 0)
	{
		spin_lock(&psFiles->file_lock);
		FD_SET(iFd, files_fdtable(psFiles)->close_on_exec);
		spin_unlock(&psFiles->file_lock);
	}
	return iFd;
#endif
}

#endif /* PVR_FD_H */
