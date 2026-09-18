# Target device profile

Everything in this directory was read off the connected device or derived from
files on it. It is what `scripts/build-ko.sh` needs in order to produce a `.ko`
the device will actually load.

## Device

| | |
|---|---|
| model / product | `22011211C` / `matisse` (Redmi K50 Pro) |
| SoC | `MT6983` |
| Android | 13 (`ro.build.version.sdk=33`), arm64-v8a |
| kernel (`uname -r`) | `5.10.168-android12-9-00001-g81e7418c6466-ab10486262` |
| KMI | **`android12-5.10`** (the `android12-9` in the version string is the KMI generation of that branch) |
| root | KernelSU (`su -c id` -> `uid=0(root) context=u:r:ksu:s0`) |
| SELinux | Enforcing |

## Why the two data files are required

| config (read from `/proc/config.gz`) | consequence for us |
|---|---|
| `CONFIG_MODULES=y` | modules can be loaded |
| `CONFIG_MODVERSIONS=y` | the `.ko` must carry the kernel's **exact symbol CRCs**, or `insmod` fails with "disagrees about version of symbol" |
| `CONFIG_MODULE_SIG*` unset | **no module signing needed** |
| `CONFIG_MODULE_FORCE_LOAD` unset | `insmod -f` cannot bypass the checks |
| `CONFIG_DEBUG_MUTEXES` unset | `mutex_destroy()` is a no-op inline and emits no relocation |
| `CONFIG_DMA_SHARED_BUFFER=y` | dma-buf is available |

`vermagic.txt` -- the vermagic the running kernel accepts. All 225 vendor
modules under `/vendor_dlkm/lib/modules` agree on it:

```text
5.10.168-android12-9-g04441ca93d05 SMP preempt mod_unload modversions aarch64
```

Note this is **not** the `uname -r` string: the loader compares against the value
carrying the vendor commit `g04441ca93d05`, not the GKI build id.

`Module.symvers` -- the CRC of every symbol this module references, each one
taken from a module the device already loads. Mined by parsing the `__versions`
section of the pulled vendor modules; 2869 symbols were mapped in total, with no
conflicting CRC for any symbol.

| symbol | CRC | | symbol | CRC |
|---|---|---|---|---|
| `module_layout` | `0x7c24b32d` | | `mutex_lock` | `0xeb9065d9` |
| `__stack_chk_guard` | `0x8f678b07` | | `mutex_unlock` | `0xe8b268ae` |
| `__stack_chk_fail` | `0x98a9d10c` | | `memset` | `0xdcb764ad` |
| `misc_register` | `0x6331d19e` | | `memcpy` | `0x4829a47e` |
| `misc_deregister` | `0xdcef2da4` | | `__arch_copy_from_user` | `0xaf507de1` |
| `dma_buf_get` | `0x6a6d5fb3` | | `__arch_copy_to_user` | `0x6b2941b2` |
| `dma_buf_put` | `0x98c2b0fc` | | `printk` | `0xc5850110` |
| `__mutex_init` | `0x574add77` | | | |

The third column of the generated symvers file is the vendor module the CRC came
from; it is informational for modpost, and `vmlinux` would work equally well
since these are all kernel symbols.

## Reproducing this on another device

```text
adb shell su -c 'mkdir -p /data/local/tmp/vmods && cp /vendor_dlkm/lib/modules/*.ko /data/local/tmp/vmods/'
adb pull /data/local/tmp/vmods ./vmods
python3 ../scripts/extract_crcs.py ./vmods        # writes vermagic.txt + Module.symvers
```

`extract_crcs.py` is the same parser used here: it walks the ELF section headers,
reads `vermagic=` out of `.modinfo`, and decodes the 64-byte `struct
modversion_info { unsigned long crc; char name[56]; }` entries of `__versions`.
