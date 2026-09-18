// SPDX-License-Identifier: GPL-2.0
/*
 * vcam_main.c -- module entry, /dev/vcam registration and module parameters.
 *
 * Build (against the kernel tree that matches the device):
 *     make -C /path/to/kernel/tree M=$PWD modules
 *     insmod vcam.ko slots=3 backend=0
 *
 * The module is deliberately self-contained: it owns a frame pool and a control
 * plane, and it exposes the pool to a device glue layer through
 * vcam_pool_acquire()/vcam_pool_release(). Until such glue exists the built-in
 * bridge backend lets a userspace consumer drive the same state machine with
 * VCAM_IOC_DEQUEUE / VCAM_IOC_RELEASE, which is also how the host harness
 * exercises it.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>

#include "vcam_kmod.h"

#define VCAM_DEFAULT_SLOTS 3u

static unsigned int slots = VCAM_DEFAULT_SLOTS;
static unsigned int backend = VCAM_BACKEND_BRIDGE;

module_param(slots, uint, 0444);
MODULE_PARM_DESC(slots, "number of dma-buf pool slots (1..8)");
module_param(backend, uint, 0444);
MODULE_PARM_DESC(backend, "0 = userspace bridge, 1 = in-kernel pipeline glue");

static struct vcam_pool g_pool = {
	.lock = __MUTEX_INITIALIZER(g_pool.lock),
};
static bool g_pool_ready;

struct vcam_pool *vcam_pool_singleton(void)
{
	return g_pool_ready ? &g_pool : NULL;
}

static const struct file_operations vcam_fops = {
	.owner = THIS_MODULE,
	.open = vcam_open,
	.release = vcam_release,
	.unlocked_ioctl = vcam_ioctl,
};

static struct miscdevice vcam_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = VCAM_DRIVER_NAME,
	.fops = &vcam_fops,
};

static int __init vcam_init(void)
{
	u32 want;
	int rc;

	/* 0 or an out-of-range value falls back to the default rather than
	 * failing the load: a bad parameter should not cost the operator a
	 * reboot cycle. */
	want = (slots == 0 || slots > VCAM_MAX_SLOTS) ? VCAM_DEFAULT_SLOTS : slots;

	g_pool_ready = false;
	rc = vcam_pool_init(&g_pool, want);
	if (rc != 0) {
		pr_err("pool init failed: %d\n", rc);
		return rc;
	}
	rc = vcam_pool_set_backend(&g_pool, backend);
	if (rc != 0)
		pr_warn("unknown backend %u, keeping bridge\n", backend);

	rc = misc_register(&vcam_misc);
	if (rc != 0) {
		pr_err("misc_register failed: %d\n", rc);
		vcam_pool_destroy(&g_pool);
		return rc;
	}
	g_pool_ready = true;
	pr_info("ready: /dev/%s slots=%u backend=%u abi=%u\n",
		VCAM_DRIVER_NAME, want, g_pool.backend, VCAM_UAPI_ABI_VERSION);
	return 0;
}

static void __exit vcam_exit(void)
{
	g_pool_ready = false;
	misc_deregister(&vcam_misc);
	vcam_pool_destroy(&g_pool);
	pr_info("unloaded\n");
}

module_init(vcam_init);
module_exit(vcam_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("VCAM_DEV offline reconstruction");
MODULE_DESCRIPTION("Kernel-side frame-sink pool for the 18.0.apk virtual camera port");
MODULE_VERSION("0.1");
