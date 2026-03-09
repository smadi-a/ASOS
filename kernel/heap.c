/*
 * kernel/heap.c — Kernel heap allocator.
 *
 * Uses a doubly-linked free list of variable-size blocks.
 * Each block has an 32-byte header immediately before the user data:
 *
 *   struct HeapBlock {
 *       size_t   size;     // usable bytes (not counting header)
 *       int      is_free;
 *       HeapBlock *next;
 *       HeapBlock *prev;
 *   };
 *
 * kmalloc() does a first-fit search and splits the block if the
 * remainder is large enough (>= 32 bytes of usable space + header).
 * kfree() marks the block free and coalesces with adjacent free blocks.
 * All returned pointers are 16-byte aligned.
 */

#include "heap.h"
#include "vmm.h"
#include "pmm.h"
#include "panic.h"
#include "serial.h"
#include <stdint.h>

#define HEAP_INITIAL_FRAMES  64     /* 64 × 4 KB = 256 KB */
#define HEAP_ALIGN           16U
#define SPLIT_THRESHOLD      32U    /* min usable bytes in remainder block */

typedef struct HeapBlock {
    size_t           size;     /* usable bytes after header */
    int              is_free;
    struct HeapBlock *next;
    struct HeapBlock *prev;
} HeapBlock;

#define HEADER_SIZE  (sizeof(HeapBlock))   /* 32 bytes on x86-64 */

static HeapBlock *g_head = NULL;

/* ── heap_init ────────────────────────────────────────────────────────────*/

void heap_init(void)
{
    uint64_t heap_va = KERNEL_HEAP_START;

    /* Map HEAP_INITIAL_FRAMES pages. */
    for (int i = 0; i < HEAP_INITIAL_FRAMES; i++) {
        uint64_t phys = pmm_alloc_frame();
        vmm_map_page(heap_va + (uint64_t)i * PMM_PAGE_SIZE,
                     phys,
                     PTE_WRITABLE);
    }

    uint64_t heap_size = (uint64_t)HEAP_INITIAL_FRAMES * PMM_PAGE_SIZE;

    /* Place the first (and only) block header at the start of the heap. */
    g_head = (HeapBlock *)(uintptr_t)heap_va;
    g_head->size    = (size_t)(heap_size - HEADER_SIZE);
    g_head->is_free = 1;
    g_head->next    = NULL;
    g_head->prev    = NULL;

    serial_puts("[HEAP] Kernel heap: 256 KB at 0xFFFFFFFF90000000.\n");
}

/* ── kmalloc ──────────────────────────────────────────────────────────────*/

void *kmalloc(size_t size)
{
    if (size == 0)
        return NULL;

    /* Round up to 16-byte alignment. */
    size = (size + HEAP_ALIGN - 1) & ~(size_t)(HEAP_ALIGN - 1);

    /* First-fit search. */
    for (HeapBlock *b = g_head; b != NULL; b = b->next) {
        if (!b->is_free || b->size < size)
            continue;

        /* Can we split? */
        if (b->size >= size + HEADER_SIZE + SPLIT_THRESHOLD) {
            /* Carve a new free block from the tail. */
            HeapBlock *tail = (HeapBlock *)((uint8_t *)(b + 1) + size);
            tail->size    = b->size - size - HEADER_SIZE;
            tail->is_free = 1;
            tail->next    = b->next;
            tail->prev    = b;
            if (b->next) b->next->prev = tail;
            b->next  = tail;
            b->size  = size;
        }

        b->is_free = 0;
        return (void *)(b + 1);
    }

    kpanic("kmalloc: heap exhausted");
}

/* ── kfree ────────────────────────────────────────────────────────────────*/

void kfree(void *ptr)
{
    if (!ptr) return;

    HeapBlock *b = (HeapBlock *)ptr - 1;
    b->is_free = 1;

    /* Coalesce with next block. */
    if (b->next && b->next->is_free) {
        b->size += HEADER_SIZE + b->next->size;
        b->next  = b->next->next;
        if (b->next) b->next->prev = b;
    }

    /* Coalesce with previous block. */
    if (b->prev && b->prev->is_free) {
        b->prev->size += HEADER_SIZE + b->size;
        b->prev->next  = b->next;
        if (b->next) b->next->prev = b->prev;
    }
}
