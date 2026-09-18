// SPDX-License-Identifier: GPL-2.0
/*
 * vcam_ctrl.c -- the control plane.
 *
 * This replaces the sample's Binder service (transactions 11..25, 50, 51) with
 * ioctls on /dev/vcam. The mapping is deliberate and documented:
 *
 *   tx50 REGISTER_BUF   -> VCAM_IOC_REGISTER_BUF   (int,int,int,IBinder/IMemory)
 *   tx12 stop/cleanup   -> VCAM_IOC_UNREGISTER_BUF + module teardown
 *   tx11 install        -> VCAM_IOC_ATTACH         (the attach half of tx51)
 *   tx15 / tx22 range   -> VCAM_IOC_SET_RANGE
 *   tx16 / tx18 / tx19  -> VCAM_IOC_SET_POSE
 *   tx13 delta poll     -> VCAM_IOC_POLL
 *   (frame delivery)    -> VCAM_IOC_SUBMIT / VCAM_IOC_DEQUEUE / VCAM_IOC_RELEASE
 *
 * Transactions with no kernel-side meaning are intentionally absent: tx11's
 * "inject libvc into cameraserver" and tx12's "kill cameraserver" exist only
 * because the sample could not reach the buffer any other way. That whole
 * mechanism is what the .ko replaces.
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/dma-buf.h>
#include <linux/string.h>

#include "vcam_kmod.h"

static long vcam_ioctl_register(struct vcam_pool *pool, void __user *uarg)
{
	struct vcam_frame frame;
	u32 slot = 0;
	int rc;

	if (copy_from_user(&frame, uarg, sizeof(frame)))
		return -EFAULT;
	rc = vcam_pool_register(pool, frame.fd, &frame.fmt, &slot);
	if (rc != 0)
		return rc;
	frame.slot = slot;
	if (copy_to_user(uarg, &frame, sizeof(frame)))
		return -EFAULT;
	return 0;
}

static long vcam_ioctl_submit(struct vcam_pool *pool, void __user *uarg)
{
	struct vcam_frame frame;

	if (copy_from_user(&frame, uarg, sizeof(frame)))
		return -EFAULT;
	return vcam_pool_submit(pool, frame.slot, frame.seq, frame.timestamp_ns,
				frame.rotation_deg, frame.flags);
}

static long vcam_ioctl_dequeue(struct vcam_pool *pool, void __user *uarg)
{
	struct vcam_dequeue dq;
	int rc;

	memset(&dq, 0, sizeof(dq));
	rc = vcam_pool_dequeue(pool, &dq);
	if (rc != 0)
		return rc;
	if (copy_to_user(uarg, &dq, sizeof(dq)))
		return -EFAULT;
	return 0;
}

static long vcam_ioctl_get_version(struct vcam_pool *pool, void __user *uarg)
{
	struct vcam_version ver;

	memset(&ver, 0, sizeof(ver));
	ver.abi = VCAM_UAPI_ABI_VERSION;
	ver.max_streams = 1;		/* one injection stream per instance */
	ver.pool_size = pool->nslots;
	if (copy_to_user(uarg, &ver, sizeof(ver)))
		return -EFAULT;
	return 0;
}

long vcam_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct vcam_pool *pool = filp->private_data;
	void __user *uarg = (void __user *)(uintptr_t)arg;
	struct vcam_range range;
	struct vcam_pose pose;
	struct vcam_stats stats;
	struct vcam_poll poll;
	u32 slot;
	u64 seq;
	int rc;

	if (pool == NULL)
		return -ENODEV;
	if (_IOC_TYPE(cmd) != VCAM_IOC_MAGIC)
		return -ENOTTY;
	if (_IOC_NR(cmd) > VCAM_IOC_MAXNR)
		return -ENOTTY;

	switch (cmd) {
	case VCAM_IOC_GET_VERSION:
		return vcam_ioctl_get_version(pool, uarg);

	case VCAM_IOC_REGISTER_BUF:
		return vcam_ioctl_register(pool, uarg);

	case VCAM_IOC_UNREGISTER_BUF:
		if (copy_from_user(&slot, uarg, sizeof(slot)))
			return -EFAULT;
		return vcam_pool_unregister(pool, slot);

	case VCAM_IOC_SUBMIT:
		return vcam_ioctl_submit(pool, uarg);

	case VCAM_IOC_DEQUEUE:
		return vcam_ioctl_dequeue(pool, uarg);

	case VCAM_IOC_RELEASE:
		if (copy_from_user(&seq, uarg, sizeof(seq)))
			return -EFAULT;
		return vcam_pool_release(pool, seq);

	case VCAM_IOC_SET_RANGE:
		if (copy_from_user(&range, uarg, sizeof(range)))
			return -EFAULT;
		return vcam_pool_set_range(pool, &range);

	case VCAM_IOC_SET_POSE:
		if (copy_from_user(&pose, uarg, sizeof(pose)))
			return -EFAULT;
		return vcam_pool_set_pose(pool, &pose);

	case VCAM_IOC_GET_STATS:
		rc = vcam_pool_get_stats(pool, &stats);
		if (rc != 0)
			return rc;
		if (copy_to_user(uarg, &stats, sizeof(stats)))
			return -EFAULT;
		return 0;

	case VCAM_IOC_POLL:
		rc = vcam_pool_poll(pool, &poll);
		if (rc != 0)
			return rc;
		if (copy_to_user(uarg, &poll, sizeof(poll)))
			return -EFAULT;
		return 0;

	default:
		return -ENOTTY;
	}
}

int vcam_open(struct inode *inode, struct file *filp)
{
	(void)inode;
	filp->private_data = vcam_pool_singleton();
	return 0;
}

int vcam_release(struct inode *inode, struct file *filp)
{
	(void)inode;
	filp->private_data = NULL;
	return 0;
}
