#!/usr/bin/env python3
"""Check the module's kernel symbol surface against the GKI KMI lists.

Why this matters
----------------
A loadable module may only reference symbols the GKI kernel guarantees. KernelSU
patches `scripts/mod/modpost.c` to bypass `check_exports` because its module
reaches into internals; a module that stays inside the KMI needs no such patch.

Method
------
The kernel API surface is derived from the module sources, then mapped to the
symbol the compiler actually emits a relocation for **on arm64**. That mapping is
the part that is easy to get wrong:

  * `copy_from_user()`/`copy_to_user()` are inline wrappers; on arm64 they become
    `__arch_copy_from_user`/`__arch_copy_to_user` (plus `__check_object_size`),
    not a symbol named `copy_from_user`.
  * `mutex_destroy()` is a no-op inline unless CONFIG_DEBUG_MUTEXES is set.
  * `printk()` resolves to `_printk` on newer kernels and `printk` on older ones.

Usage:
    python scripts/verify_kmi.py
"""
import pathlib
import re

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
VERIFY = ROOT / "verify"

API_TO_SYMBOLS = {
    "misc_register": ["misc_register"],
    "misc_deregister": ["misc_deregister"],
    "dma_buf_get": ["dma_buf_get"],
    "dma_buf_put": ["dma_buf_put"],
    "dma_buf_attach": ["dma_buf_attach"],
    "dma_buf_detach": ["dma_buf_detach"],
    "dma_buf_map_attachment": ["dma_buf_map_attachment"],
    "dma_buf_unmap_attachment": ["dma_buf_unmap_attachment"],
    "dma_buf_begin_cpu_access": ["dma_buf_begin_cpu_access"],
    "dma_buf_end_cpu_access": ["dma_buf_end_cpu_access"],
    "mutex_init": ["__mutex_init"],
    "mutex_lock": ["mutex_lock"],
    "mutex_unlock": ["mutex_unlock"],
    "kmalloc": ["kmalloc_trace", "__kmalloc"],
    "kzalloc": ["kmalloc_trace", "__kmalloc"],
    "kcalloc": ["__kmalloc", "kmalloc_trace"],
    "kfree": ["kfree"],
    "copy_from_user": ["__arch_copy_from_user", "_copy_from_user", "raw_copy_from_user"],
    "copy_to_user": ["__arch_copy_to_user", "_copy_to_user", "raw_copy_to_user"],
    "get_user": ["__get_user_8", "__get_user_4"],
    "put_user": ["__put_user_8", "__put_user_4"],
    "memset": ["memset"],
    "memcpy": ["memcpy"],
    "strlen": ["strlen"],
    "pr_info": ["_printk", "printk"],
    "pr_warn": ["_printk", "printk"],
    "pr_err": ["_printk", "printk"],
    "printk": ["_printk", "printk"],
}

CONDITIONAL = {
    "mutex_destroy": (["mutex_destroy", "__mutex_destroy"], "CONFIG_DEBUG_MUTEXES"),
}


def module_apis():
    found = set()
    names = "|".join(sorted(set(API_TO_SYMBOLS) | set(CONDITIONAL),
                           key=len, reverse=True))
    call = re.compile(r"\b(" + names + r")\s*\(")
    for src in sorted((ROOT / "src").glob("*.c")):
        text = src.read_text(encoding="utf-8", errors="replace")
        text = re.sub(r"//[^\n]*", "", text)
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        for m in call.finditer(text):
            found.add(m.group(1))
    return found


def load_list(path):
    """Return the symbol set, or None for a missing/404 stub, or a ('delta', ...) tuple."""
    text = path.read_text(encoding="utf-8", errors="replace")
    if "[abi_symbol_list]" not in text:
        return None
    syms = set()
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or line.startswith("["):
            continue
        syms.add(line)
    if len(syms) < 100:
        return None
    # Some vendor files list only the vendor's additions on top of the common
    # KMI, so they hold a few hundred entries instead of a few thousand.
    return syms if len(syms) >= 1000 else ("delta", syms)


def main():
    apis = module_apis()
    print("kernel APIs used by the module sources (%d):" % len(apis))
    print("   " + ", ".join(sorted(apis)))

    required = {a: API_TO_SYMBOLS.get(a, []) for a in sorted(apis)
                if a not in CONDITIONAL}

    lists, deltas = {}, {}
    for p in sorted(VERIFY.glob("kmi_*.txt")):
        syms = load_list(p)
        if isinstance(syms, tuple):
            deltas[p.stem.replace("kmi_", "")] = syms[1]
        elif syms:
            lists[p.stem.replace("kmi_", "")] = syms
        else:
            print("   (skipping %s: not a real KMI list)" % p.name)
    if not lists:
        print("no usable KMI lists in %s" % VERIFY)
        return 1

    print("\ncoverage per complete KMI list:")
    failures = 0
    for name, syms in lists.items():
        missing = ["%s(%s)" % (api, "/".join(alts))
                   for api, alts in required.items()
                   if alts and not any(a in syms for a in alts)]
        ok = not missing
        print("   %-28s %s%s" % (name, "OK" if ok else "MISSING",
                                 "" if ok else "  -> " + ", ".join(missing)))
        if not ok:
            failures += 1

    if deltas:
        print("\nvendor delta lists (additions on top of the common KMI, not scored):")
        for name, syms in deltas.items():
            print("   %-28s %d vendor symbols" % (name, len(syms)))

    uncovered = [api for api, alts in required.items()
                 if alts and not any(any(a in s for a in alts) for s in lists.values())]

    print("\nnote: module_layout and __this_module are added by modpost.")
    print("note: a miss in one device-specific list (e.g. the Cuttlefish "
          "virtual-device list, which has no dma-buf exporter) means that target "
          "would need the modpost bypass; the phone KMIs do not.")
    print()
    if uncovered:
        print("NOT INSIDE THE GKI KMI: %s appear in no checked list." % ", ".join(uncovered))
        return 1
    print("MODULE SYMBOL SURFACE IS INSIDE THE GKI KMI (%d/%d complete lists clean)."
          % (len(lists) - failures, len(lists)))
    print("-> for the phone KMIs the module can take the plain out-of-tree kbuild")
    print("   path; no scripts/mod/modpost.c check_exports patch is needed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
