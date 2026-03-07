#!/usr/bin/env bash
# tools/mkdatadisk.sh — Create a 32 MiB raw FAT32 data disk for ASOS.
#
# Usage:
#   tools/mkdatadisk.sh [output_path]
#
# Default output: build/datadisk.img
#
# The resulting image is a raw FAT32 volume (no partition table).  It is
# intended to be attached as the second IDE disk in QEMU:
#   -drive file=build/datadisk.img,format=raw,if=ide,index=1,media=disk
#
# Requires: mformat, mcopy (mtools)
# Env vars MFORMAT and MCOPY can override tool paths (set by Makefile).

set -euo pipefail

OUT="${1:-build/datadisk.img}"
SIZE_MB=32
MFORMAT="${MFORMAT:-mformat}"
MCOPY="${MCOPY:-mcopy}"
TMPDIR_LOCAL="$(mktemp -d)"

cleanup() { rm -rf "$TMPDIR_LOCAL"; }
trap cleanup EXIT

# ── Create blank image ─────────────────────────────────────────────────────

mkdir -p "$(dirname "$OUT")"
dd if=/dev/zero of="$OUT" bs=1M count=$SIZE_MB status=none

# ── Format as FAT32 (superfloppy — no partition table) ─────────────────────
#
# mformat -F  forces FAT32 regardless of size.
# mformat -v  sets the volume label.

"$MFORMAT" -i "$OUT" -F -v "ASOSDATA" ::

# ── Create test files ──────────────────────────────────────────────────────

# HELLO.TXT — small file (< one cluster)
cat > "$TMPDIR_LOCAL/HELLO.TXT" << 'EOF'
Hello from ASOS FAT32!
This file was read via ATA PIO + FAT32.
EOF

# TEST.TXT — one-liner
printf 'ASOS Milestone 5: ATA + FAT32 working.\n' > "$TMPDIR_LOCAL/TEST.TXT"

# LARGE.TXT — spans multiple clusters (> 4 KB)
{
    for i in $(seq 1 120); do
        printf 'Line %04d: The quick brown fox jumps over the lazy dog. ASOS!\n' "$i"
    done
} > "$TMPDIR_LOCAL/LARGE.TXT"

# ── Copy files to the image ────────────────────────────────────────────────

"$MCOPY" -i "$OUT" "$TMPDIR_LOCAL/HELLO.TXT" ::HELLO.TXT
"$MCOPY" -i "$OUT" "$TMPDIR_LOCAL/TEST.TXT"  ::TEST.TXT
"$MCOPY" -i "$OUT" "$TMPDIR_LOCAL/LARGE.TXT" ::LARGE.TXT

echo "Data disk created: $OUT (${SIZE_MB} MiB, FAT32)"
echo "  HELLO.TXT : $(wc -c < "$TMPDIR_LOCAL/HELLO.TXT") bytes"
echo "  TEST.TXT  : $(wc -c < "$TMPDIR_LOCAL/TEST.TXT") bytes"
echo "  LARGE.TXT : $(wc -c < "$TMPDIR_LOCAL/LARGE.TXT") bytes"
