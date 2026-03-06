/*
 * kernel/pmm.c — Physical Memory Manager.
 *
 * Bitmap allocator: one bit per 4 KB frame covering the first 4 GB of
 * physical address space.  Bit 0 of byte 0 = frame 0 (PA 0x0).
 * Bit set (1) means the frame is in use; bit clear (0) means free.
 *
 * Initialisation strategy
 * ───────────────────────
 *   1. Set all bits (mark everything used).
 *   2. For each EfiConventionalMemory region, clear bits (mark free).
 *   3. Re-mark the following as used regardless:
 *        • First 1 MB (legacy BIOS, IVT, BDA, etc.)
 *        • Kernel physical range (__kernel_phys_start .. __kernel_phys_end)
 *        • BootInfo struct and its memory-map buffer
 *        • The framebuffer physical region
 *
 * Allocation uses next-fit (scan from last position) to avoid O(n)
 * rescanning of the dense used region at the bottom of RAM.
 */

#include "pmm.h"
#include "panic.h"
#include "string.h"
#include "serial.h"
#include <stdint.h>

/* ── EFI memory descriptor ────────────────────────────────────────────────
 *
 * Matches EFI_MEMORY_DESCRIPTOR (UEFI spec 2.x, §7.2).
 * There is 4-byte implicit padding between type and phys_start.
 */
#define EFI_CONVENTIONAL_MEMORY 7

typedef struct {
    uint32_t type;
    uint32_t _pad;
    uint64_t phys_start;
    uint64_t virt_start;
    uint64_t num_pages;   /* 4 KB pages */
    uint64_t attributes;
} EFIMemDesc;

/* ── Linker-exported physical-address symbols ────────────────────────────*/

extern char __kernel_phys_start[];
extern char __kernel_phys_end[];

/* ── Bitmap storage (128 KB in BSS) ─────────────────────────────────────*/

#define BITMAP_SIZE (PMM_MAX_FRAMES / 8)  /* bytes */

static uint8_t  g_bitmap[BITMAP_SIZE];
static uint64_t g_total_frames = 0;
static uint64_t g_free_frames  = 0;
static uint64_t g_next_frame   = 0;  /* next-fit cursor */

/* ── Bitmap helpers ───────────────────────────────────────────────────────*/

static inline void bitmap_set(uint64_t frame)
{
    g_bitmap[frame / 8] |= (uint8_t)(1u << (frame % 8));
}

static inline void bitmap_clear(uint64_t frame)
{
    g_bitmap[frame / 8] &= (uint8_t)~(1u << (frame % 8));
}

static inline int bitmap_test(uint64_t frame)
{
    return (g_bitmap[frame / 8] >> (frame % 8)) & 1;
}

/* ── Range helpers ────────────────────────────────────────────────────────*/

static void mark_used(uint64_t phys, uint64_t length)
{
    uint64_t first = phys / PMM_PAGE_SIZE;
    uint64_t last  = (phys + length + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    if (last > PMM_MAX_FRAMES) last = PMM_MAX_FRAMES;
    for (uint64_t f = first; f < last; f++) {
        if (!bitmap_test(f)) {
            bitmap_set(f);
            if (g_free_frames) g_free_frames--;
        }
    }
}

static void mark_free(uint64_t phys, uint64_t length)
{
    uint64_t first = phys / PMM_PAGE_SIZE;
    uint64_t last  = (phys + length) / PMM_PAGE_SIZE;
    if (first >= PMM_MAX_FRAMES) return;
    if (last  >  PMM_MAX_FRAMES) last = PMM_MAX_FRAMES;
    for (uint64_t f = first; f < last; f++) {
        if (bitmap_test(f)) {
            bitmap_clear(f);
            g_free_frames++;
        }
    }
}

/* ── Simple decimal output for init stats ────────────────────────────────*/

static void pmm_print_dec(uint64_t v)
{
    if (v == 0) { serial_puts("0"); return; }
    char tmp[20];
    int i = 0;
    while (v) { tmp[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    char out[2] = {0, 0};
    while (i--) { out[0] = tmp[i]; serial_puts(out); }
}

/* ── Public API ───────────────────────────────────────────────────────────*/

void pmm_init(BootInfo *boot_info)
{
    /* 1. Mark everything used. */
    memset(g_bitmap, 0xFF, BITMAP_SIZE);
    g_free_frames  = 0;
    g_total_frames = PMM_MAX_FRAMES;

    MemoryMap *mm = &boot_info->memory_map;

    /* 2. Walk EFI memory map: mark EfiConventionalMemory regions as free. */
    uint8_t *entry = (uint8_t *)mm->map;
    uint8_t *end   = entry + mm->map_size;

    while (entry < end) {
        EFIMemDesc *desc = (EFIMemDesc *)entry;

        if (desc->type == EFI_CONVENTIONAL_MEMORY) {
            uint64_t phys   = desc->phys_start;
            uint64_t length = desc->num_pages * PMM_PAGE_SIZE;
            if (phys < 4ULL * 1024 * 1024 * 1024)
                mark_free(phys, length);
        }

        entry += mm->descriptor_size;
    }

    /* 3a. Reserve first 2 MB.
     *     The first 1 MB is legacy BIOS/IVT/BDA.
     *     We also reserve 1–2 MB so that no frame below 0x200000 is ever
     *     allocated.  This matches the identity map in vmm_init(), which
     *     skips the first 2 MB (null-pointer guard), meaning physical
     *     addresses 0x0–0x1FFFFF are only accessible via the higher-half
     *     kernel mapping.  Since the kernel itself lives in 0x100000+,
     *     keeping frames 0x100000–0x1FFFFF reserved prevents pmm_alloc_frame
     *     from handing out pages that would be inaccessible after vmm_init(). */
    mark_used(0, 0x200000ULL);

    /* 3b. Reserve the kernel's physical range. */
    uint64_t kphys_start = (uint64_t)(uintptr_t)__kernel_phys_start;
    uint64_t kphys_end   = (uint64_t)(uintptr_t)__kernel_phys_end;
    mark_used(kphys_start, kphys_end - kphys_start);

    /* 3c. Reserve the BootInfo struct itself. */
    mark_used((uint64_t)(uintptr_t)boot_info, sizeof(BootInfo));

    /* 3d. Reserve the EFI memory map buffer. */
    mark_used((uint64_t)(uintptr_t)mm->map, mm->map_size);

    /* 3e. Reserve the framebuffer physical region. */
    Framebuffer *fb = &boot_info->framebuffer;
    uint64_t fb_size = (uint64_t)fb->height * fb->pitch;
    if (fb->base < 4ULL * 1024 * 1024 * 1024)
        mark_used(fb->base, fb_size);

    /* Print summary. */
    uint64_t total_mb = (g_free_frames + (PMM_MAX_FRAMES - g_free_frames)) *
                        PMM_PAGE_SIZE / (1024 * 1024);
    uint64_t free_mb  = g_free_frames  * PMM_PAGE_SIZE / (1024 * 1024);
    uint64_t used_mb  = (PMM_MAX_FRAMES - g_free_frames) *
                        PMM_PAGE_SIZE / (1024 * 1024);

    serial_puts("[PMM] Initialised.\n");
    serial_puts("[PMM]   Total: "); pmm_print_dec(total_mb); serial_puts(" MB\n");
    serial_puts("[PMM]   Free : "); pmm_print_dec(free_mb);  serial_puts(" MB\n");
    serial_puts("[PMM]   Used : "); pmm_print_dec(used_mb);  serial_puts(" MB\n");
}

uint64_t pmm_alloc_frame(void)
{
    for (uint64_t i = 0; i < PMM_MAX_FRAMES; i++) {
        uint64_t frame = (g_next_frame + i) % PMM_MAX_FRAMES;
        if (!bitmap_test(frame)) {
            bitmap_set(frame);
            g_free_frames--;
            g_next_frame = (frame + 1) % PMM_MAX_FRAMES;

            /* Zero the frame via identity-mapped physical address. */
            memset((void *)(uintptr_t)(frame * PMM_PAGE_SIZE), 0, PMM_PAGE_SIZE);

            return frame * PMM_PAGE_SIZE;
        }
    }
    kpanic("pmm_alloc_frame: out of physical memory");
}

void pmm_free_frame(uint64_t phys_addr)
{
    uint64_t frame = phys_addr / PMM_PAGE_SIZE;
    if (frame >= PMM_MAX_FRAMES)
        kpanic("pmm_free_frame: address out of range");
    if (!bitmap_test(frame))
        kpanic("pmm_free_frame: double-free detected");
    bitmap_clear(frame);
    g_free_frames++;
}

uint64_t pmm_get_free_count(void)  { return g_free_frames; }
uint64_t pmm_get_total_count(void) { return g_total_frames; }
