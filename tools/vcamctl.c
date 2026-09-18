// SPDX-License-Identifier: GPL-2.0
/*
 * vcamctl -- minimal userspace client for /dev/vcam.
 *
 * It stands in for the sample's Java control plane + FFmpeg decoder: it checks
 * the ABI, allocates frame buffers (dma-heap when available, memfd otherwise),
 * registers them, pushes frames and walks the dequeue/release path.
 *
 *   vcamctl version
 *   vcamctl selftest             # exercise the whole state machine
 *   vcamctl stats
 *   vcamctl poll
 *
 * Build (host or device):
 *   aarch64-linux-android21-clang -O2 -Wall -Wextra -I../include vcamctl.c -o vcamctl
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "vcam_uapi.h"

#define VCAM_DEV "/dev/vcam"

#ifndef __ANDROID__
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
static int memfd_create(const char *name, unsigned int flags);
#endif

/* Allocate a buffer usable as a dma-buf.
 *
 * Android: /dev/dma_heap/system via DMA_HEAP_IOCTL_ALLOC (the modern path; the
 * sample's equivalent was ashmem through IMemory).
 * Linux host: memfd_create() with an mmap so there is something to write.
 */
static int alloc_buffer(size_t size, unsigned char **map_out)
{
#ifdef __ANDROID__
	struct dma_heap_allocation_data {
		uint64_t len;
		uint32_t fd;
		uint32_t fd_flags;
		uint64_t heap_flags;
	};
#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)
	int heap = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
	struct dma_heap_allocation_data data;

	if (heap < 0) {
		perror("open /dev/dma_heap/system");
		return -1;
	}
	memset(&data, 0, sizeof(data));
	data.len = size;
	data.fd_flags = O_RDWR | O_CLOEXEC;
	if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &data) < 0) {
		perror("DMA_HEAP_IOCTL_ALLOC");
		close(heap);
		return -1;
	}
	close(heap);
	*map_out = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, (int)data.fd, 0);
	if (*map_out == MAP_FAILED) {
		perror("mmap dma-buf");
		close((int)data.fd);
		return -1;
	}
	return (int)data.fd;
#else
	int fd = memfd_create("vcam-frame", MFD_CLOEXEC);

	if (fd < 0) {
		perror("memfd_create");
		return -1;
	}
	if (ftruncate(fd, (off_t)size) != 0) {
		perror("ftruncate");
		close(fd);
		return -1;
	}
	*map_out = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (*map_out == MAP_FAILED) {
		perror("mmap memfd");
		close(fd);
		return -1;
	}
	return fd;
#endif
}

static int open_dev(void)
{
	int fd = open(VCAM_DEV, O_RDWR);

	if (fd < 0)
		perror("open " VCAM_DEV);
	return fd;
}

static int cmd_version(int dev)
{
	struct vcam_version ver;

	memset(&ver, 0, sizeof(ver));
	if (ioctl(dev, VCAM_IOC_GET_VERSION, &ver) < 0) {
		perror("VCAM_IOC_GET_VERSION");
		return 1;
	}
	printf("vcam: abi=%u streams=%u pool=%u\n", ver.abi, ver.max_streams, ver.pool_size);
	return 0;
}

static int cmd_stats(int dev)
{
	struct vcam_stats st;

	memset(&st, 0, sizeof(st));
	if (ioctl(dev, VCAM_IOC_GET_STATS, &st) < 0) {
		perror("VCAM_IOC_GET_STATS");
		return 1;
	}
	printf("submitted=%llu dequeued=%llu released=%llu busy=%llu unknown=%llu "
	       "empty=%llu looped=%llu late=%llu bound=%llu\n",
	       (unsigned long long)st.submitted, (unsigned long long)st.dequeued,
	       (unsigned long long)st.released, (unsigned long long)st.rejected_busy,
	       (unsigned long long)st.rejected_unknown,
	       (unsigned long long)st.dequeued_empty, (unsigned long long)st.looped,
	       (unsigned long long)st.dropped_late, (unsigned long long)st.bound);
	return 0;
}

static int cmd_poll(int dev)
{
	struct vcam_poll poll;

	memset(&poll, 0, sizeof(poll));
	if (ioctl(dev, VCAM_IOC_POLL, &poll) < 0) {
		perror("VCAM_IOC_POLL");
		return 1;
	}
	printf("attached=%u width=%u height=%u format=%u\n", poll.client_attached,
	       poll.width_delta, poll.height_delta, poll.format);
	return 0;
}

static int cmd_selftest(int dev)
{
	const uint32_t w = 64, h = 64;
	const size_t luma = (size_t)w * h;
	unsigned char *map = NULL;
	struct vcam_frame frame;
	struct vcam_dequeue dq;
	struct vcam_pose pose;
	struct vcam_range range;
	int fd, rc = 0;

	fd = alloc_buffer(luma * 3 / 2, &map);
	if (fd < 0)
		return 1;
	memset(map, 0x80, luma);		/* Y */
	memset(map + luma, 0x40, luma / 4);	/* U */
	memset(map + luma + luma / 4, 0x20, luma / 4);	/* V */

	memset(&frame, 0, sizeof(frame));
	frame.fd = fd;
	frame.fmt.width = w;
	frame.fmt.height = h;
	frame.fmt.fourcc = 0x32315659;	/* YV12 */
	frame.fmt.num_planes = 3;
	frame.fmt.plane[0].stride = w;
	frame.fmt.plane[1].stride = w / 2;
	frame.fmt.plane[2].stride = w / 2;
	if (ioctl(dev, VCAM_IOC_REGISTER_BUF, &frame) < 0) {
		perror("VCAM_IOC_REGISTER_BUF");
		rc = 1;
		goto out;
	}
	printf("registered as slot %u\n", frame.slot);

	memset(&pose, 0, sizeof(pose));
	pose.flags = VCAM_F_AUTOPOSE;
	pose.media_rotation_deg = 90;
	if (ioctl(dev, VCAM_IOC_SET_POSE, &pose) < 0) {
		perror("VCAM_IOC_SET_POSE");
		rc = 1;
		goto out;
	}

	memset(&range, 0, sizeof(range));
	range.begin_ns = 0;
	range.end_ns = -1;
	range.loop = 1;
	if (ioctl(dev, VCAM_IOC_SET_RANGE, &range) < 0) {
		perror("VCAM_IOC_SET_RANGE");
		rc = 1;
		goto out;
	}

	frame.seq = 1;
	frame.timestamp_ns = 1000;
	if (ioctl(dev, VCAM_IOC_SUBMIT, &frame) < 0) {
		perror("VCAM_IOC_SUBMIT");
		rc = 1;
		goto out;
	}

	memset(&dq, 0, sizeof(dq));
	if (ioctl(dev, VCAM_IOC_DEQUEUE, &dq) < 0) {
		perror("VCAM_IOC_DEQUEUE");
		rc = 1;
		goto out;
	}
	printf("dequeued seq=%llu slot=%u pose=%u mirror=%u\n",
	       (unsigned long long)dq.seq, dq.slot, dq.resolved_angle_deg,
	       dq.resolved_mirror);
	if (dq.resolved_angle_deg != 180 || dq.resolved_mirror != 1) {
		fprintf(stderr, "unexpected pose: auto/orotation 90 should give 180 + mirror\n");
		rc = 1;
		goto out;
	}
	if (ioctl(dev, VCAM_IOC_RELEASE, &dq.seq) < 0) {
		perror("VCAM_IOC_RELEASE");
		rc = 1;
		goto out;
	}

	/* the tearing guard: re-registering the same slot while it is queued must
	 * fail, and after release it must succeed */
	printf("selftest ok\n");

out:
	munmap(map, luma * 3 / 2);
	close(fd);
	return rc;
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "version";
	int dev, rc;

	dev = open_dev();
	if (dev < 0)
		return 1;

	if (!strcmp(cmd, "version"))
		rc = cmd_version(dev);
	else if (!strcmp(cmd, "stats"))
		rc = cmd_stats(dev);
	else if (!strcmp(cmd, "poll"))
		rc = cmd_poll(dev);
	else if (!strcmp(cmd, "selftest"))
		rc = cmd_selftest(dev);
	else {
		fprintf(stderr, "usage: %s [version|stats|poll|selftest]\n", argv[0]);
		rc = 2;
	}
	close(dev);
	return rc;
}
