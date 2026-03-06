# ASOS — Top-level build system
#
# Milestone 1: boot and print.
#
# ┌─────────────────────────────────────────────────────────────────────────┐
# │  Prerequisites                                                          │
# │                                                                         │
# │  Bootloader:                                                            │
# │    • gcc (host, x86-64)          — to compile the UEFI application     │
# │    • gnu-efi development headers and libraries                          │
# │      Ubuntu/Debian: apt install gnu-efi                                 │
# │      Arch:          pacman -S gnu-efi-libs                              │
# │    • objcopy (binutils)          — to convert ELF → PE32+ .efi         │
# │                                                                         │
# │  Kernel:                                                                │
# │    • x86_64-elf-gcc cross-compiler (freestanding, no sysroot)          │
# │      See docs/cross-compiler.md for build instructions, or:            │
# │      macOS (Homebrew): brew install x86_64-elf-gcc                      │
# │      Ubuntu: build from source (binutils + gcc with --target=x86_64-elf)│
# │    • x86_64-elf-ld  (part of x86_64-elf-binutils)                      │
# │                                                                         │
# │  Disk image:                                                            │
# │    • mtools (mformat, mmd, mcopy) — FAT operations without root        │
# │      apt install mtools  /  pacman -S mtools                            │
# │    • parted  — for GPT partition table                                  │
# │                                                                         │
# │  Testing:                                                               │
# │    • qemu-system-x86_64                                                 │
# │    • OVMF firmware (UEFI for QEMU)                                      │
# │      apt install ovmf  /  pacman -S edk2-ovmf                           │
# └─────────────────────────────────────────────────────────────────────────┘
#
# Usage:
#   make           Build everything and create the bootable disk image.
#   make run       Launch QEMU; serial output appears on stdout.
#   make clean     Remove all build artefacts.

# ── Cross-compiler ─────────────────────────────────────────────────────────

# Kernel cross-compiler (freestanding x86-64-elf target).
# Override on the command line if your toolchain has a different prefix:
#   make CROSS=x86_64-linux-elf-
CROSS   ?= x86_64-elf-
CC      := $(CROSS)gcc
LD      := $(CROSS)ld

# Host compiler/tools (for the UEFI bootloader).
HOST_CC      := gcc
HOST_OBJCOPY := objcopy

# ── GNU-EFI paths ──────────────────────────────────────────────────────────
#
# Adjust these if gnu-efi is installed in a non-standard location.
# On Ubuntu/Debian the defaults below are correct after `apt install gnu-efi`.

GNUEFI_INC  ?= /usr/include/efi
GNUEFI_LIB  ?= /usr/lib
GNUEFI_CRT0 ?= $(GNUEFI_LIB)/crt0-efi-x86_64.o
GNUEFI_LDS  ?= $(GNUEFI_LIB)/elf_x86_64_efi.lds

# ── OVMF paths ─────────────────────────────────────────────────────────────
#
# Try a few common locations; override with `make OVMF=/path/to/OVMF.fd`.

ifeq ($(OVMF),)
  # Try locations in order; use the first one that exists.
  OVMF_CANDIDATES := \
      /usr/share/ovmf/x64/OVMF.fd \
      /usr/share/OVMF/OVMF_CODE.fd \
      /usr/share/edk2/x64/OVMF.fd \
      /usr/share/edk2-ovmf/OVMF_CODE.fd
  OVMF := $(firstword $(foreach f,$(OVMF_CANDIDATES),$(wildcard $(f))))
endif

# ── Build output directory ─────────────────────────────────────────────────

BUILD  := build

# Sub-directories created on demand.
BUILD_BL := $(BUILD)/bootloader
BUILD_KN := $(BUILD)/kernel

# ── Disk image ─────────────────────────────────────────────────────────────

DISK_IMG   := $(BUILD)/asos.img
IMG_SIZE   := 64   # MiB — plenty for a FAT32 ESP + tiny kernel

# The ESP partition starts at sector 2048 (1 MiB offset) by convention.
# mtools addresses a partition inside a raw image with the @@byte_offset
# syntax; 2048 sectors × 512 bytes/sector = 1 048 576 bytes = 1 MiB.
ESP_OFFSET := 1048576

# ── Bootloader build flags ─────────────────────────────────────────────────

BL_CFLAGS := \
    -I$(GNUEFI_INC) \
    -I$(GNUEFI_INC)/x86_64 \
    -I$(GNUEFI_INC)/protocol \
    -Wall \
    -Wextra \
    -std=c11 \
    -O2 \
    -fno-stack-protector \
    -fpic \
    -fshort-wchar \
    -mno-red-zone \
    -DEFI_FUNCTION_WRAPPER \
    -DGNU_EFI_USE_MS_ABI

# ── Kernel build flags ─────────────────────────────────────────────────────

KERNEL_CFLAGS := \
    -Wall \
    -Wextra \
    -std=c11 \
    -O2 \
    -ffreestanding \
    -nostdlib \
    -mno-red-zone \
    -mno-mmx \
    -mno-sse \
    -mno-sse2 \
    -mcmodel=kernel \
    -I.

# Kernel linker flags (ld, not gcc).
KERNEL_LDFLAGS := \
    -T linker.ld \
    -nostdlib \
    -z max-page-size=0x1000

# ── Source and object lists ────────────────────────────────────────────────

BL_SRC  := bootloader/main.c
BL_OBJ  := $(BUILD_BL)/main.o
BL_SO   := $(BUILD_BL)/bootloader.so
BL_EFI  := $(BUILD_BL)/BOOTX64.EFI

KERNEL_SRCS := \
    kernel/main.c \
    kernel/serial.c \
    kernel/framebuffer.c \
    kernel/font.c

# Map kernel/foo.c → build/kernel/foo.o
KERNEL_OBJS := $(patsubst kernel/%.c, $(BUILD_KN)/%.o, $(KERNEL_SRCS))

KERNEL_ELF := $(BUILD)/kernel.elf

# ── Top-level targets ──────────────────────────────────────────────────────

.PHONY: all run clean check-tools

all: check-tools $(DISK_IMG)
	@echo ""
	@echo "Build complete."
	@echo "  Disk image : $(DISK_IMG)"
	@echo "  Run with  : make run"

# ── Tool availability check ────────────────────────────────────────────────

check-tools:
	@command -v $(CC)          >/dev/null 2>&1 || \
	    { echo "ERROR: $(CC) not found. Install x86_64-elf-gcc cross-compiler."; exit 1; }
	@command -v $(HOST_CC)     >/dev/null 2>&1 || \
	    { echo "ERROR: $(HOST_CC) not found."; exit 1; }
	@command -v $(HOST_OBJCOPY)>/dev/null 2>&1 || \
	    { echo "ERROR: $(HOST_OBJCOPY) (binutils) not found."; exit 1; }
	@test -f $(GNUEFI_CRT0) || \
	    { echo "ERROR: GNU-EFI not found (looked for $(GNUEFI_CRT0)). Install gnu-efi."; exit 1; }
	@command -v mformat        >/dev/null 2>&1 || \
	    { echo "ERROR: mtools not found. Install mtools."; exit 1; }
	@command -v parted         >/dev/null 2>&1 || \
	    { echo "ERROR: parted not found. Install parted."; exit 1; }

# ── Directory creation ─────────────────────────────────────────────────────

$(BUILD_BL) $(BUILD_KN):
	mkdir -p $@

# ── Bootloader ─────────────────────────────────────────────────────────────
#
# Three steps:
#   1. Compile to a position-independent object file.
#   2. Link into a shared library using gnu-efi's linker script + CRT stub.
#   3. Strip the ELF wrapper with objcopy to produce a PE32+ .efi binary.

$(BL_OBJ): $(BL_SRC) shared/boot_info.h | $(BUILD_BL)
	$(HOST_CC) $(BL_CFLAGS) -c $< -o $@

$(BL_SO): $(BL_OBJ) | $(BUILD_BL)
	ld \
	    -shared \
	    -Bsymbolic \
	    -L$(GNUEFI_LIB) \
	    -T$(GNUEFI_LDS) \
	    $(GNUEFI_CRT0) \
	    $< \
	    -lefi -lgnuefi \
	    -o $@

$(BL_EFI): $(BL_SO) | $(BUILD_BL)
	$(HOST_OBJCOPY) \
	    -j .text \
	    -j .sdata \
	    -j .data \
	    -j .dynamic \
	    -j .dynsym \
	    -j .rel \
	    -j .rela \
	    -j .reloc \
	    --target=efi-app-x86_64 \
	    $< $@

# ── Kernel ─────────────────────────────────────────────────────────────────

$(BUILD_KN)/%.o: kernel/%.c | $(BUILD_KN)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_ELF): $(KERNEL_OBJS) linker.ld
	$(LD) $(KERNEL_LDFLAGS) $(KERNEL_OBJS) -o $@

# ── Disk image ─────────────────────────────────────────────────────────────
#
# Layout of asos.img (raw GPT image):
#
#   Bytes 0 – 1 023         GPT protective MBR + GPT header
#   Bytes 1 048 576+         EFI System Partition (FAT32)
#     /EFI/BOOT/BOOTX64.EFI — bootloader (default UEFI boot path)
#     /EFI/asos/kernel.elf  — kernel binary loaded by the bootloader
#
# mtools' @@ syntax lets us operate on the FAT partition inside the raw
# image without needing a loop device or root privileges.

$(DISK_IMG): $(BL_EFI) $(KERNEL_ELF) | $(BUILD)
	@echo "Creating disk image $(DISK_IMG) ($(IMG_SIZE) MiB)..."

	# 1. Allocate a blank image.
	dd if=/dev/zero of=$(DISK_IMG) bs=1M count=$(IMG_SIZE) status=none

	# 2. Write a GPT partition table with one EFI System Partition.
	parted -s $(DISK_IMG) mklabel gpt
	parted -s $(DISK_IMG) mkpart ESP fat32 2048s 100%
	parted -s $(DISK_IMG) set 1 esp on

	# 3. Format the partition as FAT32 using mtools (no root needed).
	#    The @@$(ESP_OFFSET) suffix tells mtools to access the raw image
	#    at the given byte offset, i.e., skip over the GPT header region.
	mformat -i $(DISK_IMG)@@$(ESP_OFFSET) -F -v "ASOS" ::

	# 4. Create the required directory tree.
	mmd -i $(DISK_IMG)@@$(ESP_OFFSET) \
	    :: ::/EFI ::/EFI/BOOT ::/EFI/asos

	# 5. Copy the bootloader to the default UEFI boot path.
	mcopy -i $(DISK_IMG)@@$(ESP_OFFSET) $(BL_EFI) ::/EFI/BOOT/BOOTX64.EFI

	# 6. Copy the kernel ELF.
	mcopy -i $(DISK_IMG)@@$(ESP_OFFSET) $(KERNEL_ELF) ::/EFI/asos/kernel.elf

	@echo "Disk image ready: $(DISK_IMG)"

# ── QEMU ───────────────────────────────────────────────────────────────────
#
# Flags:
#   -drive if=pflash ...  — OVMF firmware as a pflash device (UEFI)
#   -drive file=...       — our disk image
#   -serial stdio         — route COM1 to the terminal; kernel prints appear here
#   -m 256M               — 256 MiB RAM (more than enough for Milestone 1)
#   -nographic            — no graphical window (serial-only output)
#
# Remove -nographic and add -vga std if you want to see the framebuffer
# in a QEMU window.

run: $(DISK_IMG)
ifeq ($(OVMF),)
	$(error OVMF firmware not found. Install ovmf/edk2 or set OVMF=/path/to/OVMF.fd)
endif
	qemu-system-x86_64 \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF) \
	    -drive file=$(DISK_IMG),format=raw,if=ide,index=0,media=disk \
	    -m 256M \
	    -serial stdio \
	    -nographic \
	    -no-reboot

# ── VirtualBox disk image ──────────────────────────────────────────────────

VDI := $(BUILD)/asos.vdi

vdi: $(VDI)

$(VDI): $(DISK_IMG)
	@# VBoxManage refuses to overwrite an existing VDI, so remove it first.
	rm -f $(VDI)
	VBoxManage convertfromraw $< $@ --format VDI
	@echo "VDI ready: $(VDI)"

# ── Clean ──────────────────────────────────────────────────────────────────

clean:
	rm -rf $(BUILD)
