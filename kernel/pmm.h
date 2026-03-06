/*
 * kernel/pmm.h — Physical Memory Manager (bitmap frame allocator).
 *
 * Tracks physical frames of 4 KB each.  The bitmap covers up to 4 GB of
 * physical address space (1 048 576 frames, 128 KB of bitmap in BSS).
 *
 * Bit convention: 0 = free, 1 = used/reserved.
 */

#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include "../shared/boot_info.h"

#define PMM_PAGE_SIZE   4096ULL
#define PMM_MAX_FRAMES  (4ULL * 1024 * 1024 * 1024 / PMM_PAGE_SIZE)  /* 1 M frames */

/*
 * Initialise the PMM from the UEFI memory map in BootInfo.
 * Must be called once, before pmm_alloc_frame().
 */
void pmm_init(BootInfo *boot_info);

/*
 * Allocate one 4 KB physical frame.
 * Returns its physical address, zeroed.
 * Calls kpanic() if memory is exhausted.
 */
uint64_t pmm_alloc_frame(void);

/*
 * Release a previously allocated frame back to the free pool.
 * phys_addr must be 4 KB-aligned and within the tracked range.
 */
void pmm_free_frame(uint64_t phys_addr);

/* Diagnostic counters. */
uint64_t pmm_get_free_count(void);
uint64_t pmm_get_total_count(void);

#endif /* PMM_H */
