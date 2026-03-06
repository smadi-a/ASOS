/*
 * kernel/vmm.h — Virtual Memory Manager (4-level page table setup).
 *
 * Address space layout after vmm_init():
 *
 *   0x0000000000000000 – 0x00000000FFFFFFFF : Identity map of first 4 GB
 *       Uses 2 MB large pages.  The first 2 MB (0x0 – 0x1FFFFF) is NOT
 *       mapped, so null-pointer dereferences trigger a page fault.
 *
 *   0xFFFFFFFF80000000+                     : Higher-half kernel
 *       The kernel's physical range is mapped here using 4 KB pages.
 *       This is where all kernel code and data live (linker VMA).
 */

#ifndef VMM_H
#define VMM_H

#include <stdint.h>

/* Higher-half base — must match linker.ld KERNEL_VIRT_BASE. */
#define KERNEL_VIRT_BASE   0xFFFFFFFF80000000ULL

/* Heap region starts just above the kernel's reserved higher-half area. */
#define KERNEL_HEAP_START  0xFFFFFFFF90000000ULL

/* Convert between physical and kernel-virtual addresses (kernel range only). */
#define PHYS_TO_VIRT(pa)  ((pa)  + KERNEL_VIRT_BASE)
#define VIRT_TO_PHYS(va)  ((va)  - KERNEL_VIRT_BASE)

/* Page table entry flags. */
#define PTE_PRESENT    (1ULL << 0)
#define PTE_WRITABLE   (1ULL << 1)
#define PTE_USER       (1ULL << 2)
#define PTE_PAGE_SIZE  (1ULL << 7)   /* large page (2 MB at PD level) */
#define PTE_NO_EXEC    (1ULL << 63)

/*
 * Build page tables, switch CR3, and print diagnostics.
 * Must be called after pmm_init().
 */
void vmm_init(void);

/*
 * Map a single 4 KB page: virtual address virt → physical address phys.
 * Allocates intermediate page-table frames via pmm_alloc_frame() as needed.
 * flags: combination of PTE_* constants (PTE_PRESENT is always set).
 */
void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

#endif /* VMM_H */
