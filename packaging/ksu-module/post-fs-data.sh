#!/system/bin/sh
# KernelSU/Magisk module script: load the pool once, early, so /dev/vcam exists
# before anything that wants to produce frames.
#
# slots 1..8, default 3.  backend 0 = userspace bridge (what the device run in
# verify/device-run-k50pro-2026-09-18.txt validated), 1 = in-kernel glue.
#
# A failed insmod must not take the rest of the boot with it, so the exit status
# is not propagated.
MODDIR=${0%/*}

insmod "$MODDIR/vcam.ko" slots=3 backend=0 || true
