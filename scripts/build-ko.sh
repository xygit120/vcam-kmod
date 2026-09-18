#!/usr/bin/env bash
# Build vcam.ko for the device profiled in ../device (KMI android12-5.10).
#
# Works both in the DDK container used by CI (where KDIR, ARCH, LLVM=1 and the
# matching clang are already exported) and on a plain Linux host with a prepared
# kernel tree.
#
#   KDIR=/opt/ddk/kdir/android12-5.10 ./scripts/build-ko.sh
#
# What it does beyond a plain `make modules`:
#   * merges the device's Module.symvers into the kernel tree's, so the CRCs the
#     module records in __versions are the ones the device expects (this is what
#     CONFIG_MODVERSIONS=y checks at load time);
#   * checks the built .modinfo vermagic against the device's accepted string,
#     and rewrites that field in place when the tree's release differs;
#   * verifies, before anyone loads it, that every CRC in the built module
#     matches the device profile.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODSRC="${MODSRC:-$ROOT}"
DEVICE="${DEVICE:-$ROOT/device}"
OUT="${OUT:-$ROOT/out}"
KMI="${KMI:-android12-5.10}"

if [ -z "${KDIR:-}" ] && [ -n "${DDK_ROOT:-}" ] && [ -d "$DDK_ROOT/kdir/$KMI" ]; then
    KDIR="$DDK_ROOT/kdir/$KMI"
fi
: "${KDIR:?set KDIR to a prepared 5.10 kernel tree for $KMI (or run inside the DDK container)}"

DEVICE_SYMVERS="$DEVICE/Module.symvers"
WANT_VERMAGIC="$(cat "$DEVICE/vermagic.txt")"

[ -f "$KDIR/Makefile" ]   || { echo "KDIR=$KDIR is not a kernel tree" >&2; exit 1; }
[ -f "$DEVICE_SYMVERS" ]  || { echo "missing $DEVICE_SYMVERS" >&2; exit 1; }

echo "KDIR        : $KDIR"
echo "kernelrel   : $(make -s -C "$KDIR" kernelrelease 2>/dev/null || echo '?')"
echo "module src  : $MODSRC"
echo "device      : $(head -c 200 "$DEVICE/vermagic.txt")"

# --------------------------------------------------------------------------
# 1. device CRCs win over the tree's
# --------------------------------------------------------------------------
# modpost's read_dump() is strict: every line must be tab separated and start
# with a hex CRC, and a single comment or blank line aborts the whole build with
# "parse error in symbol dump file". So build the merged file from validated
# lines only, and report anything that had to be dropped.
SYMVERS_FILTER='BEGIN{FS="\t"} NF>=3 && $1 ~ /^0x[0-9a-fA-F]+$/ {print}'

if [ -f "$KDIR/Module.symvers" ]; then
    MERGED="$(mktemp)"
    awk -F'\t' 'NR==FNR { if (NF>=3) ours[$2]=1; next } { if (NF>=3 && !($2 in ours)) print }' \
        "$DEVICE_SYMVERS" "$KDIR/Module.symvers" \
        | awk "$SYMVERS_FILTER" > "$MERGED" || true
    awk "$SYMVERS_FILTER" "$DEVICE_SYMVERS" >> "$MERGED" || true

    dropped_tree=$(awk 'BEGIN{FS="\t"} !(NF>=3 && $1 ~ /^0x[0-9a-fA-F]+$/)' "$KDIR/Module.symvers" | wc -l)
    echo "      tree symvers: $(wc -l < "$KDIR/Module.symvers") lines, $dropped_tree dropped as malformed"
    echo "      sample (cat -A, first line): $(head -1 "$KDIR/Module.symvers" | cat -A | head -c 120)"
    echo "      merged head: $(head -1 "$MERGED")"
    echo "      merged tail: $(tail -1 "$MERGED")"

    cp -f "$KDIR/Module.symvers" "$KDIR/Module.symvers.tree-backup" 2>/dev/null || true
    cp -f "$MERGED" "$KDIR/Module.symvers"
    rm -f "$MERGED"
    echo "[1/4] merged symvers: $(wc -l < "$KDIR/Module.symvers") entries (device CRCs override)"
else
    cp -f "$DEVICE_SYMVERS" "$KDIR/Module.symvers"
    echo "[1/4] tree had no Module.symvers; installed the device's"
fi

# --------------------------------------------------------------------------
# 2. build
# --------------------------------------------------------------------------
rm -f "$MODSRC/vcam.ko"
make -C "$KDIR" M="$MODSRC" modules -j"$(nproc)"
BUILT="$MODSRC/vcam.ko"
[ -f "$BUILT" ] || { echo "build produced no $BUILT" >&2; exit 1; }
mkdir -p "$OUT"
cp -f "$BUILT" "$OUT/vcam.ko"
echo "[2/4] built $OUT/vcam.ko ($(stat -c%s "$OUT/vcam.ko") bytes)"

# --------------------------------------------------------------------------
# 3. vermagic
# --------------------------------------------------------------------------
BUILT_VM="$(modinfo -F vermagic "$OUT/vcam.ko" 2>/dev/null || true)"
echo "[3/4] built vermagic : $BUILT_VM"
echo "      wanted vermagic: $WANT_VERMAGIC"
if [ "$BUILT_VM" != "$WANT_VERMAGIC" ]; then
    python3 - "$OUT/vcam.ko" "$WANT_VERMAGIC" <<'PY'
import struct, sys

def sections(data):
    shoff = struct.unpack_from('<Q', data, 0x28)[0]
    entsize = struct.unpack_from('<H', data, 0x3A)[0]
    num = struct.unpack_from('<H', data, 0x3C)[0]
    strndx = struct.unpack_from('<H', data, 0x3E)[0]
    def sh(i):
        b = shoff + i * entsize
        name = struct.unpack_from('<I', data, b)[0]
        off, size = struct.unpack_from('<QQ', data, b + 0x18)
        return name, off, size
    _, soff, ssize = sh(strndx)
    strtab = data[soff:soff + ssize]
    out = {}
    for i in range(num):
        n, off, size = sh(i)
        out[strtab[n:strtab.find(b'\0', n)].decode()] = (off, size)
    return out

ko, want = sys.argv[1], sys.argv[2]
data = bytearray(open(ko, 'rb').read())
off, size = sections(data)['.modinfo']
blob = bytes(data[off:off + size])
key = b'vermagic='
i = blob.find(key)
assert i >= 0, 'no vermagic field in .modinfo'
start = off + i + len(key)
end = data.find(b'\0', start)
room = end - start
assert len(want) <= room, 'vermagic does not fit (%d > %d)' % (len(want), room)
data[start:start + len(want)] = want.encode()
data[start + len(want):end] = b'\0' * (room - len(want))
open(ko, 'wb').write(data)
print('      vermagic field rewritten in place (%d bytes of room)' % room)
PY
    echo "      now: $(modinfo -F vermagic "$OUT/vcam.ko")"
fi

# --------------------------------------------------------------------------
# 4. verify the CRCs the kernel will check
# --------------------------------------------------------------------------
echo "[4/4] verification"
modinfo "$OUT/vcam.ko" | sed -n '1,10p'
python3 - "$OUT/vcam.ko" "$DEVICE_SYMVERS" <<'PY'
import struct, sys

def sections(data):
    shoff = struct.unpack_from('<Q', data, 0x28)[0]
    entsize = struct.unpack_from('<H', data, 0x3A)[0]
    num = struct.unpack_from('<H', data, 0x3C)[0]
    strndx = struct.unpack_from('<H', data, 0x3E)[0]
    def sh(i):
        b = shoff + i * entsize
        name = struct.unpack_from('<I', data, b)[0]
        off, size = struct.unpack_from('<QQ', data, b + 0x18)
        return name, off, size
    _, soff, ssize = sh(strndx)
    strtab = data[soff:soff + ssize]
    out = {}
    for i in range(num):
        n, off, size = sh(i)
        out[strtab[n:strtab.find(b'\0', n)].decode()] = (off, size)
    return out

ko, symvers = sys.argv[1], sys.argv[2]
want = {}
for line in open(symvers):
    if line.startswith('#'):
        continue
    p = line.split()
    if len(p) >= 2:
        want[p[1]] = int(p[0], 16)

data = open(ko, 'rb').read()
sec = sections(data)
if '__versions' not in sec:
    print('  no __versions section -- was the tree built without CONFIG_MODVERSIONS?')
    sys.exit(1)
off, size = sec['__versions']
ok = bad = unknown = 0
for e in range(0, size - 63, 64):
    crc = struct.unpack_from('<Q', data, off + e)[0]
    name = data[off + e + 8:off + e + 64].split(b'\0', 1)[0].decode()
    if not name:
        continue
    if name not in want:
        unknown += 1
        continue
    if want[name] == crc:
        ok += 1
    else:
        bad += 1
        print('  MISMATCH %-26s module=0x%08x device=0x%08x' % (name, crc, want[name]))
print('  %d CRC(s) match the device, %d mismatch, %d not in the device profile'
      % (ok, bad, unknown))
sys.exit(1 if bad else 0)
PY

echo
echo "artifact: $OUT/vcam.ko"
echo "load with:"
echo "  adb push $OUT/vcam.ko /data/local/tmp/ && adb shell su -c 'insmod /data/local/tmp/vcam.ko slots=3 backend=0'"
echo "  adb shell su -c 'dmesg | tail -5'   # expect: vcam: ready: /dev/vcam slots=3 backend=0 abi=1"
