/*
 * kernel/tss.c — 64-bit TSS initialisation.
 */

#include "tss.h"
#include <stdint.h>

/* Compile-time size check: the CPU expects exactly 104 bytes. */
_Static_assert(sizeof(TSS) == 104, "TSS size must be 104 bytes");

/* Dedicated stack for the double-fault (#DF) handler.
 * Must be large enough to run the ISR diagnostic code.
 * 16 KiB gives plenty of headroom.                                  */
#define DF_STACK_SIZE (16 * 1024)

static TSS     g_tss                              __attribute__((aligned(16)));
static uint8_t g_df_stack[DF_STACK_SIZE]          __attribute__((aligned(16)));

void tss_init(void)
{
    /* Zero the entire TSS first. */
    uint8_t *p = (uint8_t *)&g_tss;
    for (uint32_t i = 0; i < sizeof(TSS); i++)
        p[i] = 0;

    /*
     * RSP0: will be set to the top of a kernel stack once we have
     * per-process kernel stacks.  For now leave it zero — we have no
     * user-mode code yet so it is never used.
     */
    g_tss.rsp0 = 0;

    /*
     * IST1: dedicated double-fault stack.
     * The pointer must be the *top* of the stack (highest address)
     * because x86-64 stacks grow downward.
     */
    g_tss.ist1 = (uint64_t)(g_df_stack + DF_STACK_SIZE);

    /*
     * IOPB base: setting this to sizeof(TSS) means the I/O permission
     * bitmap starts immediately after the TSS — but since it would be
     * outside the TSS segment limit (limit = sizeof(TSS)-1), the CPU
     * treats all port accesses from ring 3 as denied.  Harmless at
     * ring 0 since IOPL checks are bypassed.
     */
    g_tss.iomap_base = sizeof(TSS);
}

TSS *tss_get(void)
{
    return &g_tss;
}

void tss_set_rsp0(uint64_t rsp0)
{
    g_tss.rsp0 = rsp0;
}
