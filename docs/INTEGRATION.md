# Integrating the module with a camera driver

The module owns the pool, the control plane and the ported rules. It
deliberately does **not** contain the one piece that needs the target's camera
driver: rebinding a pool buffer into the pipeline. That is a small backend with
two entry points.

## The glue

```c
struct dma_buf *buf;
u64 seq;

/* Called wherever the camera driver would otherwise hand out its own buffer --
 * e.g. from a vb2_ops.buf_finish / dqbuf hook, or from the driver's
 * buffer-selection path. */
if (vcam_pool_acquire(pool, &seq, &buf) == 0) {
        /* Attach buf to the device instead of the driver's own buffer:
         *   dma_buf_attach() / dma_buf_map_attachment() -> sg_table -> device DMA
         *
         * Bracket any CPU access with:
         *   dma_buf_begin_cpu_access() / dma_buf_end_cpu_access()
         * That is cache maintenance. Skipping it is what corrupts frames on
         * non-coherent arm64 SoCs. */
}

/* When the hardware (or the consumer) is finished with that frame: */
dma_buf_put(buf);               /* the reference acquire handed over */
vcam_pool_release(pool, seq);   /* the slot goes back to the producer */
```

Reference ownership is the whole contract:

* the pool holds **one** dma-buf reference per registered slot for the slot's
  lifetime (taken with `dma_buf_get()` at registration);
* `vcam_pool_acquire()` hands the caller an **additional** reference which the
  caller must `dma_buf_put()` when finished;
* `vcam_pool_release()` only moves the slot back to idle; it never drops a
  reference.

Because a slot that is held cannot be re-filled, the producer can never write
pixels the pipeline is still reading. That is the rule the harness tests with a
dedicated case.

## Deployment modes

| `backend=` | Who consumes frames | Path |
|---:|---|---|
| `0` (bridge) | a userspace component | it drives `VCAM_IOC_DEQUEUE` / `VCAM_IOC_RELEASE`; the module still enforces range, pose and the tearing guard |
| `1` (pipeline) | a kernel glue layer | it calls `vcam_pool_acquire()` / `vcam_pool_release()` as above |

The bridge mode is also what `tools/vcamctl.c` exercises, so the state machine can
be validated before any driver work is done.

Bridge mode is the part that has actually been run on hardware: a full
conformance pass on a rooted Redmi K50 Pro, with SELinux enforcing, is recorded in
[`verify/device-run-k50pro-2026-09-18.txt`](../verify/device-run-k50pro-2026-09-18.txt).
`backend=1` -- the `vcam_pool_acquire()`/`vcam_pool_release()` glue below -- is
the piece that still needs the target's camera driver.

## Format and layout knowledge that matters more than the code

Plane geometry is where implementations fail, not the surrounding code. A sibling
project (`vcam11`) documents vendor specifics that this module deliberately does
not hard-code:

* Qualcomm gralloc handles carry `ints[2] = stride **in pixels**` and
  `ints[3] = aligned height`. Mixing up pixel stride and byte stride silently
  produces row-overlapping garbage; that exact bug is documented in vcam11's
  release notes as a fixed issue, and the workaround was to derive the row size
  from the buffer's real byte size and the aligned height.
* Write plane by plane honouring `rowStride` and `pixelStride`, and validate
  `(width-1)*pixelStride < rowStride` before writing.
* Some streams are vendor ISP tuning payloads rather than images. Rewriting one
  of those wholesale crashed a userspace implementation; treat unknown streams as
  pass-through.
* `VCAM_IOC_REGISTER_BUF` validates that each plane's stride covers its width
  before the slot is accepted, but it cannot know the buffer's size. The backend
  must additionally check the plane extents against the dma-buf size it pinned.

## Choosing the binding point

The options, least to most invasive, are described in the design notes that
accompanied this work: a self-built kernel with a proper hook in the vendor
camera driver is the cleanest; wrapping the driver's `vb2_ops` at load time is
next; a kprobe on the dequeue path is the smallest change and the most fragile.
All three need the kernel (or the vendor driver) under your control, which is the
real prerequisite for this whole approach.
