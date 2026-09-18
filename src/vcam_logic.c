// SPDX-License-Identifier: GPL-2.0
/*
 * vcam_logic.c -- the pure decision logic ported from 18.0.apk.
 *
 * Everything in this file is a direct port of behaviour recovered from the
 * sample and re-verified byte-wise by
 * work/18-0-equivalent-source/verify_model.py:
 *
 *   vcam_pose_apply       <- FUN_005415b4 (file 0x4415b4), pose block
 *   vcam_range_decide     <- the play-range/loop branch of the same function
 *   vcam_format_reportable<- FUN_00540c98 + FUN_00540324 (DAT_00c79c08 gating)
 *   vcam_format_validate  <- the geometry FTUN_00542080 assumes when it copies
 */

#include <linux/kernel.h>
#include <linux/errno.h>

#include "vcam_kmod.h"

/* The sample accepts only these four explicit angles; anything else becomes 0
 * and sets the shared state+0xa8 byte (tx18). */
static u32 vcam_normalize_explicit_angle(u32 deg)
{
	switch (deg) {
	case 0:
	case 90:
	case 180:
	case 270:
		return deg;
	default:
		return 0;
	}
}

/* The auto-orientation branch computes 270 - media_rotation with no validation
 * of its own (the sample feeds the raw value to its renderer). The port keeps
 * the same arithmetic but brings the result into [0,360) so a backend's
 * transform cannot be handed an out-of-range angle. */
static u32 vcam_wrap_angle_360(s32 deg)
{
	s32 v = deg % 360;

	if (v < 0)
		v += 360;
	return (u32)v;
}

void vcam_pose_apply(const struct vcam_pose *pose, u32 *angle_deg, u32 *mirror)
{
	if (pose == NULL || angle_deg == NULL || mirror == NULL)
		return;

	if (pose->flags & VCAM_F_AUTOPOSE) {
		/* 0x441788: mov w9,#0x10e ; 0x441790: sub w2,w9,w8
		 * 0x44178c: mov w3,#1   (mirroring forced on) */
		*angle_deg = vcam_wrap_angle_360(270 - pose->media_rotation_deg);
		*mirror = 1u;
	} else {
		/* 0x4418d4: ldr w2,[x24,0xac] ; 0x4418d0: ldrb w8,[x24,0xb0] ;
		 * 0x4418dc: cset w3,ne */
		*angle_deg = vcam_normalize_explicit_angle(pose->rotation_deg);
		*mirror = (pose->flags & VCAM_F_MIRROR) ? 1u : 0u;
	}
}

int vcam_range_decide(const struct vcam_pool *pool, s64 timestamp_ns)
{
	/* The sample renders while
	 *     state+0x50 == 0 || state+0x60 == -1 || pts <= state+0x60
	 * and then either rewinds to state+0x58 (loop, state+0x98 set) or waits
	 * for a seek. A kernel module must not block a producer, so the "wait"
	 * case is reported as DROP and userspace decides what to do.
	 *
	 * Note the sample additionally gates the whole range mechanism on
	 * state+0x18 == 1 (a local file). In the port that decision belongs to
	 * userspace: it simply does not issue VCAM_IOC_SET_RANGE for a live
	 * network source. */
	if (pool->range_end_ns == -1)
		return VCAM_RANGE_DELIVER;	/* -1 == to end of stream */
	if (timestamp_ns <= pool->range_end_ns)
		return VCAM_RANGE_DELIVER;
	return pool->loop ? VCAM_RANGE_WRAP : VCAM_RANGE_DROP;
}

bool vcam_format_reportable(u32 fourcc)
{
	/* FUN_00540c98 publishes the tx50 format into DAT_00c79c08 only when
	 *   ((0x23 < fmt) || ((1 << (fmt & 0x3f)) & 0xe00020000) == 0)
	 *   && fmt != 0x32315659 && fmt != 0x7fa30c06
	 * The mask covers 17 (YCrCb_420_SP) plus 33/34/35 (BLOB,
	 * IMPLEMENTATION_DEFINED, YCbCr_420_888). */
	if (fourcc == 0x32315659u || fourcc == 0x7fa30c06u)
		return false;
	if (fourcc > 0x23u)
		return true;
	return ((1ull << fourcc) & 0xE00020000ull) == 0;
}

int vcam_format_validate(const struct vcam_format *fmt)
{
	u32 i;

	if (fmt == NULL)
		return -EINVAL;
	if (fmt->width == 0 || fmt->height == 0)
		return -EINVAL;
	if (fmt->width > 32768u || fmt->height > 32768u)
		return -EINVAL;
	if (fmt->num_planes == 0 || fmt->num_planes > VCAM_MAX_PLANES)
		return -EINVAL;

	for (i = 0; i < fmt->num_planes; i++) {
		u32 need_w;

		/* The sample halves with a sign-corrected arithmetic shift
		 * (cinc + asr), i.e. floor(width/2) and floor(height/2) for
		 * the chroma planes. */
		need_w = (i == 0) ? fmt->width : (fmt->width / 2u);
		if (need_w == 0)
			return -EINVAL;
		if (fmt->plane[i].stride < need_w)
			return -EINVAL;
	}

	/* PSEUDOCODE/STUB: the sample never validates the buffer size for the
	 * tx50 sink (it trusts the client). The backend must additionally check
	 * the plane extents against the dma-buf size it has pinned. */
	return 0;
}
