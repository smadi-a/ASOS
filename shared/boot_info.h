/*
 * shared/boot_info.h — Boot information passed from bootloader to kernel.
 *
 * This header is included by both the UEFI bootloader (compiled with gnu-efi)
 * and the freestanding kernel.  It must therefore avoid depending on any
 * runtime library other than the integer types, which GCC provides in
 * freestanding mode through <stdint.h>.
 *
 * Layout is identical in both translation units because:
 *   - Both compile for x86-64 LP64
 *   - No padding surprises: all fields are naturally aligned
 */

#ifndef BOOT_INFO_H
#define BOOT_INFO_H

/*
 * In a GNU-EFI build, <efi.h> is included first and the compiler is
 * still x86-64 GCC, so <stdint.h> from the compiler's freestanding
 * headers is available without conflicts.
 */
#include <stdint.h>

/* ── Pixel format ─────────────────────────────────────────────────────────
 *
 * Describes which byte order the GOP framebuffer uses for each 32-bit pixel.
 * The unused byte (alpha / reserved) is always the most-significant byte.
 *
 *   RGB → byte[0]=R  byte[1]=G  byte[2]=B  byte[3]=X
 *   BGR → byte[0]=B  byte[1]=G  byte[2]=R  byte[3]=X
 */
typedef enum {
    PIXEL_FORMAT_RGB,
    PIXEL_FORMAT_BGR,
} PixelFormat;

/* ── Framebuffer descriptor ───────────────────────────────────────────────
 *
 * Describes the linear framebuffer obtained from the UEFI GOP.
 * All pixel data starts at physical address `base`.
 * The pixel at column x, row y is at:
 *
 *   base + y * pitch + x * 4        (4 bytes per pixel, 32-bit colour)
 */
typedef struct {
    uint64_t    base;    /* Physical address of pixel (0,0)              */
    uint32_t    width;   /* Horizontal resolution, in pixels              */
    uint32_t    height;  /* Vertical resolution, in pixels                */
    uint32_t    pitch;   /* Bytes per scanline (>= width * 4)             */
    PixelFormat format;  /* Colour channel order                          */
} Framebuffer;

/* ── UEFI memory map ──────────────────────────────────────────────────────
 *
 * The raw memory map returned by EFI_BOOT_SERVICES.GetMemoryMap().
 * The kernel will walk this to build its own memory manager later.
 *
 * Each entry is `descriptor_size` bytes of type EFI_MEMORY_DESCRIPTOR
 * (which may be larger than sizeof(EFI_MEMORY_DESCRIPTOR) due to
 * firmware-specific extensions — always use descriptor_size to step).
 */
typedef struct {
    void    *map;                /* Pointer to the raw descriptor array   */
    uint64_t map_size;           /* Total size of the array in bytes      */
    uint64_t descriptor_size;    /* Byte stride per descriptor            */
    uint32_t descriptor_version; /* EFI_MEMORY_DESCRIPTOR version field   */
} MemoryMap;

/* ── Top-level boot information ───────────────────────────────────────────
 *
 * Passed as the sole argument to the kernel entry point:
 *
 *   void kernel_main(BootInfo *info);
 */
typedef struct {
    Framebuffer framebuffer;
    MemoryMap   memory_map;
} BootInfo;

#endif /* BOOT_INFO_H */
