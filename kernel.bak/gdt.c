/*
 * kernel/gdt.c — GDT construction and loading.
 *
 * Encoding reference (Intel SDM Vol.3A §3.4.5)
 * ──────────────────────────────────────────────
 * A standard 8-byte GDT descriptor contains these fields:
 *
 *  Bit(s)   Field
 *  ───────  ─────────────────────────────────────────────────────────
 *  63:56    Base  [31:24]
 *  55       G    — Granularity (0 = byte, 1 = 4 KiB pages)
 *  54       D/B  — Default operand size (0 = 16-bit, 1 = 32-bit)
 *  53       L    — 64-bit code segment flag (must be 1 for 64-bit code)
 *  52       AVL  — Available for software use
 *  51:48    Limit[19:16]
 *  47       P    — Present
 *  46:45    DPL  — Descriptor Privilege Level (0 = kernel, 3 = user)
 *  44       S    — Descriptor type (0 = system, 1 = code/data)
 *  43       E    — Executable (1 = code segment, 0 = data segment)
 *  42       DC   — Direction/Conforming
 *  41       RW   — Readable (code) / Writable (data)
 *  40       A    — Accessed (set by CPU; we leave it 0)
 *  39:32    Base [23:16]
 *  31:16    Base [15:0]
 *  15:0     Limit[15:0]
 *
 * Access-byte values used below:
 *   0x9A = 1001 1010 — P=1, DPL=0, S=1, E=1, DC=0, RW=1, A=0  (kernel code)
 *   0x92 = 1001 0010 — P=1, DPL=0, S=1, E=0, DC=0, RW=1, A=0  (kernel data)
 *   0xFA = 1111 1010 — P=1, DPL=3, S=1, E=1, DC=0, RW=1, A=0  (user code)
 *   0xF2 = 1111 0010 — P=1, DPL=3, S=1, E=0, DC=0, RW=1, A=0  (user data)
 *
 * Flags nibble:
 *   0xA = 1010 — G=1, D/B=0, L=1, AVL=0  (64-bit code)
 *   0xC = 1100 — G=1, D/B=1, L=0, AVL=0  (32/64-bit data)
 */

#include "gdt.h"
#include "tss.h"

/* 7 slots: null + 4 code/data + 2 × 8-byte halves of the 16-byte TSS entry */
#define GDT_ENTRIES 7

static uint64_t     g_gdt[GDT_ENTRIES];
static GDTDescriptor g_gdt_desc;

/* Assembly routines defined in gdt_flush.asm */
extern void gdt_flush(GDTDescriptor *desc);
extern void tss_flush(uint16_t selector);

/* ── Encoding helpers ─────────────────────────────────────────────────────*/

/*
 * encode_entry — pack a standard 8-byte GDT descriptor.
 *
 * @base   Linear base address (used only for the TSS; code/data use 0)
 * @limit  Segment limit (0xFFFFF for flat 4 GiB with G=1)
 * @access Access byte (see table above)
 * @flags  4-bit flags nibble (G, D/B, L, AVL)
 */
static uint64_t encode_entry(uint32_t base, uint32_t limit,
                              uint8_t access, uint8_t flags)
{
    return  ((uint64_t)(limit  & 0xFFFFU))                 /*  15: 0 limit[15:0] */
          | ((uint64_t)(base   & 0xFFFFU)      << 16)      /*  31:16 base[15:0]  */
          | ((uint64_t)((base  >> 16) & 0xFFU) << 32)      /*  39:32 base[23:16] */
          | ((uint64_t) access                 << 40)      /*  47:40 access byte */
          | ((uint64_t)((limit >> 16) & 0x0FU) << 48)      /*  51:48 limit[19:16]*/
          | ((uint64_t)(flags  & 0x0FU)         << 52)      /*  55:52 flags       */
          | ((uint64_t)((base  >> 24) & 0xFFU) << 56);     /*  63:56 base[31:24] */
}

/*
 * encode_tss — write the 16-byte TSS system descriptor into two
 * consecutive GDT slots.
 *
 * The 64-bit TSS descriptor format differs from a normal descriptor:
 *
 *   Bytes  0– 1: limit[15:0]
 *   Bytes  2– 4: base[23:0]
 *   Byte       5: access = 0x89  (P=1, DPL=0, S=0, Type=9 = 64-bit TSS avail)
 *   Byte       6: limit[19:16] | flags (G=0 for byte granularity)
 *   Byte       7: base[31:24]
 *   Bytes  8–11: base[63:32]
 *   Bytes 12–15: reserved (must be zero)
 *
 * The TSS limit is sizeof(TSS)-1; G=0 so the limit is in bytes.
 */
static void encode_tss(uint64_t *lo, uint64_t *hi, const TSS *tss)
{
    uint64_t base  = (uint64_t)tss;
    uint32_t limit = (uint32_t)(sizeof(TSS) - 1);

    *lo = encode_entry((uint32_t)(base & 0xFFFFFFFFU), limit, 0x89, 0x0);
    *hi = (base >> 32) & 0xFFFFFFFFULL; /* upper 32 bits of base in low dword */
}

/* ── Public API ───────────────────────────────────────────────────────────*/

void gdt_init(void)
{
    /* 1. Initialise the TSS structure (allocates the IST stacks etc.) */
    tss_init();

    /* 2. Build the GDT entries */
    g_gdt[0] = 0;  /* null */
    g_gdt[1] = encode_entry(0, 0xFFFFF, 0x9A, 0xA); /* kernel code (64-bit) */
    g_gdt[2] = encode_entry(0, 0xFFFFF, 0x92, 0xC); /* kernel data          */
    g_gdt[3] = encode_entry(0, 0xFFFFF, 0xF2, 0xC); /* user   data          */
    g_gdt[4] = encode_entry(0, 0xFFFFF, 0xFA, 0xA); /* user   code (64-bit) */
    encode_tss(&g_gdt[5], &g_gdt[6], tss_get());    /* TSS (16 bytes)       */

    /* 3. Load the GDT and reload all segment registers */
    g_gdt_desc.limit = (uint16_t)(sizeof(g_gdt) - 1);
    g_gdt_desc.base  = (uint64_t)g_gdt;
    gdt_flush(&g_gdt_desc);

    /* 4. Load the Task Register (LTR) with the TSS selector.
     *    Must happen AFTER lgdt because LTR indexes the new GDT. */
    tss_flush(SEL_TSS);
}
