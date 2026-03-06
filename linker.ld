/*
 * linker.ld — Kernel linker script for ASOS (Milestone 3: higher-half).
 *
 * The kernel is LINKED at virtual address 0xFFFFFFFF80100000 but LOADED at
 * physical address 0x100000.  The AT() clause in each section tells the
 * linker to place the data at the given LMA (physical) while the VMA
 * (virtual) starts at 0xFFFFFFFF80100000.
 *
 * The bootloader sets up page tables before jumping to the kernel:
 *   • Identity map first 4 GB  (VA == PA, 1 GB pages)
 *   • Higher-half map: VA 0xFFFFFFFF80000000 → PA 0x0 (1 GB page)
 * so that virtual 0xFFFFFFFF80100000 == physical 0x100000.
 *
 * Exported linker symbols (used by PMM and VMM):
 *   __kernel_phys_start  — physical address where kernel begins (0x100000)
 *   __kernel_phys_end    — physical address one past the kernel
 *   __bss_start          — virtual address of BSS start
 *   __bss_end            — virtual address of BSS end
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(kernel_main)

KERNEL_VIRT_BASE = 0xFFFFFFFF80000000;
KERNEL_PHYS_BASE = 0x100000;

SECTIONS
{
    /* Start VMA at the higher-half base + physical load address so that
     * PHYS_TO_VIRT(phys) = phys + KERNEL_VIRT_BASE works correctly. */
    . = KERNEL_VIRT_BASE + KERNEL_PHYS_BASE;

    __kernel_virt_start = .;
    __kernel_phys_start = . - KERNEL_VIRT_BASE;

    .text ALIGN(0x1000) : AT(ADDR(.text) - KERNEL_VIRT_BASE) {
        *(.text)
        *(.text.*)
    }

    .rodata ALIGN(0x1000) : AT(ADDR(.rodata) - KERNEL_VIRT_BASE) {
        *(.rodata)
        *(.rodata.*)
    }

    .data ALIGN(0x1000) : AT(ADDR(.data) - KERNEL_VIRT_BASE) {
        *(.data)
        *(.data.*)
    }

    .bss ALIGN(0x1000) : AT(ADDR(.bss) - KERNEL_VIRT_BASE) {
        __bss_start = .;
        *(COMMON)
        *(.bss)
        *(.bss.*)
        __bss_end = .;
    }

    . = ALIGN(0x1000);
    __kernel_virt_end = .;
    __kernel_phys_end = . - KERNEL_VIRT_BASE;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.note.*)
        *(.gnu.*)
    }
}
