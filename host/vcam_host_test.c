// SPDX-License-Identifier: GPL-2.0
/*
 * vcam_host_test.c -- offline harness for the vcam kernel module.
 *
 * This host has no kernel tree and no aarch64 cross compiler, so a real
 * `make modules` cannot run here. The harness instead compiles the *module
 * sources themselves* (not a reimplementation) against shim/ and executes them,
 * covering what the port actually consists of: the ownership state machine, the
 * pose rule, the range/loop gate, the format gating and the ioctl marshalling.
 *
 * Build and run:
 *     gcc -std=gnu11 -DVCAM_HOST_BUILD=1 -Wall -Wextra -Werror \
 *         -Ishim -Iinclude host/vcam_host_test.c -o build/vcam_host_test
 *     build/vcam_host_test
 *
 * Each phase uses its own pool so no phase can be accidentally validated by
 * state left behind by another one. Note in particular that the pose is
 * resolved when a frame is *submitted*, exactly like the sample reads
 * state+0xac / +0xb0 / +0xa8 at render time; a pose set after submit does not
 * retroactively change a queued frame, and the tests assert that ordering.
 */

#define VCAM_HOST_BUILD 1

/* <windows.h> (via <winioctl.h>) also defines _IOW/_IOR, so it must be included
 * before the shim, whose own definitions then supersede the platform ones. */
#if defined(_WIN32)
#include <windows.h>
#endif

#include "../shim/vcam_shim_kernel.h"
#include "../include/vcam_uapi.h"

#include "../src/vcam_logic.c"
#include "../src/vcam_pool.c"
#include "../src/vcam_ctrl.c"
#include "../src/vcam_main.c"

/*
 * The module's ioctl entry keeps the kernel signature, where the third
 * parameter is `unsigned long` and therefore pointer-sized. On this Win64 host
 * `unsigned long` is only 32 bits, so a host pointer passed through it would be
 * truncated -- a limitation of the host harness, not of the module (the kernel
 * is LP64). To exercise the real entry point anyway, the test data for that
 * phase is allocated below 4 GiB so the round trip survives.
 */
static void *host_low_alloc(size_t size)
{
#if defined(_WIN32)
	/* Try a few candidate bases below 4 GiB; the first free one wins. */
	static const uintptr_t hints[] = {
		0x10000u, 0x1000000u, 0x20000000u, 0x30000000u, 0x50000000u,
	};
	size_t i;

	for (i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
		void *p = VirtualAlloc((void *)hints[i], size,
				       MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

		if (p != NULL) return p;
	}
	return NULL;
#else
	(void)size;
	return malloc(size);
#endif
}

static int g_checks;
static int g_failures;

static void expect(int cond, const char *what)
{
	g_checks++;
	if (!cond) {
		g_failures++;
		printf("FAIL %s\n", what);
	}
}

static void expect_eq(long got, long want, const char *what)
{
	g_checks++;
	if (got != want) {
		g_failures++;
		printf("FAIL %s :: got %ld want %ld\n", what, got, want);
	}
}

static struct vcam_format make_fmt(u32 w, u32 h, u32 fourcc, u32 stride)
{
	struct vcam_format f;

	memset(&f, 0, sizeof(f));
	f.width = w;
	f.height = h;
	f.fourcc = fourcc;
	f.num_planes = 3;
	f.plane[0].stride = stride;
	f.plane[1].stride = stride / 2;
	f.plane[2].stride = stride / 2;
	return f;
}

/* ------------------------------------------------------------------ */

static void phase_abi(void)
{
	/* The uapi rolls its own _IOC encoding so userspace and the module agree
	 * even where <linux/ioctl.h> differs; assert it matches the kernel's. */
	expect_eq(VCAM_IOC_SUBMIT,
		  (long)_IOW(VCAM_IOC_MAGIC, 0x03, struct vcam_frame),
		  "uapi _IOW encoding matches the kernel layout");
	expect_eq(VCAM_IOC_GET_VERSION,
		  (long)_IOR(VCAM_IOC_MAGIC, 0x00, struct vcam_version),
		  "uapi _IOR encoding matches the kernel layout");
	expect_eq(VCAM_IOC_POLL, (long)_IOR(VCAM_IOC_MAGIC, 0x09, struct vcam_poll),
		  "poll encoding matches");
	expect_eq((long)_IOC_DIR(VCAM_IOC_SUBMIT), (long)_IOC_WRITE, "submit is a write ioctl");
	expect_eq((long)_IOC_NR(VCAM_IOC_DEQUEUE), 0x04L, "dequeue nr is 4");
	expect_eq((long)_IOC_TYPE(VCAM_IOC_POLL), (long)VCAM_IOC_MAGIC, "ioctl type is 'v'");
	expect(_IOC_NR(VCAM_IOC_POLL) <= VCAM_IOC_MAXNR, "poll is inside the accepted range");
	expect(VCAM_UAPI_ABI_VERSION == 1u, "abi version is 1");
	expect(sizeof(struct vcam_frame) % 8 == 0, "vcam_frame stays 8-byte aligned");
}

static void phase_module_init(void)
{
	struct vcam_pool *p;

	expect_eq(vcam_shim_module_init(), 0, "module init succeeds");
	p = vcam_pool_singleton();
	expect(p != NULL, "singleton is available after init");
	if (p != NULL) {
		expect(p->backend == VCAM_BACKEND_BRIDGE, "default backend is the bridge");
		expect(p->range_end_ns == -1, "range defaults to end-of-stream");
		expect(p->loop == 1, "looping defaults on");
		expect(p->nslots == VCAM_DEFAULT_SLOTS, "default slot count applied");
	}
	vcam_shim_module_exit();
	expect(vcam_pool_singleton() == NULL, "singleton is gone after exit");
}

static void phase_register_and_poll(void)
{
	struct vcam_pool pool;
	struct vcam_format fmt, bad;
	struct vcam_poll poll;
	u32 slot = 0, slot_b = 0;

	expect_eq(vcam_pool_init(NULL, 1), -EINVAL, "pool_init rejects NULL");
	expect_eq(vcam_pool_init(&pool, 0), -EINVAL, "pool_init rejects 0 slots");
	expect_eq(vcam_pool_init(&pool, VCAM_MAX_SLOTS + 1), -EINVAL,
		  "pool_init rejects too many slots");
	expect_eq(vcam_pool_init(&pool, 2), 0, "pool_init accepts 2 slots");

	vcam_shim_register_fd(10, 4096);
	vcam_shim_register_fd(11, 4096);
	vcam_shim_register_fd(12, 4096);
	fmt = make_fmt(64, 64, 0x23, 64);	/* 0x23 is a known format */

	expect_eq(vcam_pool_register(&pool, -1, &fmt, &slot), -EINVAL,
		  "register rejects a negative fd");
	expect_eq(vcam_pool_register(&pool, 30, &fmt, &slot), -ENOENT,
		  "register rejects an unpinned fd");
	bad = make_fmt(64, 64, 0x23, 8);
	expect_eq(vcam_pool_register(&pool, 10, &bad, &slot), -EINVAL,
		  "register rejects a stride narrower than the width");
	bad = make_fmt(0, 64, 0x23, 64);
	expect_eq(vcam_pool_register(&pool, 10, &bad, &slot), -EINVAL,
		  "register rejects a zero width");
	bad = make_fmt(64, 64, 0x23, 64);
	bad.num_planes = VCAM_MAX_PLANES + 1;
	expect_eq(vcam_pool_register(&pool, 10, &bad, &slot), -EINVAL,
		  "register rejects too many planes");
	expect_eq(vcam_pool_register(&pool, 10, &fmt, &slot), 0, "register fd 10");
	expect_eq(vcam_pool_register(&pool, 11, &fmt, &slot_b), 0, "register fd 11");
	expect(slot != slot_b, "the two registrations get different slots");
	expect_eq(vcam_pool_register(&pool, 12, &fmt, &slot), -EBUSY,
		  "a third register exhausts the pool");
	expect_eq((long)vcam_shim_fd_refcnt(10), 1L, "the pool holds one reference per slot");
	expect_eq((long)vcam_shim_fd_refcnt(12), 0L, "the rejected fd was not pinned");

	/* tx13-shaped delta poll */
	memset(&poll, 0, sizeof(poll));
	expect_eq(vcam_pool_poll(NULL, &poll), -EINVAL, "poll rejects a NULL pool");
	expect_eq(vcam_pool_poll(&pool, &poll), 0, "poll succeeds");
	expect_eq((long)poll.width_delta, 64L, "poll reports the new width once");
	expect_eq((long)poll.height_delta, 64L, "poll reports the new height once");
	expect_eq((long)poll.format, 0L, "a known format is not published");
	expect_eq(vcam_pool_poll(&pool, &poll), 0, "second poll");
	expect_eq((long)poll.width_delta, 0L, "width is 0 when unchanged");
	expect_eq((long)poll.height_delta, 0L, "height is 0 when unchanged");
	expect_eq((long)poll.client_attached, 0L, "nothing is attached yet");
	expect_eq(vcam_pool_attach(&pool), 0, "attach (the tx51 equivalent)");
	expect_eq(vcam_pool_poll(&pool, &poll), 0, "poll after attach");
	expect_eq((long)poll.client_attached, 1L, "the attach flag is reported");

	/* a vendor format is published once and then cleared */
	expect_eq(vcam_pool_unregister(&pool, slot), 0, "free the first slot");
	fmt = make_fmt(64, 64, 0x7fa30c04, 64);
	expect_eq(vcam_pool_register(&pool, 12, &fmt, &slot), 0,
		  "register with a vendor format");
	expect_eq(vcam_pool_poll(&pool, &poll), 0, "poll after the vendor format");
	expect_eq((long)poll.format, 0x7fa30c04L, "the vendor format is published");
	expect_eq(vcam_pool_poll(&pool, &poll), 0, "poll again");
	expect_eq((long)poll.format, 0L, "the format field is cleared after reading");

	/* YV12 is explicitly excluded from the published set */
	expect_eq(vcam_pool_unregister(&pool, slot), 0, "free the slot again");
	expect_eq(vcam_pool_unregister(&pool, slot_b), 0, "free the second slot");
	fmt = make_fmt(64, 64, 0x32315659, 64);
	expect_eq(vcam_pool_register(&pool, 10, &fmt, &slot), 0, "register YV12");
	expect_eq(vcam_pool_poll(&pool, &poll), 0, "poll after YV12");
	expect_eq((long)poll.format, 0L, "YV12 is never published");

	expect_eq(vcam_pool_unregister(&pool, slot), 0, "cleanup");
	expect_eq((long)vcam_shim_fd_refcnt(10), 0L, "reference balance is restored");
	expect_eq((long)vcam_shim_fd_refcnt(12), 0L, "vendor fd balance is restored");
	vcam_pool_destroy(&pool);
}

static void phase_submit_and_guard(void)
{
	struct vcam_pool pool;
	struct vcam_format fmt = make_fmt(64, 64, 0x23, 64);
	struct dma_buf *ref = NULL;
	u64 seq = 0;
	u32 slot = 0;

	expect_eq(vcam_pool_init(&pool, 1), 0, "1-slot pool");
	vcam_shim_register_fd(20, 4096);
	expect_eq(vcam_pool_register(&pool, 20, &fmt, &slot), 0, "register fd 20");

	expect_eq(vcam_pool_submit(&pool, 99, 1, 0, 0, 0), -ENOENT,
		  "submit on an unknown slot");
	expect_eq(vcam_pool_submit(&pool, slot, 1, 100, 0, 0), 0, "submit seq 1");
	expect_eq(vcam_pool_submit(&pool, slot, 2, 200, 0, 0), -EBUSY,
		  "the same slot cannot be queued twice (backpressure)");

	/* the tearing guard: the consumer holds the buffer */
	{
		struct vcam_dequeue dq;

		expect_eq(vcam_pool_dequeue(&pool, &dq), 0, "consumer takes seq 1");
		expect_eq((long)dq.seq, 1L, "the queued frame comes out");
	}
	expect_eq(vcam_pool_submit(&pool, slot, 3, 300, 0, 0), -EBUSY,
		  "re-fill while the consumer holds the buffer is refused");
	expect_eq(vcam_pool_unregister(&pool, slot), -EBUSY,
		  "unregister while held is refused");
	expect_eq((long)vcam_shim_fd_refcnt(20), 1L, "the held fd keeps its pool reference");

	expect_eq(vcam_pool_release(&pool, 1), 0, "release seq 1");
	expect_eq(vcam_pool_release(&pool, 1), -ESRCH, "releasing twice fails");
	expect_eq(vcam_pool_release(&pool, 777), -ESRCH, "releasing an unknown frame fails");
	expect_eq(vcam_pool_submit(&pool, slot, 3, 300, 0, 0), 0,
		  "the slot accepts a new frame once released");

	/* in-kernel consumer: acquire takes an extra reference */
	expect_eq(vcam_pool_acquire(&pool, &seq, &ref), 0, "kernel consumer acquires");
	expect(ref != NULL && !IS_ERR(ref), "acquire returned a dma-buf reference");
	expect_eq((long)vcam_shim_fd_refcnt(20), 2L, "acquire took an extra reference");
	dma_buf_put(ref);
	expect_eq((long)vcam_shim_fd_refcnt(20), 1L, "the consumer put its reference");
	expect_eq(vcam_pool_release(&pool, seq), 0, "release the acquired frame");
	expect_eq(vcam_pool_acquire(&pool, &seq, &ref), -EAGAIN, "the queue is empty again");
	expect_eq(vcam_pool_peek(&pool, &ref, &seq), -EAGAIN, "peek reports the empty queue");
	expect_eq(vcam_pool_acquire(NULL, &seq, &ref), -EINVAL, "acquire rejects NULL pool");
	expect_eq(vcam_pool_acquire(&pool, &seq, NULL), -EINVAL, "acquire rejects NULL out");

	expect_eq(vcam_pool_unregister(&pool, slot), 0, "unregister the idle slot");
	expect_eq((long)vcam_shim_fd_refcnt(20), 0L, "the pool dropped its reference");
	expect_eq(vcam_pool_unregister(&pool, slot), -EINVAL, "unregistering twice fails");
	expect_eq(vcam_pool_unregister(&pool, 99), -ENOENT, "unregister an unknown slot");
	vcam_pool_destroy(&pool);
}

/* Pose is resolved at submit time, so each case sets the pose, submits a fresh
 * frame and then dequeues it. */
static void pose_case(struct vcam_pool *pool, u32 slot, u64 seq,
		      const struct vcam_pose *p, long want_angle, long want_mirror,
		      const char *what)
{
	struct vcam_dequeue dq;
	char label[128];

	snprintf(label, sizeof(label), "%s :: set pose", what);
	expect_eq(vcam_pool_set_pose(pool, p), 0, label);
	snprintf(label, sizeof(label), "%s :: submit", what);
	expect_eq(vcam_pool_submit(pool, slot, seq, (s64)seq * 100, 0, 0), 0, label);
	snprintf(label, sizeof(label), "%s :: dequeue", what);
	expect_eq(vcam_pool_dequeue(pool, &dq), 0, label);
	snprintf(label, sizeof(label), "%s :: angle", what);
	expect_eq((long)dq.resolved_angle_deg, want_angle, label);
	snprintf(label, sizeof(label), "%s :: mirror", what);
	expect_eq((long)dq.resolved_mirror, want_mirror, label);
	snprintf(label, sizeof(label), "%s :: release", what);
	expect_eq(vcam_pool_release(pool, dq.seq), 0, label);
}

static void phase_pose(void)
{
	struct vcam_pool pool;
	struct vcam_format fmt = make_fmt(64, 64, 0x23, 64);
	struct vcam_pose p;
	u32 slot = 0;

	expect_eq(vcam_pool_init(&pool, 1), 0, "1-slot pool for pose cases");
	vcam_shim_register_fd(30, 4096);
	expect_eq(vcam_pool_register(&pool, 30, &fmt, &slot), 0, "register fd 30");

	memset(&p, 0, sizeof(p));
	p.rotation_deg = 90;
	p.flags = VCAM_F_MIRROR;
	pose_case(&pool, slot, 1, &p, 90, 1, "explicit 90 with mirror");

	memset(&p, 0, sizeof(p));
	p.rotation_deg = 180;
	pose_case(&pool, slot, 2, &p, 180, 0, "explicit 180 without mirror");

	memset(&p, 0, sizeof(p));
	p.rotation_deg = 45;
	pose_case(&pool, slot, 3, &p, 0, 0, "an out-of-range explicit angle becomes 0");

	memset(&p, 0, sizeof(p));
	p.rotation_deg = 270;
	pose_case(&pool, slot, 4, &p, 270, 0, "explicit 270");

	/* auto orientation: 270 - media rotation, mirror forced on */
	memset(&p, 0, sizeof(p));
	p.flags = VCAM_F_AUTOPOSE;
	p.media_rotation_deg = 0;
	pose_case(&pool, slot, 5, &p, 270, 1, "auto pose, media rotation 0");

	memset(&p, 0, sizeof(p));
	p.flags = VCAM_F_AUTOPOSE;
	p.media_rotation_deg = 90;
	pose_case(&pool, slot, 6, &p, 180, 1, "auto pose, media rotation 90");

	memset(&p, 0, sizeof(p));
	p.flags = VCAM_F_AUTOPOSE;
	p.media_rotation_deg = 270;
	pose_case(&pool, slot, 7, &p, 0, 1, "auto pose, media rotation 270");

	/* 270 - 300 = -30, wrapped into [0,360) */
	memset(&p, 0, sizeof(p));
	p.flags = VCAM_F_AUTOPOSE;
	p.media_rotation_deg = 300;
	pose_case(&pool, slot, 8, &p, 330, 1, "auto pose wraps a negative result");

	/* a pose set after submit must not change an already queued frame */
	memset(&p, 0, sizeof(p));
	p.rotation_deg = 90;
	expect_eq(vcam_pool_set_pose(&pool, &p), 0, "pose 90 before submit");
	expect_eq(vcam_pool_submit(&pool, slot, 9, 900, 0, 0), 0, "submit seq 9");
	p.rotation_deg = 270;
	expect_eq(vcam_pool_set_pose(&pool, &p), 0, "pose changed after submit");
	{
		struct vcam_dequeue dq;

		expect_eq(vcam_pool_dequeue(&pool, &dq), 0, "dequeue seq 9");
		expect_eq((long)dq.resolved_angle_deg, 90L,
			  "the queued frame keeps the pose it was submitted with");
		expect_eq(vcam_pool_release(&pool, dq.seq), 0, "release seq 9");
	}

	memset(&p, 0, sizeof(p));
	p.flags = 0x8000u;
	expect_eq(vcam_pool_set_pose(&pool, &p), -EINVAL,
		  "set_pose rejects unknown flag bits");
	expect_eq(vcam_pool_set_pose(&pool, NULL), -EINVAL, "set_pose rejects NULL");

	expect_eq(vcam_pool_unregister(&pool, slot), 0, "cleanup");
	vcam_pool_destroy(&pool);
}

static void phase_range(void)
{
	struct vcam_pool pool;
	struct vcam_format fmt = make_fmt(64, 64, 0x23, 64);
	struct vcam_range range;
	struct vcam_dequeue dq;
	struct vcam_stats stats;
	u32 slot = 0, slot_b = 0;

	expect_eq(vcam_pool_init(&pool, 2), 0, "2-slot pool for range cases");
	vcam_shim_register_fd(40, 4096);
	vcam_shim_register_fd(41, 4096);
	expect_eq(vcam_pool_register(&pool, 40, &fmt, &slot), 0, "register fd 40");
	expect_eq(vcam_pool_register(&pool, 41, &fmt, &slot_b), 0, "register fd 41");

	range.begin_ns = 0;
	range.end_ns = 1000;
	range.loop = 0;
	expect_eq(vcam_pool_set_range(&pool, &range), 0, "range 0..1000, looping off");
	expect_eq(vcam_pool_submit(&pool, slot, 1, 500, 0, 0), 0, "in-range frame");
	expect_eq(vcam_pool_submit(&pool, slot_b, 2, 2000, 0, 0), 0, "late frame queued");
	expect_eq(vcam_pool_dequeue(&pool, &dq), 0, "dequeue");
	expect_eq((long)dq.seq, 1L, "the in-range frame is delivered first");
	expect_eq(vcam_pool_release(&pool, 1), 0, "release seq 1");
	expect_eq(vcam_pool_dequeue(&pool, &dq), -EAGAIN,
		  "the late frame is dropped instead of delivered");
	expect_eq(vcam_pool_get_stats(&pool, &stats), 0, "stats readable");
	expect_eq((long)stats.dropped_late, 1L, "the drop is counted");

	range.loop = 1;
	expect_eq(vcam_pool_set_range(&pool, &range), 0, "looping on");
	expect_eq(vcam_pool_submit(&pool, slot, 3, 2000, 0, 0), 0, "queue another late frame");
	expect_eq(vcam_pool_dequeue(&pool, &dq), 0, "with looping the frame is delivered");
	expect_eq((long)dq.seq, 3L, "the late frame comes out under looping");
	expect_eq(vcam_pool_release(&pool, 3), 0, "release seq 3");
	expect_eq(vcam_pool_get_stats(&pool, &stats), 0, "stats readable");
	expect_eq((long)stats.looped, 1L, "the wrap is counted");
	expect(pool.wraps_due == 1u, "one wrap is due");

	/* end == -1 means "to the end of the stream" and always delivers */
	range.begin_ns = 0;
	range.end_ns = -1;
	expect_eq(vcam_pool_set_range(&pool, &range), 0, "end == -1 accepted");
	expect_eq(vcam_pool_submit(&pool, slot, 4, 999999, 0, 0), 0, "very late frame");
	expect_eq(vcam_pool_dequeue(&pool, &dq), 0, "still delivered");
	expect_eq((long)dq.seq, 4L, "seq 4 delivered");
	expect_eq(vcam_pool_release(&pool, 4), 0, "release seq 4");

	expect_eq(vcam_pool_set_range(&pool, NULL), -EINVAL, "set_range rejects NULL");
	range.begin_ns = 500;
	range.end_ns = 100;
	expect_eq(vcam_pool_set_range(&pool, &range), -EINVAL, "inverted range rejected");
	range.begin_ns = -5;
	range.end_ns = 100;
	expect_eq(vcam_pool_set_range(&pool, &range), -EINVAL, "negative begin rejected");

	expect_eq(vcam_pool_unregister(&pool, slot), 0, "cleanup slot");
	expect_eq(vcam_pool_unregister(&pool, slot_b), 0, "cleanup slot b");
	expect_eq((long)vcam_shim_fd_refcnt(40), 0L, "reference balance restored");
	vcam_pool_destroy(&pool);
}

static void phase_ioctl_entry(void)
{
	struct vcam_pool pool;
	struct vcam_format fmt = make_fmt(64, 64, 0x23, 64);
	struct file filp;
	/* The ioctl entry keeps the kernel signature, where the third parameter is
	 * `unsigned long` and therefore pointer-sized. On this Win64 host
	 * `unsigned long` is 32-bit, so a stack pointer passed through it would be
	 * truncated (the kernel is LP64 and has no such limit). Addressing the test
	 * data below 4 GiB lets the real entry point be exercised unmodified. */
	struct vcam_version *ver = host_low_alloc(sizeof(*ver));
	struct vcam_frame *frame = host_low_alloc(sizeof(*frame));
	struct vcam_dequeue *dq = host_low_alloc(sizeof(*dq));
	u64 *relseq_p = host_low_alloc(sizeof(*relseq_p));
	unsigned int unknown = _IO(VCAM_IOC_MAGIC, 0x7f);
	unsigned long a_ver, a_frame, a_dq, a_rel;
	u32 slot = 0;

	expect(ver != NULL && frame != NULL && dq != NULL && relseq_p != NULL,
	       "low-address test data allocated");
	if (ver == NULL || frame == NULL || dq == NULL || relseq_p == NULL)
		return;
	a_ver = (unsigned long)(uintptr_t)ver;
	a_frame = (unsigned long)(uintptr_t)frame;
	a_dq = (unsigned long)(uintptr_t)dq;
	a_rel = (unsigned long)(uintptr_t)relseq_p;

	expect_eq(vcam_pool_init(&pool, 1), 0, "1-slot pool for the ioctl entry");
	vcam_shim_register_fd(50, 4096);
	expect_eq(vcam_pool_register(&pool, 50, &fmt, &slot), 0, "register fd 50");

	memset(&filp, 0, sizeof(filp));
	memset(ver, 0, sizeof(*ver));
	expect_eq(vcam_ioctl(&filp, VCAM_IOC_GET_VERSION, a_ver), -ENODEV,
		  "ioctl without private_data fails");

	filp.private_data = &pool;
	expect_eq(vcam_ioctl(&filp, VCAM_IOC_GET_VERSION, a_ver), 0,
		  "GET_VERSION through the ioctl entry");
	expect_eq((long)ver->abi, 1L, "version reports the abi");
	expect_eq((long)ver->pool_size, 1L, "version reports the pool size");
	expect_eq((long)ver->max_streams, 1L, "version reports one stream");
	expect_eq(vcam_ioctl(&filp, unknown, 0), -ENOTTY, "unknown ioctl nr rejected");

	/* a round trip through the ioctl marshalling */
	memset(frame, 0, sizeof(*frame));
	frame->slot = slot;
	frame->seq = 1;
	frame->timestamp_ns = 100;
	expect_eq(vcam_ioctl(&filp, VCAM_IOC_SUBMIT, a_frame), 0,
		  "SUBMIT through the ioctl entry");
	memset(dq, 0, sizeof(*dq));
	expect_eq(vcam_ioctl(&filp, VCAM_IOC_DEQUEUE, a_dq), 0,
		  "DEQUEUE through the ioctl entry");
	expect_eq((long)dq->seq, 1L, "the marshalled frame survives the round trip");
	expect_eq((long)dq->slot, (long)slot, "the slot survives the round trip");
	*relseq_p = 1;
	expect_eq(vcam_ioctl(&filp, VCAM_IOC_RELEASE, a_rel), 0,
		  "RELEASE through the ioctl entry");

	frame->slot = 99;
	expect_eq(vcam_ioctl(&filp, VCAM_IOC_SUBMIT, a_frame), -ENOENT,
		  "SUBMIT on a stale slot is refused");

	expect_eq(vcam_pool_unregister(&pool, slot), 0, "cleanup");
	vcam_pool_destroy(&pool);
}

int main(void)
{
	/* Unbuffered so a crash cannot hide how far the harness got. */
	setvbuf(stdout, NULL, _IONBF, 0);

	phase_abi();
	phase_module_init();
	phase_register_and_poll();
	phase_submit_and_guard();
	phase_pose();
	phase_range();
	phase_ioctl_entry();

	printf("%d checks, %d failure(s)\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
