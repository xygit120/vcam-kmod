# vcam.ko -- kernel-side port of a userspace virtual-camera injection stack

An out-of-tree Linux kernel module that implements the **kernel-portable half**
of a userspace virtual-camera design: a dma-buf frame pool with a hardened
ownership state machine, a small ioctl control plane, and the pose / play-range /
format rules ported from the original.

Everything is GPL-2.0. There is no vendor code in this repository: it is an
independent reimplementation written from observed behaviour and from public
kernel interfaces.

## What it does

The original design reaches the camera buffer from userspace: it decodes frames
with FFmpeg/MediaCodec, injects a hook library into the camera service, rewrites
the already-produced buffer on its way to the consumer, and restores the stock
camera by killing the service and letting it restart.

This module moves the *data plane* into the kernel and drops the fragile parts:

| userspace original | this module |
|---|---|
| process injection + inline hooks on the camera service | **gone** -- one module loaded once, no injection, no `setenforce` window |
| kill the camera service to un-hook | **gone** -- `rmmod`, or simply stop binding frames |
| register an `IMemory` sink and memcpy packed YUV420 into it | **replaced by dma-buf rebinding** -- the module never copies a pixel |
| Binder service with a large transaction table | **ioctls on `/dev/vcam`** (10 commands) |

Decoding intentionally stays in userspace: userspace writes frames into a
dma-heap buffer and hands the module a dma-buf fd.

## Layout

```
include/vcam_uapi.h     shared userspace/kernel ABI (portable _IOC encoding)
include/vcam_kmod.h     internal API: pool, backends, ported rules
src/vcam_main.c         module init/exit, /dev/vcam, module parameters
src/vcam_ctrl.c         ioctl dispatch and userspace marshalling
src/vcam_pool.c         dma-buf pool + ownership state machine
src/vcam_logic.c        pose / range / format rules ported from the original
tools/vcamctl.c         userspace client (dma-heap on Android, memfd on Linux)
shim/                   kernel-API shim for the offline harness (never kbuild)
host/vcam_host_test.c   offline harness: compiles and runs the module sources
verify/                 ioctl ABI check + the authoritative kernel ioctl header
verify/kmi_*.txt        GKI ABI symbol lists used by the symbol check
scripts/verify.py       offline build + behaviour verification
scripts/verify_kmi.py   kernel symbol surface vs the GKI KMI lists
BUILD-KO.md             how to produce a real .ko (the KernelSU/DDK route)
```

## The rules that were ported

These are the parts of the original that are genuinely kernel-portable, and each
one is covered by the harness:

* **Pose** -- in auto-orientation mode the output angle is `270 - media rotation`
  and mirroring is forced on; otherwise an explicit angle (only 0/90/180/270 is
  accepted, anything else becomes 0) and an explicit mirror flag are used.
* **Play range / loop** -- frames past the range end are dropped when looping is
  off; with looping on they are delivered and a wrap is counted. Seeking the
  decoder back stays in userspace because only userspace owns the decoder.
* **Format gating** -- the format is only published when it is not one the
  original already recognises, and the published value is cleared after it is
  read (a delta poll).
* **Ownership** -- a slot the consumer still holds refuses re-fill with `-EBUSY`.
  This single rule is what prevents tearing, and the harness has a dedicated
  case for it.

## Verified

```text
gcc -std=gnu11 -DVCAM_HOST_BUILD=1 -Wall -Wextra -Werror -Ishim -Iinclude \
    host/vcam_host_test.c -o build/vcam_host_test && ./build/vcam_host_test
-> 190 checks, 0 failure(s)

cc -std=gnu11 -Wall -Wextra -Werror -Iinclude verify/ioctl_abi_check.c -o build/ioctl_abi_check
-> all 10 ioctl encodings match the authoritative kernel _IOC definition bit for bit

python scripts/verify_kmi.py
-> the module's whole symbol surface is inside the GKI KMI on the phone lists
   (no modpost bypass needed; only the Cuttlefish virtual-device list lacks the
   dma-buf exporter symbols)
```

`scripts/verify.py` runs the first two in one go.

## Not verified

**No `.ko` has been built or loaded.** Building one needs a Linux kernel tree, an
aarch64 cross toolchain and kbuild, none of which existed in the environment
where this was written. [BUILD-KO.md](BUILD-KO.md) gives three ways to produce it
(the `ddk` CLI, the DDK container used by KernelSU's CI, or your device's own
kernel tree) plus the deployment and integration notes. The device-specific step
that remains is binding a pool dma-buf into the target camera driver; the README
sections in [BUILD-KO.md](BUILD-KO.md) and `include/vcam_kmod.h` describe the
entry points and the `dma_buf_begin_cpu_access`/`end_cpu_access` requirement.

## Deploying

```bash
make -C /path/to/kernel/tree M=$PWD modules
adb push vcam.ko /data/local/tmp/
adb shell su -c 'insmod /data/local/tmp/vcam.ko slots=3 backend=0'
adb shell su -c 'dmesg | tail -3'
```

Module parameters: `slots` (1..8, default 3) and `backend` (0 = userspace
bridge, 1 = in-kernel pipeline glue).

## Provenance and third-party material

See [NOTICE](NOTICE). In short: the module and its tests are original work
released under GPL-2.0; `verify/linux-asm-generic-ioctl.h` and the
`verify/kmi_*.txt` symbol lists are files from the Linux kernel / AOSP and keep
their own licenses; the build method follows the public KernelSU and DDK
projects, which are credited in [BUILD-KO.md](BUILD-KO.md).
