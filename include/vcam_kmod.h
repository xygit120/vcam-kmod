/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vcam_kmod.h -- internal API of the vcam kernel module.
 *
 * This module is the kernel-side half of the 18.0.apk port. It implements the
 * part of that sample which is genuinely kernel-portable:
 *
 *   * a dma-buf frame pool with the ownership rules that prevent tearing
 *     (the sample keeps the same information in IMemory + its state object);
 *   * the pose rule (auto orientation = 270 - media rotation + forced mirror);
 *   * the play-range / loop decision that decides whether a decoded frame is
 *     delivered, skipped or wrapped;
 *   * the format / plane-geometry validation the sample performs before it
 *     touches a buffer;
 *   * a control plane that replaces the sample's Binder transactions (11..25,
 *     50, 51) with ioctls;
 *   * the tx13-shaped delta poll.
 *
 * What it deliberately does NOT do: decode. The sample decodes with FFmpeg and
 * AMediaCodec in userspace, and no kernel equivalent exists, so userspace keeps
 * decoding into dma-heap buffers and this module only rebinds them.
 */
#ifndef VCAM_KMOD_H
#define VCAM_KMOD_H

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/dma-buf.h>
#include <linux/fs.h>

#include "vcam_uapi.h"

#define VCAM_DRIVER_NAME "vcam"
#define VCAM_MAX_SLOTS   8u

/* Backend selection (VCAM_IOC_BIND_BACKEND). */
#define VCAM_BACKEND_BRIDGE   0u /* frames are handed out via DEQUEUE/RELEASE */
#define VCAM_BACKEND_PIPELINE 1u /* a device glue layer binds frames itself */

enum vcam_slot_state {
	VCAM_SLOT_UNBOUND = 0,
	VCAM_SLOT_IDLE = 1,
	VCAM_SLOT_FILLED = 2,
	VCAM_SLOT_HELD = 3,
};

/* errno-style results used by the pool. */
#define VCAM_OK        0
#define VCAM_ERR_BUSY  1
#define VCAM_ERR_AGAIN 2
#define VCAM_ERR_LOOKUP 3
#define VCAM_ERR_INVAL 4

struct vcam_slot {
	struct dma_buf *dbuf;
	int fd;				/* kept so an extra reference can be re-taken */
	u64 seq;
	s64 timestamp_ns;
	u32 state;
	u32 rotation_deg;
	u32 flags;
	u32 resolved_angle;		/* pose applied when this frame was queued */
	u32 resolved_mirror;
	struct vcam_format fmt;
};

struct vcam_pool {
	struct mutex lock;
	struct vcam_slot slot[VCAM_MAX_SLOTS];
	u32 nslots;
	u32 backend;
	u64 last_submitted_seq;
	u64 last_dequeued_seq;
	u64 active_seq;			/* frame currently bound into the pipeline */
	s64 range_begin_ns;
	s64 range_end_ns;		/* -1 == to end of stream */
	u32 loop;
	u32 client_attached;
	struct vcam_pose pose;
	/* tx13-shaped delta poll state */
	u32 reported_w;
	u32 reported_h;
	u32 reported_format;
	u32 last_polled_w;
	u32 last_polled_h;
	u64 wraps_due;			/* range end reached with loop enabled */
	struct vcam_stats stats;
};

int vcam_pool_init(struct vcam_pool *pool, u32 nslots);
void vcam_pool_destroy(struct vcam_pool *pool);

int vcam_pool_register(struct vcam_pool *pool, int fd,
		       const struct vcam_format *fmt, u32 *slot_out);
int vcam_pool_unregister(struct vcam_pool *pool, u32 slot);
int vcam_pool_submit(struct vcam_pool *pool, u32 slot, u64 seq, s64 timestamp_ns,
		     u32 rotation_deg, u32 flags);

/* In-kernel consumer API: a device glue layer calls acquire() when it needs the
 * next frame to bind, and release() when the hardware/consumer is done with it.
 *
 * Reference ownership, which is the whole contract:
 *   - the pool holds ONE dma_buf reference per registered slot, for the slot's
 *     lifetime (taken by dma_buf_get() in vcam_pool_register);
 *   - acquire() hands the caller an ADDITIONAL reference (another dma_buf_get)
 *     which the caller must dma_buf_put() when the hardware is finished;
 *   - release() only moves the slot back to IDLE; it never drops a reference.
 * This is why a buffer cannot be re-filled while HELD: the producer would be
 * writing pixels the pipeline may still be reading. */
int vcam_pool_acquire(struct vcam_pool *pool, u64 *seq_out, struct dma_buf **out);
int vcam_pool_release(struct vcam_pool *pool, u64 seq);

/* Userspace bridge variant of acquire: it only moves the slot to HELD and tells
 * the caller which slot it is plus the pose resolved for that frame. No
 * additional kernel reference is taken, because the userspace side already owns
 * the fd it registered. */
int vcam_pool_dequeue(struct vcam_pool *pool, struct vcam_dequeue *dq);

/* Copies the newest complete frame's descriptor without transferring an
 * ownership step; used by the pipeline backend to peek before binding. */
int vcam_pool_peek(struct vcam_pool *pool, struct dma_buf **out, u64 *seq_out);

/* tx13-shaped delta poll. Fills *poll and clears the format field. */
int vcam_pool_poll(struct vcam_pool *pool, struct vcam_poll *poll);

/* Control-plane setters, replacing the sample's Binder transactions. */
int vcam_pool_set_range(struct vcam_pool *pool, const struct vcam_range *range);
int vcam_pool_set_pose(struct vcam_pool *pool, const struct vcam_pose *pose);
int vcam_pool_set_backend(struct vcam_pool *pool, u32 backend);
int vcam_pool_attach(struct vcam_pool *pool);
int vcam_pool_get_stats(struct vcam_pool *pool, struct vcam_stats *out);

/* ------------------------------------------------------------------ */
/* Module entry points (vcam_main.c) and the control-plane entry used   */
/* by the misc device (vcam_ctrl.c).                                    */
/* ------------------------------------------------------------------ */
struct vcam_pool *vcam_pool_singleton(void);
long vcam_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
int vcam_open(struct inode *inode, struct file *filp);
int vcam_release(struct inode *inode, struct file *filp);

/* ------------------------------------------------------------------ */
/* Pure logic, ported from the sample and unit-tested on the host       */
/* ------------------------------------------------------------------ */

/* FUN_005415b4 pose resolution. Returns nothing; fills *angle and *mirror. */
void vcam_pose_apply(const struct vcam_pose *pose, u32 *angle_deg, u32 *mirror);

/* Play-range / loop decision for an incoming frame timestamp.
 * Returns VCAM_RANGE_DELIVER, VCAM_RANGE_WRAP or VCAM_RANGE_DROP. */
#define VCAM_RANGE_DELIVER 0
#define VCAM_RANGE_WRAP    1
#define VCAM_RANGE_DROP    2
int vcam_range_decide(const struct vcam_pool *pool, s64 timestamp_ns);

/* The sample only publishes a format it does not already recognise
 * (DAT_00c79c08 is set unless the format is one of the known set). */
bool vcam_format_reportable(u32 fourcc);

/* Plane geometry sanity, i.e. the checks the sample performs before it writes a
 * buffer: stride must cover the width and the chroma planes must fit. */
int vcam_format_validate(const struct vcam_format *fmt);

#endif /* VCAM_KMOD_H */
