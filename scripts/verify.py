#!/usr/bin/env python3
"""Offline verification of the vcam module sources.

There is no kernel tree, aarch64 cross compiler or NDK in the environment this
was written in, so a real `make modules` cannot run here. What can be verified
is:

  1. the portability shim's ioctl encoding matches the authoritative kernel
     definition (include/uapi/asm-generic/ioctl.h) -- compiled as static
     assertions (this is what lets a real client talk to the module);
  2. the module sources compile clean under -Wall -Wextra -Werror against the
     kernel-API shim and their behaviour tests pass.

Usage:
    python scripts/verify.py [--gcc PATH]
"""
import argparse
import os
import pathlib
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent

# Fall back to the toolchain used during development on Windows.
DEFAULT_GCC = r"C:\VCAM_DEV\toolchain\cpp\mingw64\bin\gcc.exe"

FAILURES = []


def check(ok, label, detail=""):
    print(("PASS " if ok else "FAIL ") + label + (" :: " + detail if detail else ""))
    if not ok:
        FAILURES.append(label)
    return ok


def run(cmd, env=None):
    proc = subprocess.run(cmd, capture_output=True, text=True, env=env)
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gcc", default=None)
    args = ap.parse_args()

    gcc = pathlib.Path(args.gcc) if args.gcc else None
    if gcc is None:
        found = shutil.which("gcc") or shutil.which("cc")
        if found:
            gcc = pathlib.Path(found)
        elif pathlib.Path(DEFAULT_GCC).exists():
            gcc = pathlib.Path(DEFAULT_GCC)
    if gcc is None or not gcc.exists():
        print("FAIL no C compiler found; pass --gcc PATH")
        return 1
    print("compiler: %s" % gcc)

    build = ROOT / "build"
    build.mkdir(exist_ok=True)

    env = dict(os.environ)
    env["PATH"] = str(gcc.parent) + os.pathsep + env.get("PATH", "")
    exe_suffix = ".exe" if os.name == "nt" else ""

    print("== 1. ioctl ABI matches the authoritative kernel encoding")
    exe = build / ("ioctl_abi_check" + exe_suffix)
    rc, out = run([str(gcc), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                   "-I", str(ROOT / "include"),
                   str(ROOT / "verify" / "ioctl_abi_check.c"), "-o", str(exe)])
    if check(rc == 0, "ioctl_abi_check compiles", out.strip()[:200]):
        rc, out = run([str(exe)], env=env)
        check(rc == 0, "ioctl ABI assertions hold")
        for line in out.splitlines():
            if line.strip().startswith(("GET_VERSION", "SUBMIT", "REGISTER_BUF",
                                        "POLL", "sizeof")):
                print("     " + line.strip())

    print("== 2. module sources compile and their tests pass")
    exe = build / ("vcam_host_test" + exe_suffix)
    rc, out = run([str(gcc), "-std=gnu11", "-DVCAM_HOST_BUILD=1", "-Wall",
                   "-Wextra", "-Werror", "-I", str(ROOT / "shim"),
                   "-I", str(ROOT / "include"),
                   str(ROOT / "host" / "vcam_host_test.c"), "-o", str(exe)])
    if check(rc == 0, "module sources compile clean (-Wall -Wextra -Werror)",
             out.strip()[:200]):
        rc, out = run([str(exe)], env=env)
        summary = [ln for ln in out.splitlines() if "checks," in ln]
        failures = [ln for ln in out.splitlines() if ln.startswith("FAIL")]
        check(rc == 0 and not failures, "host harness passes",
              (summary[0] if summary else out.strip()[:120]))
        for ln in failures[:5]:
            print("     " + ln)

    print()
    if FAILURES:
        print("%d FAILURE(S): %s" % (len(FAILURES), ", ".join(FAILURES)))
        return 1
    print("ALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
