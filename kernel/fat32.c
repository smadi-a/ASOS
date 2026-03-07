/*
 * kernel/fat32.c — Read-only FAT32 filesystem driver.
 *
 * Design notes
 * ────────────
 *  • A single 512-byte sector buffer (g_sec) is used for all disk reads
 *    (BPB, FAT lookups, directory scans, file data).  This works because
 *    the kernel is single-threaded at this milestone.
 *
 *  • Only the root directory is searched.  Subdirectory traversal is not
 *    implemented.
 *
 *  • Long File Name (LFN) entries (attr == 0x0F) are skipped; only 8.3
 *    short entries are returned to callers.
 *
 *  • Cluster indices < 2, == 0x0FFFFFF7 (bad cluster), or ≥ 0x0FFFFFF8
 *    (end-of-chain) all terminate chain traversal.
 */

#include "fat32.h"
#include "ata.h"
#include "serial.h"
#include <stddef.h>

/* ── Disk structures (packed — no compiler-inserted padding) ─────────────*/

typedef struct __attribute__((packed)) {
    uint8_t  jmp_boot[3];
    uint8_t  oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;       /* 0 for FAT32 */
    uint16_t total_sectors_16;       /* 0 for FAT32 */
    uint8_t  media;
    uint16_t fat_size_16;            /* 0 for FAT32 */
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    /* FAT32 extended BPB */
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t  _reserved[12];
    uint8_t  drive_number;
    uint8_t  _reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];             /* "FAT32   " */
} FAT32BPB;

typedef struct __attribute__((packed)) {
    uint8_t  name[8];
    uint8_t  ext[3];
    uint8_t  attr;
    uint8_t  _reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t last_access_date;
    uint16_t first_cluster_hi;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_lo;
    uint32_t file_size;
} FAT32DirEnt;

#define ATTR_READ_ONLY  0x01U
#define ATTR_HIDDEN     0x02U
#define ATTR_SYSTEM     0x04U
#define ATTR_VOLUME_ID  0x08U
#define ATTR_DIRECTORY  0x10U
#define ATTR_ARCHIVE    0x20U
#define ATTR_LFN        0x0FU   /* Long file name entry */

/* End-of-chain threshold (cluster value ≥ this → no more data). */
#define FAT32_EOC       0x0FFFFFF8U
#define FAT32_MASK      0x0FFFFFFFU

/* ── Module globals populated by fat32_init() ────────────────────────────*/

static uint8_t  g_drive;          /* ATA drive number                     */
static uint8_t  g_spc;            /* Sectors per cluster                  */
static uint32_t g_fat_lba;        /* LBA of the first FAT                 */
static uint32_t g_data_lba;       /* LBA of the data region (cluster 2)   */
static uint32_t g_root_cluster;   /* First cluster of the root directory  */

/* Re-used 512-byte I/O buffer. */
static uint8_t g_sec[512];

/* ── Internal helpers ─────────────────────────────────────────────────────*/

static int read_sec(uint32_t lba)
{
    return ata_read_sectors(g_drive, lba, 1, g_sec);
}

/*
 * Follow the FAT cluster chain: returns the cluster that comes after
 * 'cluster', or FAT32_EOC on end-of-chain or I/O error.
 */
static uint32_t next_cluster(uint32_t cluster)
{
    uint32_t fat_byte   = cluster * 4U;
    uint32_t fat_sector = g_fat_lba + fat_byte / 512U;
    uint32_t fat_off    = fat_byte % 512U;

    if (read_sec(fat_sector) != 0)
        return FAT32_EOC;

    /* Read 4 bytes little-endian. */
    uint32_t val = (uint32_t)g_sec[fat_off]
                 | ((uint32_t)g_sec[fat_off + 1] <<  8)
                 | ((uint32_t)g_sec[fat_off + 2] << 16)
                 | ((uint32_t)g_sec[fat_off + 3] << 24);

    val &= FAT32_MASK;
    /* Treat bad-cluster marker (0x0FFFFFF7) as end-of-chain. */
    if (val >= 0x0FFFFFF7U) return FAT32_EOC;
    return val;
}

/* Return the LBA of the first sector of the given cluster. */
static uint32_t cluster_lba(uint32_t cluster)
{
    return g_data_lba + (cluster - 2U) * (uint32_t)g_spc;
}

/* ── Directory iteration ──────────────────────────────────────────────────*/

/*
 * Visit every non-deleted, non-LFN directory entry in the cluster chain
 * starting at root_cluster.  Calls cb(ent) for each entry; if cb returns
 * non-zero the iteration stops and that value is returned.
 * Returns 0 when the chain is exhausted.
 *
 * Used by both fat32_list_root and fat32_find.
 */
typedef int (*dir_cb_t)(const FAT32DirEnt *ent, void *ctx);

static int iterate_dir(uint32_t start_cluster, dir_cb_t cb, void *ctx)
{
    uint32_t cluster = start_cluster;

    while (cluster >= 2U && cluster < FAT32_EOC) {
        uint32_t lba = cluster_lba(cluster);

        for (uint8_t s = 0; s < g_spc; s++) {
            if (read_sec(lba + s) != 0) return -1;

            const FAT32DirEnt *entries = (const FAT32DirEnt *)g_sec;
            int per_sector = 512 / (int)sizeof(FAT32DirEnt); /* always 16 */

            for (int e = 0; e < per_sector; e++) {
                const FAT32DirEnt *ent = &entries[e];

                if (ent->name[0] == 0x00) return 0;  /* End of directory */
                if (ent->name[0] == 0xE5) continue;  /* Deleted entry    */
                if (ent->attr == ATTR_LFN) continue; /* Long name        */

                int rc = cb(ent, ctx);
                if (rc != 0) return rc;
            }
        }

        cluster = next_cluster(cluster);
    }
    return 0;
}

/* Build an 8.3 display name ("HELLO   TXT" → "HELLO.TXT"). */
static void build_display_name(const FAT32DirEnt *ent, char *out)
{
    int n = 0;
    for (int i = 0; i < 8; i++) {
        if (ent->name[i] != ' ') out[n++] = (char)ent->name[i];
    }
    if (ent->ext[0] != ' ') {
        out[n++] = '.';
        for (int i = 0; i < 3; i++) {
            if (ent->ext[i] != ' ') out[n++] = (char)ent->ext[i];
        }
    }
    out[n] = '\0';
}

/* ── Directory callbacks ──────────────────────────────────────────────────*/

/* Uppercase a single byte (ASCII only — FAT short names are ASCII). */
static uint8_t to_upper(uint8_t c)
{
    return (c >= 'a' && c <= 'z') ? (uint8_t)(c - 32U) : c;
}

typedef struct {
    fat32_dirent_t *entries;
    uint32_t        max;
    uint32_t        count;
} list_ctx_t;

static int cb_list(const FAT32DirEnt *ent, void *ctx_v)
{
    list_ctx_t *ctx = (list_ctx_t *)ctx_v;

    /* Skip volume labels; keep files and directories. */
    if (ent->attr & ATTR_VOLUME_ID) return 0;

    if (ctx->count >= ctx->max) return 1; /* Stop — buffer full */

    fat32_dirent_t *out = &ctx->entries[ctx->count++];
    build_display_name(ent, out->name);
    out->size   = ent->file_size;
    out->is_dir = (ent->attr & ATTR_DIRECTORY) != 0;
    return 0;
}

typedef struct {
    const char  *name11;    /* 11-char padded 8.3 name to search for */
    fat32_file_t result;
    bool         found;
} find_ctx_t;

static int cb_find(const FAT32DirEnt *ent, void *ctx_v)
{
    find_ctx_t *ctx = (find_ctx_t *)ctx_v;

    /* Skip volume labels and directories. */
    if (ent->attr & (ATTR_VOLUME_ID | ATTR_DIRECTORY)) return 0;

    /* Compare 11-character FAT name (8 name + 3 ext), case-insensitive.
     * Directory entries are always uppercase; uppercase the input side only. */
    for (int i = 0; i < 8; i++) {
        if (ent->name[i] != to_upper((uint8_t)ctx->name11[i])) return 0;
    }
    for (int i = 0; i < 3; i++) {
        if (ent->ext[i] != to_upper((uint8_t)ctx->name11[8 + i])) return 0;
    }

    ctx->result.first_cluster = ((uint32_t)ent->first_cluster_hi << 16)
                                | (uint32_t)ent->first_cluster_lo;
    ctx->result.size = ent->file_size;
    ctx->found = true;
    return 1; /* Stop iteration */
}

/* ── Public API ───────────────────────────────────────────────────────────*/

int fat32_init(uint8_t ata_drive)
{
    g_drive = ata_drive;

    /* Read the Volume Boot Record (sector 0 of a raw FAT32 image). */
    if (ata_read_sectors(g_drive, 0, 1, g_sec) != 0) {
        serial_puts("[FAT32] I/O error reading VBR\n");
        return -1;
    }

    /* Mandatory boot sector signature at bytes 510-511 (0x55 0xAA). */
    if (g_sec[510] != 0x55U || g_sec[511] != 0xAAU) {
        serial_puts("[FAT32] Bad boot sector signature\n");
        return -1;
    }

    const FAT32BPB *bpb = (const FAT32BPB *)g_sec;

    /* Validate FAT32 filesystem type string. */
    const uint8_t sig[] = { 'F','A','T','3','2',' ',' ',' ' };
    for (int i = 0; i < 8; i++) {
        if (bpb->fs_type[i] != sig[i]) {
            serial_puts("[FAT32] Not a FAT32 volume\n");
            return -1;
        }
    }

    g_spc          = bpb->sectors_per_cluster;
    g_fat_lba      = bpb->reserved_sectors;
    g_data_lba     = g_fat_lba
                     + (uint32_t)bpb->num_fats * bpb->fat_size_32;
    g_root_cluster = bpb->root_cluster;

    serial_puts("[FAT32] Mounted: spc=");
    /* Print g_spc as decimal via serial */
    {
        char tmp[4]; int i = 0;
        uint8_t v = g_spc;
        if (v == 0) { serial_putc('0'); }
        else {
            while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
            while (i--) serial_putc(tmp[i]);
        }
    }
    serial_puts(" fat_lba=");
    {
        /* Simple hex print */
        static const char hex[] = "0123456789ABCDEF";
        serial_putc('0'); serial_putc('x');
        for (int s = 28; s >= 0; s -= 4)
            serial_putc(hex[(g_fat_lba >> s) & 0xF]);
    }
    serial_puts(" data_lba=");
    {
        static const char hex[] = "0123456789ABCDEF";
        serial_putc('0'); serial_putc('x');
        for (int s = 28; s >= 0; s -= 4)
            serial_putc(hex[(g_data_lba >> s) & 0xF]);
    }
    serial_puts("\n");

    return 0;
}

int fat32_list_root(fat32_dirent_t *entries, uint32_t max, uint32_t *count)
{
    list_ctx_t ctx = { entries, max, 0 };
    int rc = iterate_dir(g_root_cluster, cb_list, &ctx);
    *count = ctx.count;
    /* rc == 1 just means the buffer filled; that's not an error. */
    return (rc == -1) ? -1 : 0;
}

int fat32_find(const char *name11, fat32_file_t *out)
{
    find_ctx_t ctx;
    ctx.name11 = name11;
    ctx.found  = false;

    int rc = iterate_dir(g_root_cluster, cb_find, &ctx);
    if (rc == -1) return -1;
    if (!ctx.found) return -1;

    *out = ctx.result;
    return 0;
}

int fat32_read(const fat32_file_t *f, uint32_t offset,
               void *buf, uint32_t len, uint32_t *got)
{
    *got = 0;

    if (offset >= f->size) return 0;
    if (offset + len > f->size) len = f->size - offset;
    if (len == 0) return 0;

    uint32_t cluster_bytes = (uint32_t)g_spc * 512U;

    /* Which cluster in the chain holds 'offset'? */
    uint32_t cluster_idx    = offset / cluster_bytes;
    uint32_t byte_in_cluster = offset % cluster_bytes;

    /* Walk the cluster chain to the starting cluster. */
    uint32_t cluster = f->first_cluster;
    for (uint32_t i = 0; i < cluster_idx && cluster < FAT32_EOC; i++) {
        cluster = next_cluster(cluster);
    }

    uint8_t *dst   = (uint8_t *)buf;
    uint32_t done  = 0;

    while (done < len && cluster >= 2U && cluster < FAT32_EOC) {
        uint32_t clba = cluster_lba(cluster);

        /* Starting sector within this cluster (non-zero only on first pass). */
        uint8_t  start_sec     = (uint8_t)(byte_in_cluster / 512U);
        uint32_t byte_in_sec   = byte_in_cluster % 512U;

        for (uint8_t s = start_sec; s < g_spc && done < len; s++) {
            if (read_sec(clba + s) != 0) return -1;

            uint32_t avail = 512U - byte_in_sec;
            uint32_t copy  = avail < (len - done) ? avail : (len - done);

            for (uint32_t i = 0; i < copy; i++)
                dst[done++] = g_sec[byte_in_sec + i];

            byte_in_sec = 0; /* Only non-zero on the very first sector. */
        }

        byte_in_cluster = 0; /* Subsequent clusters always start at offset 0. */

        if (done < len)
            cluster = next_cluster(cluster);
    }

    *got = done;
    return 0;
}
