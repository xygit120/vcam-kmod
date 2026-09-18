/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * vcam_uapi.h -- userspace/kernel ABI for the kernel-side rewrite of the
 * 18.0.apk virtual-camera data plane.
 *
 * This is a DESIGN ARTEFACT, not a shipped ABI: it exists so the contract can be
 * reviewed and so the host-side model and its test share one definition of it.
 * It compiles both as a kernel uapi header (__KERNEL__) and as a plain
 * userspace/C++ header.
 *
 * Design rules encoded here:
 *   1. The kernel module NEVER copies pixels. Userspace decodes (FFmpeg /
 *      MediaCodec) into dma-heap buffers and hands the module a dma-buf fd; the
 *      module only rebinds that buffer into the camera pipeline.
 *   2. Userspace owns the frame memory; the module owns only bookkeeping.
 *   3. A buffer a consumer still holds must never be accepted for re-fill.
 *      That single guard is what prevents tearing (see vcam_swap_model.cpp).
 */
#ifndef VCAM_UAPI_H
#define VCAM_UAPI_H

#ifdef __KERNEL__
# include <linux/types.h>
typedef __u8  vcam_u8;
typedef __u16 vcam_u16;
typedef __u32 vcam_u32;
typedef __s32 vcam_s32;
typedef __u64 vcam_u64;
typedef __s64 vcam_s64;
/* A real module uses <linux/ioctl.h>; the encoding below matches its arm64
 * layout (dir<<30 | size<<16 | type<<8 | nr) so the two agree. */
#else
# include <stdint.h>
typedef uint8_t  vcam_u8;
typedef uint16_t vcam_u16;
typedef uint32_t vcam_u32;
typedef int32_t  vcam_s32;
typedef uint64_t vcam_u64;
typedef int64_t  vcam_s64;
#endif

#define VCAM_UAPI_ABI_VERSION 1u
#define VCAM_MAX_PLANES       3u
#define VCAM_IOC_MAGIC        'v'

#define VCAM_F_MIRROR    (1u << 0)
#define VCAM_F_AUTOPOSE  (1u << 1)  /* equivalent of the sample's state+0xa8 */
#define VCAM_F_EOS       (1u << 2)  /* producer has no more frames */

struct vcam_version {
	vcam_u32 abi;
	vcam_u32 max_streams;
	vcam_u32 pool_size;
	vcam_u32 reserved;
};

struct vcam_plane {
	vcam_u32 offset;
	vcam_u32 stride;
};

struct vcam_format {
	vcam_u32 width;
	vcam_u32 height;
	vcam_u32 fourcc;              /* V4L2_PIX_FMT_* or the HAL format value */
	vcam_u32 num_planes;
	struct vcam_plane plane[VCAM_MAX_PLANES];
};

struct vcam_frame {
	vcam_s32 fd;                  /* input: dma-buf fd owned by the producer */
	vcam_u32 stream_id;           /* target camera stream */
	vcam_u32 slot;                /* REGISTER_BUF: output; others: input */
	vcam_u32 rotation_deg;        /* 0/90/180/270; anything else -> 0 */
	vcam_u32 flags;               /* VCAM_F_* */
	vcam_u64 seq;
	vcam_s64 timestamp_ns;
	struct vcam_format fmt;
};

struct vcam_range {
	vcam_s64 begin_ns;
	vcam_s64 end_ns;              /* -1 == to end of stream */
	vcam_u32 loop;
	vcam_u32 reserved;
};

struct vcam_dequeue {
	vcam_u64 seq;                 /* out: which frame the consumer took */
	vcam_u32 slot;                /* out: pool slot now held by the consumer */
	vcam_u32 resolved_angle_deg;  /* out: pose resolved for this frame */
	vcam_u32 resolved_mirror;     /* out: 0/1 */
	vcam_u32 reserved;
};

/* Pose. Mirrors what 18.0.apk keeps in its media state object:
 *   VCAM_F_AUTOPOSE clear -> the explicit rotation_deg / VCAM_F_MIRROR are used
 *   VCAM_F_AUTOPOSE set   -> angle = (270 - media_rotation_deg) and mirror = 1
 * This is the FUN_005415b4 rule (`mov w9,#0x10e` / `sub w2,w9,w8` on one branch,
 * `ldr w2,[x24,0xac]` + `cset w3,ne` on the other). */
struct vcam_pose {
	vcam_u32 flags;              /* VCAM_F_MIRROR | VCAM_F_AUTOPOSE */
	vcam_u32 rotation_deg;       /* explicit angle; only 0/90/180/270 accepted */
	vcam_s32 media_rotation_deg; /* the source clip's own rotation */
	vcam_u32 reserved;
};

/* Port of the tx13 delta poll: width/height are reported only when they changed
 * since the previous poll, and the format field is cleared after every read. */
struct vcam_poll {
	vcam_u32 client_attached;    /* the tx51 attach flag equivalent */
	vcam_u32 width_delta;
	vcam_u32 height_delta;
	vcam_u32 format;
};

struct vcam_stats {
	vcam_u64 submitted;
	vcam_u64 dequeued;
	vcam_u64 released;
	vcam_u64 rejected_busy;        /* re-fill attempted while not idle */
	vcam_u64 rejected_unknown;     /* unknown slot or sequence */
	vcam_u64 dequeued_empty;       /* consumer asked while queue empty */
	vcam_u64 looped;               /* range end reached and wrapped */
	vcam_u64 dropped_late;         /* frame past the range end, loop disabled */
	vcam_u64 bound;                /* times a frame was bound into the pipeline */
};

/* Host-side IOCTL encoding (mirrors Linux's _IOC on arm64; a kernel build uses
 * <linux/ioctl.h> instead). */
#define VCAM_IOC_NRBITS    8
#define VCAM_IOC_TYPEBITS  8
#define VCAM_IOC_SIZEBITS  14
#define VCAM_IOC_DIRBITS   2
#define VCAM_IOC_NRSHIFT   0
#define VCAM_IOC_TYPESHIFT (VCAM_IOC_NRSHIFT + VCAM_IOC_NRBITS)
#define VCAM_IOC_SIZESHIFT (VCAM_IOC_TYPESHIFT + VCAM_IOC_TYPEBITS)
#define VCAM_IOC_DIRSHIFT  (VCAM_IOC_SIZESHIFT + VCAM_IOC_SIZEBITS)
#define VCAM_IOC_NONE      0u
#define VCAM_IOC_WRITE     1u
#define VCAM_IOC_READ      2u
#define VCAM_IOC(dir, type, nr, size)                                  \
	(((dir) << VCAM_IOC_DIRSHIFT) | ((type) << VCAM_IOC_TYPESHIFT) |   \
	 ((nr) << VCAM_IOC_NRSHIFT) | ((size) << VCAM_IOC_SIZESHIFT))
#define VCAM_IO(type, nr)     VCAM_IOC(VCAM_IOC_NONE, (type), (nr), 0)
#define VCAM_IOR(type, nr, t) VCAM_IOC(VCAM_IOC_READ, (type), (nr), sizeof(t))
#define VCAM_IOW(type, nr, t) VCAM_IOC(VCAM_IOC_WRITE, (type), (nr), sizeof(t))
#define VCAM_IOWR(type, nr, t) \
	VCAM_IOC(VCAM_IOC_READ | VCAM_IOC_WRITE, (type), (nr), sizeof(t))

/* control plane -- replaces the sample's Binder transactions */
#define VCAM_IOC_GET_VERSION    VCAM_IOR(VCAM_IOC_MAGIC, 0x00, struct vcam_version)
#define VCAM_IOC_REGISTER_BUF   VCAM_IOWR(VCAM_IOC_MAGIC, 0x01, struct vcam_frame)
#define VCAM_IOC_UNREGISTER_BUF VCAM_IOW(VCAM_IOC_MAGIC, 0x02, vcam_u32)
#define VCAM_IOC_SUBMIT         VCAM_IOW(VCAM_IOC_MAGIC, 0x03, struct vcam_frame)
#define VCAM_IOC_DEQUEUE        VCAM_IOR(VCAM_IOC_MAGIC, 0x04, struct vcam_dequeue)
#define VCAM_IOC_RELEASE        VCAM_IOW(VCAM_IOC_MAGIC, 0x05, vcam_u64)
#define VCAM_IOC_SET_RANGE      VCAM_IOW(VCAM_IOC_MAGIC, 0x06, struct vcam_range)
#define VCAM_IOC_SET_POSE       VCAM_IOW(VCAM_IOC_MAGIC, 0x07, struct vcam_pose)
#define VCAM_IOC_GET_STATS      VCAM_IOR(VCAM_IOC_MAGIC, 0x08, struct vcam_stats)
#define VCAM_IOC_POLL           VCAM_IOR(VCAM_IOC_MAGIC, 0x09, struct vcam_poll)
#define VCAM_IOC_MAXNR          0x09

#endif /* VCAM_UAPI_H */
