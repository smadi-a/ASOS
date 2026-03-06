/*
 * kernel/vmm.c — Virtual Memory Manager.
 *
 * Builds a complete set of 4-level page tables and switches CR3.
 *
 * Identity map (2 MB pages):
 *   PML4[0] → one PDPT with 4 × 1 GB entries → four PDs of 2 MB pages.
 *   Coverage: 0x0000000000200000 – 0x00000000FFFFFFFF
 *   The first 2 MB (0x0–0x1FFFFF) is NOT mapped → null-pointer page fault.
 *
 * Higher-half kernel (4 KB pages):
 *   PML4[511] → PDPT[510] → PD → PT chain.
 *   Coverage: KERNEL_VIRT_BASE + __kernel_phys_start .. __kernel_phys_end
 *
 * CR3 switch safety:
 *   Both the identity map and the higher-half kernel mapping are in place
 *   before we write CR3.  We run at a higher-half VA throughout, so the
 *   instruction fetch still works after the switch.  The caller must have
 *   already switched RSP to a kernel BSS stack (a higher-half VA) before
 *   calling vmm_init(), so the stack remains valid after CR3 changes.
 */

#include "vmm.h"
#include "pmm.h"
#include "panic.h"
#include "string.h"
#include "serial.h"
#include <stdint.h>

/* ── Linker symbols ───────────────────────────────────────────────────────*/

extern char __kernel_phys_start[];
extern char __kernel_phys_end[];

/* ── Page table index extraction ─────────────────────────────────────────*/

static inline uint64_t pml4_idx(uint64_t va) { return (va >> 39) & 0x1FF; }
static inline uint64_t pdpt_idx(uint64_t va) { return (va >> 30) & 0x1FF; }
static inline uint64_t pd_idx  (uint64_t va) { return (va >> 21) & 0x1FF; }
static inline uint64_t pt_idx  (uint64_t va) { return (va >> 12) & 0x1FF; }

/* ── Page table helpers ───────────────────────────────────────────────────*/

/*
 * Return pointer to the child table at table[idx].
 * If not present, allocate a frame and install it with the given flags.
 * The child table must NOT be a large-page entry.
 */
static uint64_t *get_or_create(uint64_t *table, uint64_t idx, uint64_t flags)
{
    if (!(table[idx] & PTE_PRESENT)) {
        uint64_t phys = pmm_alloc_frame();  /* always zeroed */
        table[idx] = phys | flags | PTE_PRESENT;
    }
    uint64_t child_phys = table[idx] & ~0xFFFULL;
    return (uint64_t *)(uintptr_t)child_phys;
}

/* ── PML4 physical address (set by vmm_init, used by vmm_map_page) ───────*/

static uint64_t g_pml4_phys = 0;

/* ── vmm_map_page ─────────────────────────────────────────────────────────*/

void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags)
{
    if (!g_pml4_phys)
        kpanic("vmm_map_page: vmm_init not called");

    uint64_t *pml4 = (uint64_t *)(uintptr_t)g_pml4_phys;
    uint64_t *pdpt = get_or_create(pml4, pml4_idx(virt), PTE_WRITABLE);
    uint64_t *pd   = get_or_create(pdpt, pdpt_idx(virt), PTE_WRITABLE);
    uint64_t *pt   = get_or_create(pd,   pd_idx(virt),   PTE_WRITABLE);

    pt[pt_idx(virt)] = phys | flags | PTE_PRESENT;
    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
}

/* ── vmm_init ─────────────────────────────────────────────────────────────*/

void vmm_init(void)
{
    serial_puts("[VMM] Building page tables...\n");

    /* ── PML4 ────────────────────────────────────────────────────────── */
    uint64_t pml4_phys = pmm_alloc_frame();
    uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_phys;

    /* ── Identity map: first 4 GB with 2 MB pages ───────────────────── */
    /*
     * Layout:
     *   PML4[0]     → id_pdpt (one PDPT for the first 512 GB)
     *   id_pdpt[0]  → id_pd0  (1 GB, physical 0–1 GB)
     *   id_pdpt[1]  → id_pd1  (1 GB, physical 1–2 GB)
     *   id_pdpt[2]  → id_pd2  (1 GB, physical 2–3 GB)
     *   id_pdpt[3]  → id_pd3  (1 GB, physical 3–4 GB)
     *   Each PDn[i] = 2 MB large page at physical gb*1G + i*2M
     */
    uint64_t id_pdpt_phys = pmm_alloc_frame();
    uint64_t *id_pdpt = (uint64_t *)(uintptr_t)id_pdpt_phys;
    pml4[0] = id_pdpt_phys | PTE_WRITABLE | PTE_PRESENT;

    for (int gb = 0; gb < 4; gb++) {
        uint64_t pd_phys = pmm_alloc_frame();
        uint64_t *pd = (uint64_t *)(uintptr_t)pd_phys;
        id_pdpt[gb] = pd_phys | PTE_WRITABLE | PTE_PRESENT;

        for (int mb2 = 0; mb2 < 512; mb2++) {
            /* Skip the first 2 MB block entirely — null-pointer guard. */
            if (gb == 0 && mb2 == 0)
                continue;
            uint64_t phys = (uint64_t)gb * (1024ULL * 1024 * 1024)
                          + (uint64_t)mb2 * (2ULL * 1024 * 1024);
            pd[mb2] = phys | PTE_PAGE_SIZE | PTE_WRITABLE | PTE_PRESENT;
        }
    }

    /* ── Higher-half kernel: 4 KB pages ─────────────────────────────── */
    uint64_t kphys_start = (uint64_t)(uintptr_t)__kernel_phys_start;
    uint64_t kphys_end   = (uint64_t)(uintptr_t)__kernel_phys_end;
    uint64_t kphys_base  =  kphys_start & ~0xFFFULL;
    uint64_t kphys_top   = (kphys_end   + 0xFFFULL) & ~0xFFFULL;

    /* Install PML4[511] before using vmm_map_page() for the kernel. */
    uint64_t hh_pdpt_phys = pmm_alloc_frame();
    pml4[511] = hh_pdpt_phys | PTE_WRITABLE | PTE_PRESENT;

    /* Set g_pml4_phys so vmm_map_page() can find the root. */
    g_pml4_phys = pml4_phys;

    for (uint64_t phys = kphys_base; phys < kphys_top; phys += PMM_PAGE_SIZE) {
        uint64_t virt = phys + KERNEL_VIRT_BASE;
        vmm_map_page(virt, phys, PTE_WRITABLE);
    }

    /* ── Switch CR3 ───────────────────────────────────────────────────── */
    serial_puts("[VMM] Switching CR3...\n");

    __asm__ volatile (
        "mov %0, %%cr3"
        :: "r"(pml4_phys)
        : "memory"
    );

    serial_puts("[VMM] Page tables active. Null guard at 0x0-0x1FFFFF.\n");
}
