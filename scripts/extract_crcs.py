#!/usr/bin/env python3
"""Mine the device's vendor kernel modules for build inputs.

The device runs 5.10.168-android12-9 (MT6983, Android 13) with
CONFIG_MODVERSIONS=y, so a loadable out-of-tree module must carry exactly the
symbol CRCs of that kernel. It must also match the kernel's vermagic string.

Both are recorded inside the vendor modules that the running kernel already
loads:

  * `.modinfo` holds `vermagic=<the accepted string>`
  * `__versions` holds `struct modversion_info { unsigned long crc;
    char name[56]; }` entries, i.e. the CRC of every symbol the module imports

This script parses every pulled .ko and writes:

  vermagic.txt    the accepted vermagic (and how many modules agree)
  Module.symvers  a symvers file for the symbols vcam.ko needs
  crc-map.txt     every (crc, symbol) pair found, for reference
"""
import pathlib
import struct
import sys
from collections import Counter, defaultdict

HERE = pathlib.Path(__file__).resolve().parent
MODDIR = HERE / "vendor-modules"

# Symbols vcam.ko references (derived from the module sources; see
# ../../work/18-0-equivalent-source/verify_kmi.py for the API -> symbol mapping),
# plus the ones the compiler and modpost add.
NEEDED = [
    "module_layout",
    "__this_module",
    "__stack_chk_guard",
    "__stack_chk_fail",
    "misc_register",
    "misc_deregister",
    "dma_buf_get",
    "dma_buf_put",
    "__mutex_init",
    "mutex_lock",
    "mutex_unlock",
    "memory",            # no-op alias check; ignored if absent
    "memset",
    "memcpy",
    "__arch_copy_from_user",
    "__arch_copy_to_user",
    "_printk",
    "printk",
    "mutex_destroy",
]


def elf_sections(data):
    """Return {name: (offset, size)} for a little-endian ELF64 file."""
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        return None
    e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
    e_shentsize = struct.unpack_from("<H", data, 0x3A)[0]
    e_shnum = struct.unpack_from("<H", data, 0x3C)[0]
    e_shstrndx = struct.unpack_from("<H", data, 0x3E)[0]
    if not e_shoff or not e_shnum:
        return None

    def sh(i):
        base = e_shoff + i * e_shentsize
        name, stype = struct.unpack_from("<II", data, base)
        off, size = struct.unpack_from("<QQ", data, base + 0x18)
        return name, stype, off, size

    _, _, str_off, str_size = sh(e_shstrndx)
    strtab = data[str_off:str_off + str_size]
    out = {}
    for i in range(e_shnum):
        name_off, _stype, off, size = sh(i)
        if name_off >= len(strtab):
            continue
        end = strtab.find(b"\0", name_off)
        name = strtab[name_off:end if end >= 0 else len(strtab)].decode("latin1")
        out[name] = (off, size)
    return out


def cstr_list(blob):
    for chunk in blob.split(b"\0"):
        if chunk:
            yield chunk.decode("latin1", "replace")


def parse_versions(blob):
    """__versions entries are 64 bytes: u64 crc + char name[56]."""
    out = []
    for off in range(0, len(blob) - 63, 64):
        crc = struct.unpack_from("<Q", blob, off)[0]
        raw = blob[off + 8:off + 64]
        name = raw.split(b"\0", 1)[0].decode("latin1", "replace")
        if not name or not all(32 <= ord(c) < 127 for c in name):
            continue
        out.append((crc, name))
    return out


def main():
    mods = sorted(MODDIR.rglob("*.ko"))
    if not mods:
        print("no modules under %s" % MODDIR)
        return 1

    vermagics = Counter()
    sym_crcs = defaultdict(Counter)
    sym_owner = {}
    versions_seen = 0

    for m in mods:
        data = m.read_bytes()
        secs = elf_sections(data)
        if not secs:
            continue
        if ".modinfo" in secs:
            off, size = secs[".modinfo"]
            for s in cstr_list(data[off:off + size]):
                if s.startswith("vermagic="):
                    vermagics[s[len("vermagic="):]] += 1
        if "__versions" in secs:
            off, size = secs["__versions"]
            entries = parse_versions(data[off:off + size])
            if entries:
                versions_seen += 1
            for crc, name in entries:
                sym_crcs[name][crc] += 1
                sym_owner.setdefault(name, m.name)

    print("modules parsed            : %d" % len(mods))
    print("modules with __versions   : %d" % versions_seen)
    print()
    print("== vermagic values found ==")
    for vm, n in vermagics.most_common():
        print("  %4d x  %s" % (n, vm))

    accepted = vermagics.most_common(1)[0][0] if vermagics else ""
    (HERE / "vermagic.txt").write_text(accepted + "\n", encoding="utf-8")

    print()
    print("== symbols vcam.ko needs ==")
    lines = ["# CRC\tsymbol\tmodule\texport", "# from the device's vendor modules (5.10.168-android12-9, MT6983)"]
    missing = []
    for sym in NEEDED:
        if sym not in sym_crcs:
            if sym != "memory":
                missing.append(sym)
            print("  %-26s MISSING" % sym)
            continue
        crc, n = sym_crcs[sym].most_common(1)[0]
        conflict = "  (CONFLICTING CRCs! %s)" % dict(sym_crcs[sym]) if len(sym_crcs[sym]) > 1 else ""
        print("  %-26s 0x%016x  seen in %d module(s)%s" % (sym, crc, n, conflict))
        lines.append("0x%08x\t%s\t%s\tEXPORT_SYMBOL" % (crc & 0xFFFFFFFF, sym, sym_owner.get(sym, "vmlinux")))

    (HERE / "Module.symvers").write_text("\n".join(lines) + "\n", encoding="utf-8")
    with (HERE / "crc-map.txt").open("w", encoding="utf-8") as fh:
        for name in sorted(sym_crcs):
            crc = sym_crcs[name].most_common(1)[0][0]
            fh.write("0x%016x %s\n" % (crc, name))

    print()
    if missing:
        print("WARNING: %d needed symbol(s) not found in any vendor module:" % len(missing))
        for s in missing:
            print("   " + s)
        print("(they may still be exported by vmlinux; check /proc/kallsyms)")
    print("wrote: vermagic.txt, Module.symvers, crc-map.txt  (%d symbols mapped)" % len(sym_crcs))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
