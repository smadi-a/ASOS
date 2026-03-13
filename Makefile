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

# Kernel compiler.
#
# If a proper x86_64-elf cross-compiler is installed, set CROSS:
#   make CROSS=x86_64-elf-
#
# On an x86_64 Linux host the host gcc/ld work fine because the kernel
# is compiled with -ffreestanding -nostdlib and linked with our own
# linker script — no host sysroot or C runtime is involved.
# The default below uses the host toolchain for convenience.
CROSS   ?=
CC      := $(CROSS)gcc
LD      := $(CROSS)ld

# Host compiler/tools (for the UEFI bootloader).
HOST_CC      := gcc
HOST_OBJCOPY := objcopy

# ── GNU-EFI paths ──────────────────────────────────────────────────────────
#
# Adjust these if gnu-efi is installed in a non-standard location.
# On Ubuntu/Debian the defaults below are correct after `apt install gnu-efi`.

# Local sysroot used when packages are extracted without system install.
# Kept outside build/ so that `make clean` does not delete it.
# Auto-populated by the `deps` target (requires apt-get download + dpkg -x).
LOCAL_SYSROOT := $(CURDIR)/deps/sysroot

# Prefer system install; fall back to local sysroot.
ifneq ($(wildcard /usr/lib/crt0-efi-x86_64.o),)
  GNUEFI_INC  ?= /usr/include/efi
  GNUEFI_LIB  ?= /usr/lib
else
  GNUEFI_INC  ?= $(LOCAL_SYSROOT)/usr/include/efi
  GNUEFI_LIB  ?= $(LOCAL_SYSROOT)/usr/lib
endif
GNUEFI_CRT0 ?= $(GNUEFI_LIB)/crt0-efi-x86_64.o
GNUEFI_LDS  ?= $(GNUEFI_LIB)/elf_x86_64_efi.lds

# ── OVMF paths ─────────────────────────────────────────────────────────────
#
# Try a few common locations; override with `make OVMF=/path/to/OVMF.fd`.

ifeq ($(OVMF),)
  # Try system locations then the local sysroot (populated by `make deps`).
  OVMF_CANDIDATES := \
      /usr/share/ovmf/x64/OVMF.fd \
      /usr/share/OVMF/OVMF_CODE.fd \
      /usr/share/edk2/x64/OVMF.fd \
      /usr/share/edk2-ovmf/OVMF_CODE.fd \
      $(LOCAL_SYSROOT)/usr/share/ovmf/OVMF.fd
  OVMF := $(firstword $(foreach f,$(OVMF_CANDIDATES),$(wildcard $(f))))
endif

# QEMU binary — prefer system install, fall back to local sysroot.
QEMU := $(shell command -v qemu-system-x86_64 2>/dev/null || \
                echo /usr/bin/qemu-system-x86_64)

# ── Build output directory ─────────────────────────────────────────────────

BUILD  := build

# Sub-directories created on demand.
BUILD_BL := $(BUILD)/bootloader
BUILD_KN := $(BUILD)/kernel

# ── Disk image ─────────────────────────────────────────────────────────────

DISK_IMG   := $(BUILD)/asos.img
VDI        := $(BUILD)/asos.vdi
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
    -fno-pic \
    -fno-pie \
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

KERNEL_C_SRCS := \
    kernel/main.c \
    kernel/serial.c \
    kernel/framebuffer.c \
    kernel/font.c \
    kernel/gdt.c \
    kernel/tss.c \
    kernel/idt.c \
    kernel/isr.c \
    kernel/string.c \
    kernel/panic.c \
    kernel/pmm.c \
    kernel/vmm.c \
    kernel/heap.c \
    kernel/pic.c \
    kernel/pit.c \
    kernel/ring_buffer.c \
    kernel/keyboard.c \
    kernel/mouse.c \
    kernel/ata.c \
    kernel/gpt.c \
    kernel/fat32.c \
    kernel/vfs.c \
    kernel/process.c \
    kernel/scheduler.c \
    kernel/syscall.c \
    kernel/elf.c \
    kernel/gfx.c \
    kernel/wm.c \
    kernel/power.c

KERNEL_ASM_SRCS := \
    kernel/gdt_flush.asm \
    kernel/isr_stubs.asm \
    kernel/context_switch.asm \
    kernel/syscall_entry.asm

KERNEL_C_OBJS   := $(patsubst kernel/%.c,   $(BUILD_KN)/%.o, $(KERNEL_C_SRCS))
KERNEL_ASM_OBJS := $(patsubst kernel/%.asm, $(BUILD_KN)/%.o, $(KERNEL_ASM_SRCS))
KERNEL_OBJS     := $(KERNEL_C_OBJS) $(KERNEL_ASM_OBJS)

KERNEL_ELF := $(BUILD)/kernel.elf

# ── Top-level targets ──────────────────────────────────────────────────────

# Delete partially-built targets if a recipe fails, preventing stale
# outputs from causing silent skips on the next make invocation.
.DELETE_ON_ERROR:

# ── User programs ──────────────────────────────────────────────────────

USER_DIR  := user
USER_ELFS := $(USER_DIR)/hello.elf $(USER_DIR)/shell.elf \
             $(USER_DIR)/echo.elf $(USER_DIR)/cat.elf \
             $(USER_DIR)/gfxtest.elf $(USER_DIR)/win_test.elf \
             $(USER_DIR)/desktop.elf $(USER_DIR)/terminal.elf \
             $(USER_DIR)/calc.elf \
             $(USER_DIR)/pencil.elf

.PHONY: all run clean deps check-tools vdi user-programs

all: check-tools $(VDI)
	@echo ""
	@echo "Build complete."
	@echo "  Boot disk  : $(DISK_IMG)  /  $(VDI)"
	@echo "  QEMU       : make run"
	@echo "  VirtualBox : attach $(VDI) to an IDE controller as Primary Master"
	@echo "               (IMPORTANT: use IDE controller, NOT SATA/AHCI)"

# ── Tool availability check ────────────────────────────────────────────────

# mtools and parted may live in the local sysroot if not system-installed.
MFORMAT := $(shell command -v mformat 2>/dev/null || echo $(LOCAL_SYSROOT)/usr/bin/mformat)
MMD     := $(shell command -v mmd     2>/dev/null || echo $(LOCAL_SYSROOT)/usr/bin/mmd)
MCOPY   := $(shell command -v mcopy   2>/dev/null || echo $(LOCAL_SYSROOT)/usr/bin/mcopy)
PARTED  := $(shell command -v parted  2>/dev/null || echo $(LOCAL_SYSROOT)/usr/sbin/parted)

check-tools:
	@command -v $(CC)          >/dev/null 2>&1 || \
	    { echo "ERROR: $(CC) not found. On an x86_64 Linux host run: make CROSS="; exit 1; }
	@command -v $(HOST_CC)     >/dev/null 2>&1 || \
	    { echo "ERROR: $(HOST_CC) not found."; exit 1; }
	@command -v $(HOST_OBJCOPY)>/dev/null 2>&1 || \
	    { echo "ERROR: $(HOST_OBJCOPY) (binutils) not found."; exit 1; }
	@command -v nasm           >/dev/null 2>&1 || \
	    { echo "ERROR: nasm not found. Install nasm."; exit 1; }
	@test -f $(GNUEFI_CRT0) || \
	    { echo "ERROR: GNU-EFI not found (looked for $(GNUEFI_CRT0)). Install gnu-efi."; exit 1; }
	@test -x $(MFORMAT) || \
	    { echo "ERROR: mformat (mtools) not found."; exit 1; }
	@test -x $(PARTED) || \
	    { echo "ERROR: parted not found."; exit 1; }

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

$(BUILD_KN)/%.o: kernel/%.asm | $(BUILD_KN)
	nasm -f elf64 $< -o $@

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

user-programs:
	$(MAKE) -C $(USER_DIR)

$(DISK_IMG): $(BL_EFI) $(KERNEL_ELF) user-programs | $(BUILD)
	@echo "Creating disk image $(DISK_IMG) ($(IMG_SIZE) MiB)..."

	# 1. Allocate a blank image.
	dd if=/dev/zero of=$(DISK_IMG) bs=1M count=$(IMG_SIZE) status=none

	# 2. Write a GPT partition table with one EFI System Partition.
	$(PARTED) -s $(DISK_IMG) mklabel gpt
	$(PARTED) -s $(DISK_IMG) mkpart ESP fat32 2048s 100%
	$(PARTED) -s $(DISK_IMG) set 1 esp on

	# 3. Format the partition as FAT32 using mtools (no root needed).
	#    The @@$(ESP_OFFSET) suffix tells mtools to access the raw image
	#    at the given byte offset, i.e., skip over the GPT header region.
	$(MFORMAT) -i $(DISK_IMG)@@$(ESP_OFFSET) -F -v "ASOS" ::

	# 4. Create the required directory tree.
	$(MMD) -i $(DISK_IMG)@@$(ESP_OFFSET) \
	    ::/EFI ::/EFI/BOOT ::/EFI/asos

	# 5. Copy the bootloader to the default UEFI boot path.
	$(MCOPY) -i $(DISK_IMG)@@$(ESP_OFFSET) $(BL_EFI) ::/EFI/BOOT/BOOTX64.EFI

	# 6. Copy the kernel ELF.
	$(MCOPY) -i $(DISK_IMG)@@$(ESP_OFFSET) $(KERNEL_ELF) ::/EFI/asos/kernel.elf

	# 7. Write test files into the root of the ESP.
	#    These are read back by the kernel via FAT32 to verify the driver.
	printf 'Hello from ASOS filesystem!\n' > $(BUILD)/HELLO.TXT
	printf 'The quick brown fox jumps over the lazy dog.\n' > $(BUILD)/TEST.TXT
	$(MCOPY) -i $(DISK_IMG)@@$(ESP_OFFSET) $(BUILD)/HELLO.TXT ::HELLO.TXT
	$(MCOPY) -i $(DISK_IMG)@@$(ESP_OFFSET) $(BUILD)/TEST.TXT  ::TEST.TXT

	# 8. Copy user programs.
	$(MCOPY) -i $(DISK_IMG)@@$(ESP_OFFSET) $(USER_DIR)/hello.elf ::HELLO.ELF
	$(MCOPY) -i $(DISK_IMG)@@$(ESP_OFFSET) $(USER_DIR)/shell.elf ::SHELL.ELF
	$(MCOPY) -i $(DISK_IMG)@@$(ESP_OFFSET) $(USER_DIR)/echo.elf  ::ECHO.ELF
	$(MCOPY) -i $(DISK_IMG)@@$(ESP_OFFSET) $(USER_DIR)/cat.elf   ::CAT.ELF
	$(MCOPY) -i $(DISK_IMG)@@$(ESP_OFFSET) $(USER_DIR)/loop.elf     ::LOOP.ELF
	$(MCOPY) -i $(DISK_IMG)@@$(ESP_OFFSET) $(USER_DIR)/gfxtest.elf  ::GFXTEST.ELF
	$(MCOPY) -i $(DISK_IMG)@@$(ESP_OFFSET) $(USER_DIR)/win_test.elf  ::WINTEST.ELF
	$(MCOPY) -i $(DISK_IMG)@@$(ESP_OFFSET) $(USER_DIR)/desktop.elf  ::DESKTOP.ELF
	$(MCOPY) -i $(DISK_IMG)@@$(ESP_OFFSET) $(USER_DIR)/terminal.elf ::TERMINAL.ELF
	$(MCOPY) -i $(DISK_IMG)@@$(ESP_OFFSET) $(USER_DIR)/calc.elf    ::CALC.ELF
	$(MCOPY) -i $(DISK_IMG)@@$(ESP_OFFSET) $(USER_DIR)/pencil.elf ::PENCIL.ELF
	$(MCOPY) -i $(DISK_IMG)@@$(ESP_OFFSET) $(USER_DIR)/note.elf   ::NOTE.ELF
	$(MCOPY) -i $(DISK_IMG)@@$(ESP_OFFSET) $(USER_DIR)/DOOM/asos/doom.elf ::DOOM.ELF

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
	$(error OVMF firmware not found. Run: make deps)
endif
	$(QEMU) \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF) \
	    -drive file=$(DISK_IMG),format=raw,if=ide,index=0,media=disk \
	    -m 512M \
	    -nographic \
	    -no-reboot

# ── VirtualBox disk image ──────────────────────────────────────────────────
#
# Attach the resulting VDI to an IDE controller (Primary Master) in
# VirtualBox.  The kernel's ATA PIO driver targets the primary IDE channel
# (ports 0x1F0-0x1F7).  A SATA/AHCI controller will NOT work.

vdi: $(VDI)

$(VDI): $(DISK_IMG)
	@# VBoxManage refuses to overwrite an existing VDI, so remove it first.
	rm -f $(VDI)
	VBoxManage convertfromraw $< $@ --format VDI
	@echo "VDI ready: $(VDI)"

# ── Local dependency bootstrap ─────────────────────────────────────────────
#
# Downloads gnu-efi and mtools .deb packages and extracts them to deps/sysroot
# without requiring root.  Run this once if the packages are not installed
# system-wide.  `make clean` intentionally does NOT remove deps/ so you do
# not have to re-download on every clean rebuild.

DEPS_STAMP := $(LOCAL_SYSROOT)/.extracted

deps: $(DEPS_STAMP)

$(DEPS_STAMP):
	mkdir -p $(LOCAL_SYSROOT)
	@echo "Downloading dependencies..."
	cd /tmp && apt-get download gnu-efi mtools ovmf qemu-system-x86
	dpkg -x /tmp/gnu-efi_*.deb          $(LOCAL_SYSROOT)
	dpkg -x /tmp/mtools_*.deb           $(LOCAL_SYSROOT)
	dpkg -x /tmp/ovmf_*.deb             $(LOCAL_SYSROOT)
	dpkg -x /tmp/qemu-system-x86_*.deb  $(LOCAL_SYSROOT)
	touch $(DEPS_STAMP)
	@echo "deps/ sysroot ready."

# ── Clean ──────────────────────────────────────────────────────────────────

clean:
	rm -rf $(BUILD)
	$(MAKE) -C $(USER_DIR) clean
	@echo "Note: deps/ sysroot preserved. Run 'rm -rf deps' to remove it too."
