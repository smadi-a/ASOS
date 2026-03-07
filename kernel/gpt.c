/*
 * kernel/gpt.c — Minimal GPT partition table scanner.
 *
 * GPT disk layout
 * ───────────────
 *  LBA 0 : Protective MBR (ignored)
 *  LBA 1 : GPT Header (512 bytes)
 *    [  0.. 7] Signature  "EFI PART"
 *    [ 72..79] Partition entry array start LBA  (uint64_t LE)
 *    [ 80..83] Number of partition entries       (uint32_t LE)
 *    [ 84..87] Size of each partition entry      (uint32_t LE, usually 128)
 *  LBA 2+ : Partition Entry Array
 *    Each 128-byte entry:
 *    [  0..15] Partition type GUID (little-endian mixed format)
 *    [ 16..31] Unique partition GUID
 *    [ 32..39] First LBA  (uint64_t LE)
 *    [ 40..47] Last LBA   (uint64_t LE)
 *    [ 48..55] Attributes
 *    [ 56..127] UTF-16LE name
 *
 * EFI System Partition type GUID: C12A7328-F81F-11D2-BA4B-00A0C93EC93B
 * As stored (Data1/Data2/Data3 little-endian, Data4 big-endian):
 *   28 73 2A C1  1F F8  D2 11  BA 4B 00 A0 C9 3E C9 3B
 */

#include "gpt.h"
#include "ata.h"
#include "serial.h"
#include <stdint.h>
#include <stdbool.h>

/* Dedicated 512-byte sector buffer — separate from fat32's g_sec. */
static uint8_t g_buf[512];

/* EFI System Partition type GUID as it appears in a GPT entry. */
static const uint8_t ESP_TYPE_GUID[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

/* ── Helpers ──────────────────────────────────────────────────────────────*/

static uint32_t rd32(const uint8_t *b, int off)
{
    return (uint32_t)b[off]
         | ((uint32_t)b[off + 1] <<  8)
         | ((uint32_t)b[off + 2] << 16)
         | ((uint32_t)b[off + 3] << 24);
}

/* Return the low 32 bits of a little-endian uint64_t.
 * High 32 bits are ignored — LBA28 limits us to 2 TB anyway. */
static uint32_t rd64lo(const uint8_t *b, int off)
{
    return rd32(b, off);
}

static void print_hex32(uint32_t v)
{
    static const char h[] = "0123456789ABCDEF";
    serial_putc('0'); serial_putc('x');
    for (int s = 28; s >= 0; s -= 4)
        serial_putc(h[(v >> s) & 0xF]);
}

/* Print the first 'n' bytes of 'data' as a run of hex pairs. */
static void print_hex_bytes(const uint8_t *data, int n)
{
    static const char h[] = "0123456789ABCDEF";
    for (int i = 0; i < n; i++) {
        serial_putc(h[(data[i] >> 4) & 0xF]);
        serial_putc(h[ data[i]       & 0xF]);
    }
}

/* ── Public API ───────────────────────────────────────────────────────────*/

uint32_t gpt_find_esp(uint8_t drive)
{
    /* ── Read and validate the primary GPT header at LBA 1 ── */
    if (ata_read_sectors(drive, 1, 1, g_buf) != 0) {
        serial_puts("[GPT] I/O error reading header\n");
        return 0;
    }

    static const uint8_t SIG[8] = { 'E','F','I',' ','P','A','R','T' };
    for (int i = 0; i < 8; i++) {
        if (g_buf[i] != SIG[i]) {
            serial_puts("[GPT] Bad signature — not a GPT disk\n");
            return 0;
        }
    }

    uint32_t entry_lba   = rd64lo(g_buf, 72);
    uint32_t num_entries = rd32(g_buf,   80);
    uint32_t entry_size  = rd32(g_buf,   84);

    serial_puts("[GPT] header OK  entry_lba="); print_hex32(entry_lba);
    serial_puts("  count=");                    print_hex32(num_entries);
    serial_puts("  entry_sz=");                 print_hex32(entry_size);
    serial_puts("\n");

    if (entry_size == 0 || entry_size > 512) {
        serial_puts("[GPT] Unsupported entry size\n");
        return 0;
    }

    uint32_t entries_per_sector = 512U / entry_size;

    /* ── Scan partition entries ── */
    uint32_t cur_sector = (uint32_t)-1;   /* forces a read on entry 0 */

    for (uint32_t i = 0; i < num_entries; i++) {
        uint32_t sector = entry_lba + i / entries_per_sector;
        uint32_t offset = (i % entries_per_sector) * entry_size;

        /* Load the sector only when it changes. */
        if (sector != cur_sector) {
            if (ata_read_sectors(drive, sector, 1, g_buf) != 0) {
                serial_puts("[GPT] I/O error reading entries\n");
                return 0;
            }
            cur_sector = sector;
        }

        const uint8_t *e = g_buf + offset;

        /* Skip entries whose type GUID is all-zero (unused slot). */
        bool empty = true;
        for (int j = 0; j < 16; j++) {
            if (e[j]) { empty = false; break; }
        }
        if (empty) continue;

        /* Log non-empty entries: index, first 8 bytes of type GUID, first LBA. */
        serial_puts("[GPT] entry ");   print_hex32(i);
        serial_puts("  type=");        print_hex_bytes(e, 8); serial_puts("...");
        serial_puts("  first_lba=");   print_hex32(rd64lo(e, 32));
        serial_puts("\n");

        /* Compare full 16-byte type GUID against the ESP GUID. */
        bool match = true;
        for (int j = 0; j < 16; j++) {
            if (e[j] != ESP_TYPE_GUID[j]) { match = false; break; }
        }

        if (match) {
            uint32_t first_lba = rd64lo(e, 32);
            serial_puts("[GPT] Found ESP at LBA=");
            print_hex32(first_lba);
            serial_puts("\n");
            return first_lba;
        }
    }

    serial_puts("[GPT] ESP not found\n");
    return 0;
}
