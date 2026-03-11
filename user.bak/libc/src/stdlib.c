/*
 * stdlib.c — Memory allocation (malloc/free) backed by sbrk.
 *
 * Simple free-list allocator with first-fit strategy.
 * All allocations are 16-byte aligned.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>

/* ── Block header ─────────────────────────────────────────────────────── */

typedef struct block {
    size_t         size;    /* Usable size (excluding header) */
    struct block  *next;    /* Next free block (if free)      */
    int            free;    /* 1 = free, 0 = in use           */
    int            _pad;    /* Pad to 16 bytes                */
} block_t;

#define BLOCK_SIZE  sizeof(block_t)  /* 24 bytes, but we ensure 16-align */
#define ALIGN16(x)  (((x) + 15) & ~(size_t)15)

static block_t *free_list = (void *)0;

/* ── sbrk-based expansion ─────────────────────────────────────────────── */

static block_t *request_space(size_t size)
{
    size_t total = ALIGN16(BLOCK_SIZE + size);
    void *p = sbrk((intptr_t)total);
    if (p == (void *)-1)
        return (void *)0;

    block_t *blk = (block_t *)p;
    blk->size = total - BLOCK_SIZE;
    blk->next = (void *)0;
    blk->free = 0;
    return blk;
}

/* ── Find a free block (first fit) ────────────────────────────────────── */

static block_t *find_free(size_t size)
{
    block_t *cur = free_list;
    while (cur) {
        if (cur->free && cur->size >= size)
            return cur;
        cur = cur->next;
    }
    return (void *)0;
}

/* ── malloc ───────────────────────────────────────────────────────────── */

void *malloc(size_t size)
{
    if (size == 0) return (void *)0;

    size = ALIGN16(size);

    /* Try free list. */
    block_t *blk = find_free(size);
    if (blk) {
        /* Split if there's enough room for another block + 16 bytes. */
        if (blk->size >= size + BLOCK_SIZE + 16) {
            block_t *new_blk = (block_t *)((uint8_t *)blk + BLOCK_SIZE + size);
            new_blk->size = blk->size - size - BLOCK_SIZE;
            new_blk->next = blk->next;
            new_blk->free = 1;
            blk->size = size;
            blk->next = new_blk;
        }
        blk->free = 0;
        return (void *)((uint8_t *)blk + BLOCK_SIZE);
    }

    /* No free block — request more from kernel. */
    blk = request_space(size);
    if (!blk) {
        errno = ENOMEM;
        return (void *)0;
    }

    /* Append to list. */
    if (!free_list) {
        free_list = blk;
    } else {
        block_t *cur = free_list;
        while (cur->next) cur = cur->next;
        cur->next = blk;
    }

    return (void *)((uint8_t *)blk + BLOCK_SIZE);
}

/* ── free ─────────────────────────────────────────────────────────────── */

void free(void *ptr)
{
    if (!ptr) return;

    block_t *blk = (block_t *)((uint8_t *)ptr - BLOCK_SIZE);
    blk->free = 1;

    /* Coalesce with next block if it's also free. */
    if (blk->next && blk->next->free) {
        blk->size += BLOCK_SIZE + blk->next->size;
        blk->next = blk->next->next;
    }
}

/* ── calloc ───────────────────────────────────────────────────────────── */

void *calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    if (nmemb != 0 && total / nmemb != size) {
        errno = ENOMEM;
        return (void *)0;
    }
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

/* ── realloc ──────────────────────────────────────────────────────────── */

void *realloc(void *ptr, size_t size)
{
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return (void *)0; }

    block_t *blk = (block_t *)((uint8_t *)ptr - BLOCK_SIZE);
    if (blk->size >= size)
        return ptr;

    void *new_ptr = malloc(size);
    if (!new_ptr) return (void *)0;
    memcpy(new_ptr, ptr, blk->size);
    free(ptr);
    return new_ptr;
}

/* ── abort ────────────────────────────────────────────────────────────── */

void abort(void)
{
    _exit(127);
}

void exit(int status)
{
    _exit(status);
}

/* ── atoi / atol / abs ────────────────────────────────────────────────── */

int atoi(const char *s)
{
    int neg = 0, val = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }
    while (*s >= '0' && *s <= '9')
        val = val * 10 + (*s++ - '0');
    return neg ? -val : val;
}

long atol(const char *s)
{
    long neg = 0, val = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }
    while (*s >= '0' && *s <= '9')
        val = val * 10 + (*s++ - '0');
    return neg ? -val : val;
}

int abs(int n)
{
    return n < 0 ? -n : n;
}
