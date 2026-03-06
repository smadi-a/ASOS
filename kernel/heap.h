/*
 * kernel/heap.h — Kernel heap: kmalloc / kfree.
 *
 * Free-list (first-fit) allocator backed by vmm_map_page() + pmm_alloc_frame().
 * The heap grows from KERNEL_HEAP_START (0xFFFFFFFF90000000) and is
 * initially 64 KB (16 frames).
 *
 * All allocations are 16-byte aligned.
 */

#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>

/* Set up the initial heap region.  Must be called after vmm_init(). */
void  heap_init(void);

/* Allocate at least `size` bytes.  Returns NULL on failure (kpanic). */
void *kmalloc(size_t size);

/* Free a pointer previously returned by kmalloc(). */
void  kfree(void *ptr);

#endif /* HEAP_H */
