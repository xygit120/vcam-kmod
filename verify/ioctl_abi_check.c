// SPDX-License-Identifier: GPL-2.0
/*
 * ioctl ABI check.
 *
 * vcam_uapi.h rolls its own _IOC encoding so that userspace and the module
 * agree without depending on any particular libc. That portability is only
 * useful if the encoding is *byte-identical* to the kernel's, otherwise the
 * module would reject every ioctl a real client sends.
 *
 * This file proves it by compiling the uapi next to the authoritative kernel
 * definition -- include/uapi/asm-generic/ioctl.h, which is the one arm64 (and
 * most other architectures) uses -- and statically asserting equality for every
 * command, plus asserting the driver-side decode of each command.
 *
 * Build and run:
 *   cc -std=gnu11 -Wall -Wextra -Werror ioctl_abi_check.c -o ioctl_abi_check
 *   ./ioctl_abi_check
 */

#include <stdio.h>

/* Authoritative kernel definition (fetched from the kernel tree; see README). */
#include "linux-asm-generic-ioctl.h"

/* Our portable definition. */
#include "../include/vcam_uapi.h"

#define CHECK_IOC(name, ours, kernel_expr)                                    \
	_Static_assert((unsigned long)(ours) == (unsigned long)(kernel_expr), \
		       "ioctl encoding mismatch: " name)

CHECK_IOC("GET_VERSION", VCAM_IOC_GET_VERSION,
	  _IOR(VCAM_IOC_MAGIC, 0x00, struct vcam_version));
CHECK_IOC("REGISTER_BUF", VCAM_IOC_REGISTER_BUF,
	  _IOWR(VCAM_IOC_MAGIC, 0x01, struct vcam_frame));
CHECK_IOC("UNREGISTER_BUF", VCAM_IOC_UNREGISTER_BUF,
	  _IOW(VCAM_IOC_MAGIC, 0x02, vcam_u32));
CHECK_IOC("SUBMIT", VCAM_IOC_SUBMIT, _IOW(VCAM_IOC_MAGIC, 0x03, struct vcam_frame));
CHECK_IOC("DEQUEUE", VCAM_IOC_DEQUEUE, _IOR(VCAM_IOC_MAGIC, 0x04, struct vcam_dequeue));
CHECK_IOC("RELEASE", VCAM_IOC_RELEASE, _IOW(VCAM_IOC_MAGIC, 0x05, vcam_u64));
CHECK_IOC("SET_RANGE", VCAM_IOC_SET_RANGE, _IOW(VCAM_IOC_MAGIC, 0x06, struct vcam_range));
CHECK_IOC("SET_POSE", VCAM_IOC_SET_POSE, _IOW(VCAM_IOC_MAGIC, 0x07, struct vcam_pose));
CHECK_IOC("GET_STATS", VCAM_IOC_GET_STATS, _IOR(VCAM_IOC_MAGIC, 0x08, struct vcam_stats));
CHECK_IOC("POLL", VCAM_IOC_POLL, _IOR(VCAM_IOC_MAGIC, 0x09, struct vcam_poll));

/* The driver validates _IOC_TYPE/_IOC_NR; the decode must line up too. */
_Static_assert(_IOC_TYPE(VCAM_IOC_SUBMIT) == VCAM_IOC_MAGIC, "submit type mismatch");
_Static_assert(_IOC_NR(VCAM_IOC_SUBMIT) == 0x03, "submit nr mismatch");
_Static_assert(_IOC_DIR(VCAM_IOC_SUBMIT) == _IOC_WRITE, "submit dir mismatch");
_Static_assert(_IOC_DIR(VCAM_IOC_DEQUEUE) == _IOC_READ, "dequeue dir mismatch");
_Static_assert(_IOC_DIR(VCAM_IOC_REGISTER_BUF) == (_IOC_READ | _IOC_WRITE),
	       "register dir mismatch");
_Static_assert(_IOC_SIZE(VCAM_IOC_SUBMIT) == sizeof(struct vcam_frame),
	       "submit size mismatch");
_Static_assert(VCAM_IOC_MAXNR == 0x09, "MAXNR must cover the highest command");
_Static_assert(_IOC_NR(VCAM_IOC_POLL) == VCAM_IOC_MAXNR,
	       "MAXNR must equal the highest command number");

/* The module's pool caps and the uapi's plane count must not drift apart. */
_Static_assert(VCAM_MAX_PLANES == 3u, "plane count changed");
_Static_assert(sizeof(struct vcam_format) == 40u, "vcam_format size changed");
_Static_assert(sizeof(struct vcam_frame) % 8u == 0u, "vcam_frame alignment changed");

int main(void)
{
	/* If the static assertions above held, every command agrees. Print the
	 * values so the numbers are recorded in the build log too. */
	printf("vcam ioctl ABI check: all commands match the kernel _IOC encoding\n");
	printf("  GET_VERSION    = 0x%08lx\n", (unsigned long)VCAM_IOC_GET_VERSION);
	printf("  REGISTER_BUF   = 0x%08lx\n", (unsigned long)VCAM_IOC_REGISTER_BUF);
	printf("  UNREGISTER_BUF = 0x%08lx\n", (unsigned long)VCAM_IOC_UNREGISTER_BUF);
	printf("  SUBMIT         = 0x%08lx\n", (unsigned long)VCAM_IOC_SUBMIT);
	printf("  DEQUEUE        = 0x%08lx\n", (unsigned long)VCAM_IOC_DEQUEUE);
	printf("  RELEASE        = 0x%08lx\n", (unsigned long)VCAM_IOC_RELEASE);
	printf("  SET_RANGE      = 0x%08lx\n", (unsigned long)VCAM_IOC_SET_RANGE);
	printf("  SET_POSE       = 0x%08lx\n", (unsigned long)VCAM_IOC_SET_POSE);
	printf("  GET_STATS      = 0x%08lx\n", (unsigned long)VCAM_IOC_GET_STATS);
	printf("  POLL           = 0x%08lx\n", (unsigned long)VCAM_IOC_POLL);
	printf("  sizeof(struct vcam_frame) = %u\n", (unsigned)sizeof(struct vcam_frame));
	return 0;
}
