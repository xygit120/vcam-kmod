/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Offline compile-check shim for the vcam kernel module.  NOT used by kbuild.
 *
 * Why this exists
 * ---------------
 * This host has no Linux kernel tree, no aarch64 cross compiler and no NDK, so
 * a real `make modules` is impossible here. The module sources nevertheless
 * include <linux/...> headers exactly as they must for kbuild, so this shim
 * supplies a faithful-enough subset of that API to let the compiler type-check
 * the module and to let a host harness actually execute the pool and the ported
 * logic.
 *
 * The shim mirrors the real signatures; it deliberately does not emulate kernel
 * semantics beyond what the harness needs (a synthetic fd -> dma_buf registry,
 * real pthread mutexes, libc allocation).
 *
 * Build the module for real with:
 *     make -C <kernel-tree> M=$PWD modules
 */
#ifndef VCAM_SHIM_KERNEL_H
#define VCAM_SHIM_KERNEL_H

#ifdef __KERNEL__
#error "the offline shim must not be used inside a real kernel build"
#endif

#ifndef VCAM_HOST_BUILD
#error "define VCAM_HOST_BUILD=1 for the offline harness"
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int8_t   __s8;
typedef int16_t  __s16;
typedef int32_t  __s32;
typedef int64_t  __s64;

typedef __u8  u8;
typedef __u16 u16;
typedef __u32 u32;
typedef __u64 u64;
typedef __s8  s8;
typedef __s16 s16;
typedef __s32 s32;
typedef __s64 s64;
typedef unsigned long ulong;

#ifndef NULL
#define NULL ((void *)0)
#endif

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* --- errno -------------------------------------------------------------
 * The module returns -EINVAL/-EBUSY/... symbolically; the host build reuses
 * the libc's errno values (they are not required to match the kernel's numbers
 * because no errno ever crosses the ABI boundary -- the ioctl return value is
 * what userspace sees). */
/* The kernel's __user/__iomem annotations are no-ops for the host build. */
#define __user
#define __kernel
#define __iomem
#define __force
#define __must_check
#define __always_inline inline

#define likely(x)   (x)
#define unlikely(x) (x)

/* --- error pointers --------------------------------------------------- */
static inline void *ERR_PTR(long err) { return (void *)(intptr_t)err; }
static inline bool IS_ERR(const void *p)
{
	return (uintptr_t)p >= (uintptr_t)-4095L;
}
static inline long PTR_ERR(const void *p) { return (long)(intptr_t)p; }

/* --- logging ---------------------------------------------------------- */
#define pr_info(fmt, ...) printf("vcam: " fmt "\n", ##__VA_ARGS__)
#define pr_warn(fmt, ...) printf("vcam: warn: " fmt "\n", ##__VA_ARGS__)
#define pr_err(fmt, ...)  printf("vcam: err: " fmt "\n", ##__VA_ARGS__)

/* --- memory ----------------------------------------------------------- */
typedef unsigned vcam_gfp_t;
#define GFP_KERNEL 0u

static inline void *kmalloc(size_t size, vcam_gfp_t flags)
{
	(void)flags;
	return calloc(1, size);
}
#define kzalloc(size, flags) kmalloc((size), (flags))
#define kfree(p) free((void *)(p))

/* --- mutex ------------------------------------------------------------ */
struct mutex {
	pthread_mutex_t mu;
};

#define DEFINE_MUTEX(name) struct mutex name = { PTHREAD_MUTEX_INITIALIZER }

static inline void mutex_init(struct mutex *m) { pthread_mutex_init(&m->mu, NULL); }
static inline void mutex_destroy(struct mutex *m) { pthread_mutex_destroy(&m->mu); }
static inline void mutex_lock(struct mutex *m) { pthread_mutex_lock(&m->mu); }
static inline void mutex_unlock(struct mutex *m) { pthread_mutex_unlock(&m->mu); }

/* --- dma-buf ---------------------------------------------------------- */
struct dma_buf {
	int fd;
	unsigned long size;
};

#define VCAM_SHIM_MAX_DMA_BUFS 64

struct vcam_shim_fd_state {
	unsigned long size;
	unsigned refcnt;
	unsigned peak;
};

static struct vcam_shim_fd_state vcam_shim_fds[VCAM_SHIM_MAX_DMA_BUFS];

static inline void vcam_shim_register_fd(int fd, unsigned long size)
{
	if (fd < 0 || fd >= VCAM_SHIM_MAX_DMA_BUFS) return;
	vcam_shim_fds[fd].size = size;
	vcam_shim_fds[fd].refcnt = 0;
	vcam_shim_fds[fd].peak = 0;
}

static inline struct dma_buf *dma_buf_get(int fd)
{
	static struct dma_buf scratch[VCAM_SHIM_MAX_DMA_BUFS];
	if (fd < 0 || fd >= VCAM_SHIM_MAX_DMA_BUFS) return ERR_PTR(-EINVAL);
	if (vcam_shim_fds[fd].size == 0) return ERR_PTR(-ENOENT);
	vcam_shim_fds[fd].refcnt++;
	if (vcam_shim_fds[fd].refcnt > vcam_shim_fds[fd].peak)
		vcam_shim_fds[fd].peak = vcam_shim_fds[fd].refcnt;
	scratch[fd].fd = fd;
	scratch[fd].size = vcam_shim_fds[fd].size;
	return &scratch[fd];
}

static inline void dma_buf_put(struct dma_buf *dmabuf)
{
	if (dmabuf == NULL) return;
	if (dmabuf->fd >= 0 && dmabuf->fd < VCAM_SHIM_MAX_DMA_BUFS &&
	    vcam_shim_fds[dmabuf->fd].refcnt > 0)
		vcam_shim_fds[dmabuf->fd].refcnt--;
}

static inline unsigned vcam_shim_fd_refcnt(int fd)
{
	if (fd < 0 || fd >= VCAM_SHIM_MAX_DMA_BUFS) return 0;
	return vcam_shim_fds[fd].refcnt;
}

/* --- uaccess ---------------------------------------------------------- */
static inline unsigned long copy_from_user(void *to, const void *from,
					   unsigned long n)
{
	memcpy(to, from, n);
	return 0;
}

static inline unsigned long copy_to_user(void *to, const void *from,
					 unsigned long n)
{
	memcpy(to, from, n);
	return 0;
}

/* --- misc device ------------------------------------------------------ */
struct inode;
struct file {
	void *private_data;
};

struct file_operations {
	struct module *owner;
	int (*open)(struct inode *, struct file *);
	int (*release)(struct inode *, struct file *);
	long (*unlocked_ioctl)(struct file *, unsigned int, unsigned long);
};

#define MISC_DYNAMIC_MINOR 255

struct miscdevice {
	int minor;
	const char *name;
	const struct file_operations *fops;
};

static inline int misc_register(struct miscdevice *misc) { (void)misc; return 0; }
static inline void misc_deregister(struct miscdevice *misc) { (void)misc; }

/* --- module boilerplate ----------------------------------------------- */
#define MODULE_LICENSE(x)
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_VERSION(x)
#define MODULE_ALIAS(x)
#define module_param(name, type, perm)
#define module_param_named(name, val, type, perm)
#define MODULE_PARM_DESC(name, desc)

/* module_init/module_exit become real (harness-callable) functions so the
 * module's own init/exit path is exercised, not just the pool functions. */
#define module_init(fn) int vcam_shim_module_init(void) { return fn(); }
#define module_exit(fn) void vcam_shim_module_exit(void) { fn(); }

struct module;
#define THIS_MODULE ((struct module *)0)
#define __init
#define __exit
#define __MUTEX_INITIALIZER(m) { PTHREAD_MUTEX_INITIALIZER }

/* --- ioctl accessors -------------------------------------------------- */
/* The shim keeps the same encoding the uapi header defines, so the harness can
 * assert on it -- and so the driver's _IOC_TYPE/_IOC_NR validation decodes the
 * numbers a kernel build would produce. The host toolchain ships its own ioctl
 * macros with a different layout, so drop them first. */
#undef _IOC
#undef _IO
#undef _IOR
#undef _IOW
#undef _IOWR
#undef _IOC_DIR
#undef _IOC_NR
#undef _IOC_TYPE

#define _IOC_NRBITS    8
#define _IOC_TYPEBITS  8
#define _IOC_SIZEBITS  14
#define _IOC_DIRBITS   2
#define _IOC_NRSHIFT   0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT  (_IOC_SIZESHIFT + _IOC_SIZEBITS)
#define _IOC_NONE  0u
#define _IOC_WRITE 1u
#define _IOC_READ  2u
#define _IOC(dir, type, nr, size)                       \
	(((dir) << _IOC_DIRSHIFT) | ((type) << _IOC_TYPESHIFT) | \
	 ((nr) << _IOC_NRSHIFT) | ((size) << _IOC_SIZESHIFT))
#define _IO(type, nr)          _IOC(_IOC_NONE, (type), (nr), 0)
#define _IOR(type, nr, t)      _IOC(_IOC_READ, (type), (nr), sizeof(t))
#define _IOW(type, nr, t)      _IOC(_IOC_WRITE, (type), (nr), sizeof(t))
#define _IOWR(type, nr, t)     _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), sizeof(t))
#define _IOC_DIR(nr) (((nr) >> _IOC_DIRSHIFT) & ((1u << _IOC_DIRBITS) - 1))
#define _IOC_NR(nr)  (((nr) >> _IOC_NRSHIFT) & ((1u << _IOC_NRBITS) - 1))
#define _IOC_TYPE(nr) (((nr) >> _IOC_TYPESHIFT) & ((1u << _IOC_TYPEBITS) - 1))

#endif /* VCAM_SHIM_KERNEL_H */
