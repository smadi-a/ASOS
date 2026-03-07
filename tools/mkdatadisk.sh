#!/usr/bin/env bash
# tools/mkdatadisk.sh — RETIRED
#
# The separate data disk approach has been removed.  Test files (HELLO.TXT,
# TEST.TXT) are now written directly into the root of the EFI System
# Partition on the boot disk during `make`.  The kernel reads them via
# GPT partition discovery + FAT32 mounted at the ESP's starting LBA.
#
# This script is kept for reference only and is no longer called by the
# Makefile.
echo "mkdatadisk.sh is retired — test files are now on the ESP." >&2
exit 0
