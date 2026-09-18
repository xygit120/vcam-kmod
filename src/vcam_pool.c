// SPDX-License-Identifier: GPL-2.0
/*
 * vcam_pool.c -- the dma-buf frame pool and its ownership state machine.
 *
 * This is the kernel-side replacement for the sample's tx50 frame sink: instead
 * of copying packed YUV420 into a client-supplied IMemory (FUN_00542080), the
 * module keeps the producer's dma-buf and only rebinds the reference, so no
 * pixel ever passes through the CPU a second time.
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/dma-buf.h>
#include <linux/string.h>

#include "vcam_kmod.h"

static struct vcam_slot *vcam_find_held(struct vcam_pool *pool, u64 seq)
{
	u32 i;

	for (i = 0; i < pool->nslots; i++) {
		if (pool->slot[i].state == VCAM_SLOT_HELD &&
		    pool->slot[i].seq == seq)
			return &pool->slot[i];
	}
	return NULL;
}

int vcam_pool_init(struct vcam_pool *pool, u32 nslots)
{
	if (pool == NULL)
		return -EINVAL;
	if (nslots == 0 || nslots > VCAM_MAX_SLOTS)
		return -EINVAL;

	memset(pool, 0, sizeof(*pool));
	mutex_init(&pool->lock);
	pool->nslots = nslots;
	pool->backend = VCAM_BACKEND_BRIDGE;
	pool->range_begin_ns = 0;
	pool->range_end_ns = -1;	/* to end of stream */
	pool->loop = 1;
	return 0;
}

void vcam_pool_destroy(struct vcam_pool *pool)
{
	u32 i;

	if (pool == NULL)
		return;

	mutex_lock(&pool->lock);
	for (i = 0; i < pool->nslots; i++) {
		if (pool->slot[i].dbuf != NULL) {
			dma_buf_put(pool->slot[i].dbuf);
			pool->slot[i].dbuf = NULL;
		}
		pool->slot[i].state = VCAM_SLOT_UNBOUND;
		pool->slot[i].fd = -1;
	}
	mutex_unlock(&pool->lock);
	mutex_destroy(&pool->lock);
}

int vcam_pool_register(struct vcam_pool *pool, int fd,
		       const struct vcam_format *fmt, u32 *slot_out)
{
	struct dma_buf *dbuf;
	struct vcam_slot *s;
	u32 i;
	int free_slot = -1;
	int rc;

	if (pool == NULL || fmt == NULL || slot_out == NULL)
		return -EINVAL;
	if (fd < 0)
		return -EINVAL;

	rc = vcam_format_validate(fmt);
	if (rc != 0)
		return rc;

	dbuf = dma_buf_get(fd);
	if (IS_ERR(dbuf))
		return (int)PTR_ERR(dbuf);

	mutex_lock(&pool->lock);
	for (i = 0; i < pool->nslots; i++) {
		if (pool->slot[i].state == VCAM_SLOT_UNBOUND) {
			free_slot = (int)i;
			break;
		}
	}
	if (free_slot < 0) {
		mutex_unlock(&pool->lock);
		dma_buf_put(dbuf);
		return -EBUSY;
	}

	s = &pool->slot[free_slot];
	s->dbuf = dbuf;
	s->fd = fd;
	s->seq = 0;
	s->timestamp_ns = 0;
	s->state = VCAM_SLOT_IDLE;
	s->fmt = *fmt;

	/* tx50 also published the geometry, and the format only when it is not
	 * one the sample already knows. */
	pool->reported_w = fmt->width;
	pool->reported_h = fmt->height;
	if (vcam_format_reportable(fmt->fourcc))
		pool->reported_format = fmt->fourcc;

	*slot_out = (u32)free_slot;
	mutex_unlock(&pool->lock);
	return 0;
}

int vcam_pool_unregister(struct vcam_pool *pool, u32 slot)
{
	struct vcam_slot *s;

	if (pool == NULL)
		return -EINVAL;

	mutex_lock(&pool->lock);
	if (slot >= pool->nslots) {
		pool->stats.rejected_unknown++;
		mutex_unlock(&pool->lock);
		return -ENOENT;
	}
	s = &pool->slot[slot];
	if (s->state == VCAM_SLOT_FILLED || s->state == VCAM_SLOT_HELD) {
		/* The pipeline may still dereference this buffer. */
		pool->stats.rejected_busy++;
		mutex_unlock(&pool->lock);
		return -EBUSY;
	}
	if (s->state == VCAM_SLOT_UNBOUND) {
		mutex_unlock(&pool->lock);
		return -EINVAL;
	}
	dma_buf_put(s->dbuf);
	s->dbuf = NULL;
	s->fd = -1;
	s->seq = 0;
	s->state = VCAM_SLOT_UNBOUND;
	mutex_unlock(&pool->lock);
	return 0;
}

int vcam_pool_submit(struct vcam_pool *pool, u32 slot, u64 seq,
		     s64 timestamp_ns, u32 rotation_deg, u32 flags)
{
	struct vcam_slot *s;

	if (pool == NULL)
		return -EINVAL;

	mutex_lock(&pool->lock);
	if (slot >= pool->nslots || pool->slot[slot].state == VCAM_SLOT_UNBOUND) {
		pool->stats.rejected_unknown++;
		mutex_unlock(&pool->lock);
		return -ENOENT;
	}
	s = &pool->slot[slot];
	if (s->state != VCAM_SLOT_IDLE) {
		/* FILLED: the consumer has not taken the previous frame yet.
		 * HELD  : the consumer is reading this very buffer right now.
		 * Both are refused; the second case is the tearing guard. */
		pool->stats.rejected_busy++;
		mutex_unlock(&pool->lock);
		return -EBUSY;
	}
	if (seq <= pool->last_submitted_seq) {
		pool->stats.rejected_unknown++;
		mutex_unlock(&pool->lock);
		return -EINVAL;
	}

	s->seq = seq;
	s->timestamp_ns = timestamp_ns;
	s->rotation_deg = rotation_deg;
	s->flags = flags;
	/* Pose is global state in the sample: state+0xac (angle), state+0xb0
	 * (mirror) and state+0xa8 (auto orientation) are read when the frame is
	 * rendered, not carried per frame. So it is resolved here from the pool's
	 * pose and the consumer only has to apply the result. The per-frame
	 * rotation_deg/flags from SUBMIT are recorded but deliberately not folded
	 * in, which keeps the port faithful to the sample. */
	vcam_pose_apply(&pool->pose, &s->resolved_angle, &s->resolved_mirror);
	s->state = VCAM_SLOT_FILLED;
	pool->last_submitted_seq = seq;
	pool->stats.submitted++;
	mutex_unlock(&pool->lock);
	return 0;
}

/* Shared core of acquire/dequeue. `take_ref` distinguishes the in-kernel
 * consumer (which gets an extra dma_buf reference and must put it) from the
 * userspace bridge (which already owns the fd it registered, so it gets the
 * slot index instead). */
static int vcam_pool_take(struct vcam_pool *pool, bool take_ref, u64 *seq_out,
			  u32 *slot_out, struct dma_buf **ref_out)
{
	struct vcam_slot *best = NULL;
	struct dma_buf *ref;
	u32 i;

	if (pool == NULL || seq_out == NULL)
		return -EINVAL;

	mutex_lock(&pool->lock);
	for (i = 0; i < pool->nslots; i++) {
		struct vcam_slot *s = &pool->slot[i];

		if (s->state != VCAM_SLOT_FILLED)
			continue;
		if (best == NULL || s->seq < best->seq)
			best = s;
	}
	if (best == NULL) {
		pool->stats.dequeued_empty++;
		mutex_unlock(&pool->lock);
		return -EAGAIN;
	}

	/* Range gate, ported from the worker's
	 *   state+0x50 == 0 || state+0x60 == -1 || pts <= state+0x60
	 * test. A frame past the end is either skipped (loop off) or delivered
	 * with a wrap recorded (loop on); seeking the decoder back to
	 * range_begin_ns is userspace's job because only it owns the decoder. */
	if (vcam_range_decide(pool, best->timestamp_ns) == VCAM_RANGE_DROP) {
		best->state = VCAM_SLOT_IDLE;
		best->seq = 0;
		pool->stats.dropped_late++;
		mutex_unlock(&pool->lock);
		return -EAGAIN;
	}
	if (pool->range_end_ns != -1 &&
	    best->timestamp_ns >= pool->range_end_ns && pool->loop) {
		pool->wraps_due++;
		pool->stats.looped++;
	}

	ref = NULL;
	if (take_ref) {
		ref = dma_buf_get(best->fd);
		if (IS_ERR(ref)) {
			mutex_unlock(&pool->lock);
			return (int)PTR_ERR(ref);
		}
	}

	best->state = VCAM_SLOT_HELD;
	pool->last_dequeued_seq = best->seq;
	pool->active_seq = best->seq;
	*seq_out = best->seq;
	if (slot_out != NULL)
		*slot_out = (u32)(best - pool->slot);
	if (ref_out != NULL)
		*ref_out = ref;
	pool->stats.dequeued++;
	pool->stats.bound++;
	mutex_unlock(&pool->lock);
	return 0;
}

int vcam_pool_acquire(struct vcam_pool *pool, u64 *seq_out, struct dma_buf **out)
{
	if (out == NULL)
		return -EINVAL;
	return vcam_pool_take(pool, true, seq_out, NULL, out);
}

int vcam_pool_dequeue(struct vcam_pool *pool, struct vcam_dequeue *dq)
{
	u64 seq = 0;
	u32 slot = 0;
	int rc;

	if (dq == NULL)
		return -EINVAL;
	rc = vcam_pool_take(pool, false, &seq, &slot, NULL);
	if (rc != 0)
		return rc;

	dq->seq = seq;
	dq->slot = slot;
	dq->resolved_angle_deg = pool->slot[slot].resolved_angle;
	dq->resolved_mirror = pool->slot[slot].resolved_mirror;
	dq->reserved = 0;
	return 0;
}

int vcam_pool_release(struct vcam_pool *pool, u64 seq)
{
	struct vcam_slot *s;

	if (pool == NULL)
		return -EINVAL;

	mutex_lock(&pool->lock);
	s = vcam_find_held(pool, seq);
	if (s == NULL) {
		/* Unknown, or never acquired: a caller must not "release" a
		 * buffer the consumer never held. */
		pool->stats.rejected_unknown++;
		mutex_unlock(&pool->lock);
		return -ESRCH;
	}
	s->state = VCAM_SLOT_IDLE;
	pool->active_seq = 0;
	pool->stats.released++;
	mutex_unlock(&pool->lock);
	return 0;
}

int vcam_pool_peek(struct vcam_pool *pool, struct dma_buf **out, u64 *seq_out)
{
	struct vcam_slot *best = NULL;
	u32 i;

	if (pool == NULL || out == NULL || seq_out == NULL)
		return -EINVAL;

	mutex_lock(&pool->lock);
	for (i = 0; i < pool->nslots; i++) {
		struct vcam_slot *s = &pool->slot[i];

		if (s->state != VCAM_SLOT_FILLED)
			continue;
		if (best == NULL || s->seq < best->seq)
			best = s;
	}
	if (best == NULL) {
		mutex_unlock(&pool->lock);
		return -EAGAIN;
	}
	*seq_out = best->seq;
	*out = best->dbuf;	/* borrowed: do not dma_buf_put() */
	mutex_unlock(&pool->lock);
	return 0;
}

int vcam_pool_poll(struct vcam_pool *pool, struct vcam_poll *poll)
{
	if (pool == NULL || poll == NULL)
		return -EINVAL;

	mutex_lock(&pool->lock);
	memset(poll, 0, sizeof(*poll));
	poll->client_attached = pool->client_attached;

	/* tx13: report width/height only when they changed since the previous
	 * poll, then clear the format field. */
	if (pool->reported_w != pool->last_polled_w) {
		pool->last_polled_w = pool->reported_w;
		poll->width_delta = pool->reported_w;
	}
	if (pool->reported_h != pool->last_polled_h) {
		pool->last_polled_h = pool->reported_h;
		poll->height_delta = pool->reported_h;
	}
	poll->format = pool->reported_format;
	pool->reported_format = 0;
	mutex_unlock(&pool->lock);
	return 0;
}

int vcam_pool_set_range(struct vcam_pool *pool, const struct vcam_range *range)
{
	if (pool == NULL || range == NULL)
		return -EINVAL;
	/* end == -1 means "to the end of the stream"; otherwise the range must
	 * be ordered. This mirrors FUN_00541f7c, which stores whatever it is
	 * given but is only ever called with (0, -1) or a real sub-range. */
	if (range->end_ns != -1 && range->end_ns < range->begin_ns)
		return -EINVAL;
	if (range->begin_ns < 0)
		return -EINVAL;

	mutex_lock(&pool->lock);
	pool->range_begin_ns = range->begin_ns;
	pool->range_end_ns = range->end_ns;
	pool->loop = range->loop ? 1u : 0u;
	mutex_unlock(&pool->lock);
	return 0;
}

int vcam_pool_set_pose(struct vcam_pool *pool, const struct vcam_pose *pose)
{
	if (pool == NULL || pose == NULL)
		return -EINVAL;
	if (pose->flags & ~(VCAM_F_MIRROR | VCAM_F_AUTOPOSE))
		return -EINVAL;

	mutex_lock(&pool->lock);
	pool->pose = *pose;
	mutex_unlock(&pool->lock);
	return 0;
}

int vcam_pool_set_backend(struct vcam_pool *pool, u32 backend)
{
	if (pool == NULL)
		return -EINVAL;
	if (backend != VCAM_BACKEND_BRIDGE && backend != VCAM_BACKEND_PIPELINE)
		return -EINVAL;

	mutex_lock(&pool->lock);
	pool->backend = backend;
	mutex_unlock(&pool->lock);
	return 0;
}

int vcam_pool_attach(struct vcam_pool *pool)
{
	if (pool == NULL)
		return -EINVAL;
	mutex_lock(&pool->lock);
	pool->client_attached = 1;
	mutex_unlock(&pool->lock);
	return 0;
}

int vcam_pool_get_stats(struct vcam_pool *pool, struct vcam_stats *out)
{
	if (pool == NULL || out == NULL)
		return -EINVAL;
	mutex_lock(&pool->lock);
	*out = pool->stats;
	mutex_unlock(&pool->lock);
	return 0;
}
