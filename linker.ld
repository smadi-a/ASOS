/*
 * linker.ld — Kernel linker script for ASOS (Milestone 1).
 *
 * The kernel is linked as a flat 64-bit ELF executable.  All sections are
 * placed starting at physical address 0x100000 (1 MiB), which is:
 *
 *   • Above the real-mode interrupt vector table and BIOS data area (0–0x500).
 *   • Above the extended BIOS data area and legacy VGA memory (typically
 *     below 0xA0000).
 *   • Above the original 640 KiB conventional memory limit (0xA0000).
 *   • Above the BIOS ROM shadow area (0xC0000–0xFFFFF on classic BIOS).
 *
 * On UEFI systems the firmware typically does NOT place runtime data at
 * 0x100000, but we tell UEFI explicitly via AllocatePages(AllocateAddress)
 * in the bootloader so a collision is caught at load time, not silently.
 *
 * Future milestone: relocate to a high virtual address (e.g., 0xFFFFFFFF80000000)
 * once we set up our own page tables.
 *
 * Section layout
 * ──────────────
 *  .text   — executable code (read + execute)
 *  .rodata — read-only data: string literals, font table, …
 *  .data   — mutable initialised globals
 *  .bss    — zero-initialised globals (the bootloader zeroes this for us
 *            as part of loading the PT_LOAD segment that covers .bss)
 *
 * Every section is aligned to a 4 KiB page boundary so the kernel can
 * later apply appropriate memory protection attributes to each region.
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

/*
 * ENTRY tells the linker (and therefore the ELF e_entry field) which
 * symbol is the kernel entry point.  The bootloader jumps there directly.
 */
ENTRY(kernel_main)

SECTIONS
{
    /* Load address: 1 MiB. */
    . = 0x100000;

    /*
     * .text — machine code.
     *
     * ALIGN(0x1000) ensures the section starts on a page boundary even if
     * the load address itself is already aligned (it is, but being explicit
     * makes future changes safer).
     */
    .text ALIGN(0x1000) : {
        *(.text)
        *(.text.*)
    }

    /* .rodata — constants, string literals, font glyph table. */
    .rodata ALIGN(0x1000) : {
        *(.rodata)
        *(.rodata.*)
    }

    /* .data — mutable initialised variables. */
    .data ALIGN(0x1000) : {
        *(.data)
        *(.data.*)
    }

    /*
     * .bss — uninitialised (zero-init) data.
     *
     * The bootloader zeroes the entire PT_LOAD segment in memory before
     * copying file data, so .bss is already cleared when the kernel starts.
     *
     * COMMON holds tentative definitions (global variables declared without
     * explicit initialisation in C); place them here too.
     */
    .bss ALIGN(0x1000) : {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }

    /*
     * Discard sections that the kernel does not need and that would only
     * bloat the ELF file or confuse simple ELF loaders.
     */
    /DISCARD/ : {
        *(.comment)       /* Compiler version strings                   */
        *(.eh_frame)      /* C++ exception-handling frame data          */
        *(.eh_frame_hdr)
        *(.note.*)        /* ABI / build-ID notes                       */
        *(.gnu.*)
    }
}
