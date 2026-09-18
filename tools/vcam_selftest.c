// SPDX-License-Identifier: GPL-2.0
/*
 * vcam_selftest -- on-device conformance test for the vcam kernel module.
 *
 * vcamctl is the minimal operator CLI; this is the test. It drives the whole
 * pool state machine through /dev/vcam and asserts the refusals as well as the
 * successes, because the refusals are where the reconstructed behaviour is
 * encoded:
 *
 *   SUBMIT into a HELD slot            -> EBUSY   (the tearing guard)
 *   UNREGISTER a HELD slot             -> EBUSY
 *   DEQUEUE on an empty pool           -> EAGAIN
 *   RELEASE of a sequence never held   -> ESRCH
 *   a non-increasing sequence number   -> EINVAL
 *   a frame past range_end, loop off   -> dropped, not delivered
 *   pose AUTOPOSE   -> angle = wrap360(270 - media_rotation), mirror forced 1
 *                      (FUN_005415b4: mov w9,#0x10e ; sub w2,w9,w8 ; mov w3,#1)
 *   pose explicit   -> only 0/90/180/270 survive, anything else folds to 0
 *                      (tx18) and mirror comes from VCAM_F_MIRROR
 *   POLL            -> one-shot per field (tx13 delta semantics)
 *
 * Frame memory comes from a /dev/dma_heap/ node. REGISTER_BUF pins the producer's
 * dma-buf, so a plain memfd is not acceptable here: dma_buf_get() rejects it.
 *
 * Build a static Android binary from any host toolchain:
 *   zig cc -target aarch64-linux-musl -static -O2 -Wall -Wextra \
 *          -I../include vcam_selftest.c -o vcam_selftest
 *
 * Run on the device (as root):
 *   adb push vcam_selftest /data/local/tmp/ && \
 *   adb shell su -c /data/local/tmp/vcam_selftest
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "vcam_uapi.h"

#define VCAM_DEV "/dev/vcam"

static unsigned failures;

static void pass(const char *what)
{
	printf("  PASS  %s\n", what);
}

static void expect_ok(const char *what, long rc)
{
	if (rc < 0) {
		printf("  FAIL  %s: errno=%d (%s)\n", what, errno, strerror(errno));
		failures++;
	} else {
		pass(what);
	}
}

static void expect_err(const char *what, long rc, int want)
{
	if (rc >= 0) {
		printf("  FAIL  %s: expected errno %d, call succeeded\n", what, want);
		failures++;
	} else if (errno != want) {
		printf("  FAIL  %s: expected errno %d, got %d (%s)\n", what, want,
		       errno, strerror(errno));
		failures++;
	} else {
		printf("  PASS  %s: refused with %s\n", what, strerror(errno));
	}
}

static void expect_val(const char *what, unsigned long got, unsigned long want)
{
	if (got != want) {
		printf("  FAIL  %s: got %lu, want %lu\n", what, got, want);
		failures++;
	} else {
		printf("  PASS  %s = %lu\n", what, got);
	}
}

/* --- dma-heap producer side ------------------------------------------- */

struct dma_heap_allocation_data {
	uint64_t len;
	uint32_t fd;
	uint32_t fd_flags;
	uint64_t heap_flags;
};

#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC \
	_IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)

static const char *const heap_paths[] = {
	"/dev/dma_heap/system",
	"/dev/dma_heap/system-uncached",
	"/dev/dma_heap/mtk_mm",
	"/dev/dma_heap/mtk_prot_region",
	"/dev/dma_heap/mtk_2d_fr_region",
};

static int alloc_dmabuf(size_t size, unsigned char **map_out)
{
	size_t i;

	for (i = 0; i < sizeof(heap_paths) / sizeof(heap_paths[0]); i++) {
		struct dma_heap_allocation_data data;
		void *map;
		int heap, fd;

		/* The heap nodes are commonly mode 0444; the alloc ioctl only
		 * needs the file open, so O_RDONLY is enough. */
		heap = open(heap_paths[i], O_RDONLY | O_CLOEXEC);
		if (heap < 0)
			continue;
		memset(&data, 0, sizeof(data));
		data.len = size;
		data.fd_flags = O_RDWR | O_CLOEXEC;
		if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &data) < 0) {
			printf("  note  %s: alloc: %s\n", heap_paths[i],
			       strerror(errno));
			close(heap);
			continue;
		}
		close(heap);
		fd = (int)data.fd;
		map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (map == MAP_FAILED) {
			printf("  note  %s: mmap: %s\n", heap_paths[i],
			       strerror(errno));
			close(fd);
			continue;
		}
		printf("  note  frame buffer: %s, fd=%d, %zu bytes\n",
		       heap_paths[i], fd, size);
		*map_out = map;
		return fd;
	}
	return -1;
}

static void fill_frame(unsigned char *map, size_t luma)
{
	memset(map, 0x80, luma);			/* Y */
	memset(map + luma, 0x40, luma / 4);		/* U */
	memset(map + luma + luma / 4, 0x20, luma / 4);	/* V */
}

int main(void)
{
	const uint32_t w = 64, h = 64;
	const size_t luma = (size_t)w * h;
	const size_t size = luma * 3 / 2;
	unsigned char *map = NULL;
	struct vcam_version ver;
	struct vcam_frame frame;
	struct vcam_dequeue dq;
	struct vcam_pose pose;
	struct vcam_range range;
	struct vcam_poll poll;
	struct vcam_stats base, st;
	uint32_t slot;
	int dev, fd;

	printf("vcam_selftest: abi=%u\n", VCAM_UAPI_ABI_VERSION);

	dev = open(VCAM_DEV, O_RDWR);
	if (dev < 0) {
		printf("  FAIL  open %s: %s\n", VCAM_DEV, strerror(errno));
		return 1;
	}

	fd = alloc_dmabuf(size, &map);
	if (fd < 0) {
		printf("  FAIL  no usable dma-heap on this device\n");
		close(dev);
		return 1;
	}
	fill_frame(map, luma);

	/* The assertions below -- strictly increasing sequence numbers, one-shot
	 * POLL deltas -- describe a *fresh* pool, because the sequence watermark
	 * and the last-polled geometry are module state, not per-open state.
	 * Rather than emit a cascade of confusing failures on a reused module,
	 * say so once and stop. The counters are still asserted as deltas so the
	 * values stay readable. */
	memset(&base, 0, sizeof(base));
	if (ioctl(dev, VCAM_IOC_GET_STATS, &base) < 0) {
		printf("  FAIL  GET_STATS baseline: %s\n", strerror(errno));
		failures++;
	}
	if (base.submitted != 0 || base.dequeued != 0 || base.dropped_late != 0) {
		printf("  SKIP  the pool is not fresh: submitted=%llu dequeued=%llu "
		       "dropped_late=%llu\n"
		       "        Reload the module and re-run:\n"
		       "          su -c 'rmmod vcam; insmod /data/local/tmp/vcam.ko "
		       "slots=3 backend=0'\n",
		       (unsigned long long)base.submitted,
		       (unsigned long long)base.dequeued,
		       (unsigned long long)base.dropped_late);
		close(dev);
		close(fd);
		munmap(map, size);
		return 2;
	}

	/* --- ABI ------------------------------------------------------- */
	memset(&ver, 0, sizeof(ver));
	expect_ok("GET_VERSION", ioctl(dev, VCAM_IOC_GET_VERSION, &ver));
	expect_val("  abi", ver.abi, VCAM_UAPI_ABI_VERSION);
	expect_val("  max_streams", ver.max_streams, 1);
	if (ver.pool_size == 0) {
		printf("  FAIL  pool_size is 0\n");
		failures++;
	} else {
		printf("  PASS  pool_size = %u\n", ver.pool_size);
	}

	/* --- register -------------------------------------------------- */
	memset(&frame, 0, sizeof(frame));
	frame.fd = fd;
	frame.fmt.width = w;
	frame.fmt.height = h;
	frame.fmt.fourcc = 0x32315659u;	/* YV12 */
	frame.fmt.num_planes = 3;
	frame.fmt.plane[0].stride = w;
	frame.fmt.plane[1].stride = w / 2;
	frame.fmt.plane[2].stride = w / 2;
	expect_ok("REGISTER_BUF", ioctl(dev, VCAM_IOC_REGISTER_BUF, &frame));
	expect_val("  slot", frame.slot, 0);

	/* tx13: geometry is published, the YV12 value is filtered by
	 * vcam_format_reportable(), and every field is one-shot. */
	memset(&poll, 0, sizeof(poll));
	expect_ok("POLL #1", ioctl(dev, VCAM_IOC_POLL, &poll));
	expect_val("  width_delta", poll.width_delta, w);
	expect_val("  height_delta", poll.height_delta, h);
	expect_val("  format (YV12 is not reportable)", poll.format, 0);
	memset(&poll, 0, sizeof(poll));
	expect_ok("POLL #2", ioctl(dev, VCAM_IOC_POLL, &poll));
	expect_val("  width_delta after read", poll.width_delta, 0);

	/* --- input validation ------------------------------------------ */
	memset(&pose, 0, sizeof(pose));
	pose.flags = 0x80000000u;
	expect_err("SET_POSE with unknown flag bits",
		   ioctl(dev, VCAM_IOC_SET_POSE, &pose), EINVAL);

	memset(&range, 0, sizeof(range));
	range.begin_ns = 5000;
	range.end_ns = 1000;
	expect_err("SET_RANGE with end < begin",
		   ioctl(dev, VCAM_IOC_SET_RANGE, &range), EINVAL);

	/* --- the pose rules from FUN_005415b4 --------------------------- */
	memset(&pose, 0, sizeof(pose));
	pose.flags = VCAM_F_AUTOPOSE;
	pose.media_rotation_deg = 90;	/* 270 - 90 = 180, mirror forced on */
	expect_ok("SET_POSE autopose/90", ioctl(dev, VCAM_IOC_SET_POSE, &pose));

	memset(&range, 0, sizeof(range));
	range.begin_ns = 0;
	range.end_ns = -1;
	range.loop = 1;
	expect_ok("SET_RANGE 0..-1 loop on",
		   ioctl(dev, VCAM_IOC_SET_RANGE, &range));

	/* --- submit / dequeue / release --------------------------------- */
	frame.seq = 1;
	frame.timestamp_ns = 1000;
	expect_ok("SUBMIT seq=1", ioctl(dev, VCAM_IOC_SUBMIT, &frame));

	memset(&dq, 0, sizeof(dq));
	expect_ok("DEQUEUE", ioctl(dev, VCAM_IOC_DEQUEUE, &dq));
	expect_val("  dq.seq", dq.seq, 1);
	expect_val("  resolved_angle (autopose 90)", dq.resolved_angle_deg, 180);
	expect_val("  resolved_mirror", dq.resolved_mirror, 1);

	/* the tearing guard: the consumer holds the only buffer */
	frame.seq = 2;
	expect_err("SUBMIT into a HELD slot (tearing guard)",
		   ioctl(dev, VCAM_IOC_SUBMIT, &frame), EBUSY);
	expect_err("DEQUEUE while the only buffer is HELD",
		   ioctl(dev, VCAM_IOC_DEQUEUE, &dq), EAGAIN);
	slot = frame.slot;
	expect_err("UNREGISTER a HELD slot",
		   ioctl(dev, VCAM_IOC_UNREGISTER_BUF, &slot), EBUSY);
	{
		uint64_t never_held = 77;

		expect_err("RELEASE a sequence never dequeued",
			   ioctl(dev, VCAM_IOC_RELEASE, &never_held), ESRCH);
	}
	expect_ok("RELEASE seq=1", ioctl(dev, VCAM_IOC_RELEASE, &dq.seq));

	/* sequence numbers must increase across the pool's lifetime */
	frame.seq = 0;
	expect_err("SUBMIT seq=0 after seq=1",
		   ioctl(dev, VCAM_IOC_SUBMIT, &frame), EINVAL);

	frame.seq = 2;
	expect_ok("SUBMIT seq=2 after release", ioctl(dev, VCAM_IOC_SUBMIT, &frame));
	memset(&dq, 0, sizeof(dq));
	expect_ok("DEQUEUE seq=2", ioctl(dev, VCAM_IOC_DEQUEUE, &dq));
	expect_val("  dq.seq", dq.seq, 2);
	expect_ok("RELEASE seq=2", ioctl(dev, VCAM_IOC_RELEASE, &dq.seq));

	memset(&dq, 0, sizeof(dq));
	expect_err("DEQUEUE on an empty pool",
		   ioctl(dev, VCAM_IOC_DEQUEUE, &dq), EAGAIN);

	/* tx18: an explicit angle outside {0,90,180,270} folds to 0 */
	memset(&pose, 0, sizeof(pose));
	pose.flags = VCAM_F_MIRROR;
	pose.rotation_deg = 45;
	expect_ok("SET_POSE explicit 45", ioctl(dev, VCAM_IOC_SET_POSE, &pose));
	frame.seq = 3;
	frame.timestamp_ns = 3000;
	expect_ok("SUBMIT seq=3", ioctl(dev, VCAM_IOC_SUBMIT, &frame));
	memset(&dq, 0, sizeof(dq));
	expect_ok("DEQUEUE seq=3", ioctl(dev, VCAM_IOC_DEQUEUE, &dq));
	expect_val("  resolved_angle (45 -> 0)", dq.resolved_angle_deg, 0);
	expect_val("  resolved_mirror (explicit flag)", dq.resolved_mirror, 1);
	expect_ok("RELEASE seq=3", ioctl(dev, VCAM_IOC_RELEASE, &dq.seq));

	/* --- range end with loop off ------------------------------------ */
	memset(&range, 0, sizeof(range));
	range.begin_ns = 0;
	range.end_ns = 100;
	range.loop = 0;
	expect_ok("SET_RANGE 0..100 loop off",
		   ioctl(dev, VCAM_IOC_SET_RANGE, &range));
	frame.seq = 4;
	frame.timestamp_ns = 500;	/* past the range end */
	expect_ok("SUBMIT seq=4 past the range end",
		   ioctl(dev, VCAM_IOC_SUBMIT, &frame));
	memset(&dq, 0, sizeof(dq));
	expect_err("DEQUEUE drops the frame past range_end",
		   ioctl(dev, VCAM_IOC_DEQUEUE, &dq), EAGAIN);

	/* --- counters must match exactly what happened above ------------ */
	memset(&st, 0, sizeof(st));
	expect_ok("GET_STATS", ioctl(dev, VCAM_IOC_GET_STATS, &st));
	expect_val("  submitted", st.submitted - base.submitted, 4);
	expect_val("  dequeued", st.dequeued - base.dequeued, 3);
	expect_val("  released", st.released - base.released, 3);
	expect_val("  bound", st.bound - base.bound, 3);
	expect_val("  rejected_busy", st.rejected_busy - base.rejected_busy, 2);
	expect_val("  rejected_unknown",
		   st.rejected_unknown - base.rejected_unknown, 2);
	expect_val("  dequeued_empty", st.dequeued_empty - base.dequeued_empty, 2);
	expect_val("  dropped_late", st.dropped_late - base.dropped_late, 1);
	expect_val("  looped", st.looped - base.looped, 0);

	/* --- clean up --------------------------------------------------- */
	slot = 0;
	expect_ok("UNREGISTER an idle slot",
		   ioctl(dev, VCAM_IOC_UNREGISTER_BUF, &slot));
	slot = 99;
	expect_err("UNREGISTER an unknown slot",
		   ioctl(dev, VCAM_IOC_UNREGISTER_BUF, &slot), ENOENT);

	munmap(map, size);
	close(fd);
	close(dev);

	printf("vcam_selftest: %s (%u failure(s))\n",
	       failures ? "FAILED" : "ok", failures);
	return failures ? 1 : 0;
}
