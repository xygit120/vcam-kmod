# Building vcam.ko -- the KernelSU / DDK route

This is how to actually produce a loadable `vcam.ko`, following the same method
KernelSU uses for its own kernel module.

## What KernelSU does (and why it matters here)

**KernelSU does not ship kernel headers.** Its repository contains only its own
sources and a thin Kbuild wrapper; the headers come from a full kernel tree
provided by outside the repo. Concretely, from
[`tiann/KernelSU`](https://github.com/tiann/KernelSU):

| File | What it shows |
|---|---|
| `.github/workflows/ddk-lkm.yml` | CI builds `kernelsu.ko` inside the container `ghcr.io/ylarod/ddk-min:<kmi>-<ddk_release>`, then runs `CONFIG_KSU=m CC=clang make` |
| `.github/workflows/build-lkm.yml` | the KMI matrix: `android12-5.10`, `android13-5.10`, `android13-5.15`, `android14-5.15`, `android14-6.1`, `android15-6.6`, `android16-6.12`, `android17-6.18` -- one `.ko` per KMI |
| `kernel/Makefile` | `$(MAKE) -C $(KDIR) M=$(ODIR) src=$(MDIR) modules` (on 6.18 the args become `M= MO=`) |
| `kernel/build-all.sh` | local equivalent: `ddk build "$kmi" "ODIR=$ODIR" -e CONFIG_KSU=m` |
| `kernel/setup.sh` | **older, in-tree** flow: symlink the module into `drivers/kernelsu` and build it as part of the kernel |

The actual header source is the **DDK** project
([`5ec1cff/ddk`](https://github.com/5ec1cff/ddk), images published as
`ghcr.io/ylarod/ddk-*`). Its `docker/ddk-min/Dockerfile` unpacks two prebuilt
archives per Android version:

```
/opt/ddk/src/<kmi>/     <- src.<ANDROID_VER>.tar.zst   full GKI kernel source
/opt/ddk/kdir/<kmi>/    <- kdir.<ANDROID_VER>.tar.zst  prepared build tree
                                                        (the "headers": generated
                                                        headers, scripts,
                                                        Module.symvers, ...)
/opt/ddk/clang/<ver>/bin                                the matching clang
```

and exports exactly the environment kbuild needs:

```
ARCH=arm64  CROSS_COMPILE=aarch64-linux-gnu-  LLVM=1  LLVM_IAS=1
KDIR=/opt/ddk/kdir/<kmi>
```

So "the kernel headers it uses" = **a `modules_prepare`d output tree of the GKI
kernel for that KMI**, taken from the DDK image. The DDK README is explicit that
this guarantees module/内核 *compatibility of the build*, not full behavioural
compatibility: for that you build against the device's own tree.

## Route 1 -- `ddk` CLI (closest to KernelSU's dev flow)

```bash
# Linux host, once
git clone https://github.com/Ylarod/ddk && cd ddk
bash host/install.sh            # unpacks src/ + kdir/ + clang/ into /opt/ddk

# then, from this directory
ddk build android14-6.1 "ODIR=$PWD/out/android14-6.1" -e
```

`ddk build` sets `KDIR`, `ARCH`, `LLVM=1` etc. for you and drops `vcam.ko` in the
given output directory.

## Route 2 -- the DDK container (what KernelSU's CI does)

```bash
docker run --rm -it --privileged -v "$PWD":/work -w /work/kernel \
  ghcr.io/ylarod/ddk-min:android14-6.1-20260828 bash

# inside the container KDIR/ARCH/LLVM are already exported
make -C /path/to/vcam_kmod M=/path/to/vcam_kmod modules
llvm-strip -d vcam.ko
```

KernelSU's workflow additionally does four things that are worth copying
verbatim because they are kbuild requirements, not KernelSU quirks:

```bash
make O=$KDIR gki_defconfig
scripts/config --file $KDIR/.config -d LTO_CLANG -e LTO_NONE \
    -d LTO_CLANG_THIN -d LTO_CLANG_FULL -d THINLTO      # LTO breaks external modules
make O=$KDIR modules_prepare
make O=$KDIR security/selinux/built-in.a                # generate SELinux headers
```

### The one step you should *not* need

KernelSU patches `scripts/mod/modpost.c` to disable `check_exports`, i.e. to
bypass the KMI symbol check. That is needed because its module reaches into
kernel internals that are not in the KMI. **`vcam.ko` does not need it**, and
that is checkable rather than asserted: `scripts/verify_kmi.py` derives the
module's whole kernel symbol surface from the sources and tests it against the
ABI symbol lists published in AOSP's `kernel/common`:

```
python scripts/verify_kmi.py
   android13-5.10_generic       OK
   android14-6.1_mtk            OK
   android14-6.1_qcom           OK
   android15-6.6_qcom           OK
   android14-6.1_virtual_device MISSING -> dma_buf_get, dma_buf_put   (Cuttlefish
                                            has no dma-buf exporter; that target
                                            would need the bypass)
MODULE SYMBOL SURFACE IS INSIDE THE GKI KMI (4/5 complete lists clean).
```

The module touches 13 required symbols: `misc_register`, `misc_deregister`,
`dma_buf_get`, `dma_buf_put`, `__mutex_init`, `mutex_lock`, `mutex_unlock`,
`__arch_copy_from_user`, `__arch_copy_to_user`, `memset`, `_printk` (or `printk`
on 5.10), plus `module_layout`/`__this_module` which modpost adds. It allocates
nothing (`kmalloc`/`kfree` are unused) and hooks nothing.

## Route 3 -- the device's own kernel tree (best fidelity)

```bash
make -C /path/to/device/kernel M=$PWD modules
```

This is what you want if you are targeting one specific device: the resulting
module matches that kernel's `vermagic` and KMI exactly, so `insmod` succeeds
without any of the compatibility caveats the DDK README mentions. It is also the
only route that lets you add the device glue (see the main README) into the same
tree.

## Device loading

```bash
adb push vcam.ko /data/local/tmp/
adb shell su -c 'insmod /data/local/tmp/vcam.ko slots=3 backend=0'
adb shell su -c 'dmesg | tail -5'      # "vcam: ready: /dev/vcam slots=3 backend=0 abi=1"
adb shell su -c 'ls -lZ /dev/vcam'
```

`vermagic` mismatch shows up as `insmod: invalid module format`; that is the
kernel-side analogue of the ABI-variant fragility the sample has in userspace.

## What the first real build proved

`vcam.ko` has now been built by this repository's CI and loaded on the K50 Pro
whose profile is in [`device/`](device). Two things are worth carrying forward.

* **Pinning the release works; rewriting the `.modinfo` field afterwards does
  not.** The DDK tree reports its own release as `5.10.252`, so
  `scripts/build-ko.sh` pins `include/config/kernel.release` and
  `include/generated/utsrelease.h` to the device's string *before* kbuild runs.
  The rewrite-after-the-fact route fails because the built string is shorter than
  the target and the field has no slack in it.
* **The built `.ko` has an empty `__versions` section, and the device loads it
  anyway.** With `CONFIG_MODVERSIONS=y` the loader compares a CRC only for the
  symbols the module lists, so an empty section means nothing is version-checked.
  The CI CRC check consequently reports `0 CRC(s) match, 0 mismatch` -- read that
  as "the check had nothing to check", not as a pass. The symbols the module
  imports resolved against the DDK tree's own `Module.symvers`, including ones the
  15-symbol device profile does not list (`param_ops_uint`,
  `arm64_const_caps_ready`, `cpu_hwcap_keys`, `cpu_hwcaps`,
  `gic_nonsecure_priorities`).

The load, the `dmesg` lines and the full conformance run are recorded in
[`verify/device-run-k50pro-2026-09-18.txt`](verify/device-run-k50pro-2026-09-18.txt).

## Building the userspace side

Both tools under `tools/` are plain C and need no Android SDK and no NDK: zig
ships a complete cross toolchain.

```bash
zig cc -target aarch64-linux-musl -static -O2 -Wall -Wextra \
  -DVCAM_USE_DMA_HEAP -Iinclude tools/vcamctl.c      -o vcamctl
zig cc -target aarch64-linux-musl -static -O2 -Wall -Wextra \
  -Iinclude tools/vcam_selftest.c -o vcam_selftest
```

`.github/workflows/build-tools.yml` does exactly this and uploads the binaries.
`-DVCAM_USE_DMA_HEAP` selects the dma-heap allocation path; without it the CLI
falls back to a memfd, which is fine on a desktop Linux host but is **not**
registrable with the module, because `dma_buf_get()` rejects a memfd.

Then the conformance run:

```bash
adb push vcam_selftest /data/local/tmp/
adb shell "su -c 'chmod 755 /data/local/tmp/vcam_selftest'"
adb shell "su -c 'rmmod vcam; insmod /data/local/tmp/vcam.ko slots=3 backend=0'"
adb shell "su -c '/data/local/tmp/vcam_selftest'"
# -> vcam_selftest: ok (0 failure(s))
```

The reload is not optional: the pool keeps a sequence watermark and the
last-polled geometry for the module's lifetime, and the test's assertions are
about a fresh pool. A second run against the same load reports
`SKIP the pool is not fresh` (exit 2) rather than a cascade of failures.

One trap that costs more time than it should: on a KernelSU device the device
shell has to see the command as **one quoted word** for the whole thing to run as
root. `adb shell 'su -c "cmd1; cmd2"'` runs `cmd1` as root and then `cmd2` as the
unprivileged `shell` user (uid 2000), which surfaces as a puzzling `EACCES` on
`/dev/vcam` that is *not* a SELinux denial -- the discriminator is that no
`avc: denied` record appears for the `open`. Use the quoting of the example
above.

## Loading it on every boot

`packaging/ksu-module/` is a minimal KernelSU/Magisk module: `module.prop` plus a
`post-fs-data.sh` that does one `insmod`. Install it by dropping the directory in
place with the built module next to it:

```bash
adb push vcam.ko /data/local/tmp/
adb push packaging/ksu-module /data/local/tmp/
adb shell "su -c 'mkdir -p /data/adb/modules/vcam_kmod \
  && cp /data/local/tmp/vcam.ko /data/local/tmp/ksu-module/* /data/adb/modules/vcam_kmod/ \
  && chmod 755 /data/adb/modules/vcam_kmod/post-fs-data.sh'"
```

`rm -rf /data/adb/modules/vcam_kmod` uninstalls it again; there is no other state
to undo. Whether KernelSU actually runs the script at boot could not be verified
without rebooting the phone -- what *was* verified is the script itself: after
`rmmod vcam`, running `post-fs-data.sh` by hand loads the module (`rc=0`) and
leaves `/dev/vcam` present.

## Reference

* KernelSU module build: `tiann/KernelSU` -- `.github/workflows/ddk-lkm.yml`,
  `kernel/Makefile`, `kernel/build-all.sh`, `kernel/setup.sh`
* The DDK that supplies the per-KMI trees, clang and environment:
  `5ec1cff/ddk` -- `README.md`, `docker/ddk{,-min}/Dockerfile`
* GKI ABI symbol lists used by the check: `aosp-mirror/kernel_common`,
  `android/abi_gki_aarch64_*`
