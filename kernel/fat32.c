/*
 * kernel/fat32.c — FAT32 filesystem driver with read/write and subdirectory
 *                  support.
 */

#include "fat32.h"
#include "ata.h"
#include "serial.h"
#include "string.h"
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
static uint32_t g_part_lba;       /* First sector of the FAT32 partition  */
static uint8_t  g_spc;            /* Sectors per cluster                  */
static uint32_t g_fat_lba;        /* LBA of first FAT (rel. to partition) */
static uint32_t g_data_lba;       /* LBA of data region  (rel. to part.)  */
static uint32_t g_root_cluster;   /* First cluster of the root directory  */

/* BPB values cached for fat32_get_stats. */
static uint32_t g_total_sectors;  /* total_sectors_32 from BPB            */
static uint16_t g_reserved_secs;  /* reserved_sectors from BPB            */
static uint8_t  g_num_fats;       /* num_fats from BPB (usually 2)        */
static uint32_t g_fat_size;       /* fat_size_32 from BPB (sectors/FAT)   */

/* Cached free cluster count. */
static uint32_t g_cached_free_clusters;
static int      g_free_clusters_valid;

/* Next-fit cursor for cluster allocation. */
static uint32_t g_next_free_search = 2;

/* Re-used 512-byte I/O buffer (used by read_sec / write_sec). */
static uint8_t g_sec[512];

/* ── Sector cache ─────────────────────────────────────────────────────────
 *
 * A small direct-mapped cache of recently-read sectors.  When DOOM (or any
 * program) does many small reads, the VFS layer re-reads the same FAT and
 * data sectors repeatedly.  The cache avoids redundant ATA PIO transfers.
 *
 * 16 slots × 512 bytes = 8 KB BSS — cheap for a big win.
 */
#define SEC_CACHE_SLOTS  16U
#define SEC_CACHE_MASK   (SEC_CACHE_SLOTS - 1U)  /* direct-mapped index */

static struct {
    uint32_t abs_lba;   /* Absolute LBA (partition-relative + g_part_lba). */
    uint8_t  data[512];
    uint8_t  valid;     /* 0 = empty, 1 = contains data for abs_lba.      */
} g_sec_cache[SEC_CACHE_SLOTS];

/* Invalidate all cache slots (e.g., after partition switch — unlikely). */
static void sec_cache_invalidate_all(void)
{
    for (uint32_t i = 0; i < SEC_CACHE_SLOTS; i++)
        g_sec_cache[i].valid = 0;
}

/* Invalidate a single absolute LBA from the cache (used after writes). */
static void sec_cache_invalidate(uint32_t abs_lba)
{
    uint32_t slot = abs_lba & SEC_CACHE_MASK;
    if (g_sec_cache[slot].valid && g_sec_cache[slot].abs_lba == abs_lba)
        g_sec_cache[slot].valid = 0;
}

/* ── Internal helpers ─────────────────────────────────────────────────────*/

/*
 * Read a single sector (partition-relative LBA) into g_sec.
 * Checks the sector cache first; on miss, reads from ATA and caches.
 */
static int read_sec(uint32_t lba)
{
    uint32_t abs = g_part_lba + lba;
    uint32_t slot = abs & SEC_CACHE_MASK;

    if (g_sec_cache[slot].valid && g_sec_cache[slot].abs_lba == abs) {
        /* Cache hit — copy into g_sec (callers expect data there). */
        memcpy(g_sec, g_sec_cache[slot].data, 512);
        return 0;
    }

    /* Cache miss — read from disk. */
    if (ata_read_sectors(g_drive, abs, 1, g_sec) != 0)
        return -1;

    /* Populate cache. */
    g_sec_cache[slot].abs_lba = abs;
    memcpy(g_sec_cache[slot].data, g_sec, 512);
    g_sec_cache[slot].valid = 1;
    return 0;
}

static int write_sec(uint32_t lba)
{
    /* Invalidate the cache slot for this sector — the on-disk data is
     * about to change, and g_sec holds the new content.  We could also
     * update the cache, but invalidation is simpler and writes are rare. */
    sec_cache_invalidate(g_part_lba + lba);
    return ata_write_sectors(g_drive, g_part_lba + lba, 1, g_sec);
}

/* Serial decimal printer for debug output. */
static void fat32_put_dec(uint32_t v)
{
    if (v == 0) { serial_putc('0'); return; }
    char tmp[12]; int i = 0;
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    while (i--) serial_putc(tmp[i]);
}

/* ── FAT entry read/write ────────────────────────────────────────────────*/

static uint32_t read_fat_entry(uint32_t cluster)
{
    uint32_t fat_byte   = cluster * 4U;
    uint32_t fat_sector = g_fat_lba + fat_byte / 512U;
    uint32_t off        = fat_byte % 512U;

    if (read_sec(fat_sector) != 0) return FAT32_EOC;

    uint32_t val = (uint32_t)g_sec[off]
                 | ((uint32_t)g_sec[off + 1] <<  8)
                 | ((uint32_t)g_sec[off + 2] << 16)
                 | ((uint32_t)g_sec[off + 3] << 24);
    return val & FAT32_MASK;
}

static int write_fat_entry(uint32_t cluster, uint32_t value)
{
    uint32_t fat_byte   = cluster * 4U;
    uint32_t fat_sector = g_fat_lba + fat_byte / 512U;
    uint32_t off        = fat_byte % 512U;

    /* Read sector, modify entry preserving top 4 bits, write back. */
    if (read_sec(fat_sector) != 0) return -1;

    uint32_t old = (uint32_t)g_sec[off]
                 | ((uint32_t)g_sec[off + 1] <<  8)
                 | ((uint32_t)g_sec[off + 2] << 16)
                 | ((uint32_t)g_sec[off + 3] << 24);

    uint32_t nv = (old & 0xF0000000U) | (value & FAT32_MASK);
    g_sec[off]     = (uint8_t)(nv);
    g_sec[off + 1] = (uint8_t)(nv >> 8);
    g_sec[off + 2] = (uint8_t)(nv >> 16);
    g_sec[off + 3] = (uint8_t)(nv >> 24);

    if (write_sec(fat_sector) != 0) return -1;

    /* Update the second FAT copy. */
    if (g_num_fats > 1) {
        if (ata_write_sectors(g_drive, g_part_lba + fat_sector + g_fat_size,
                              1, g_sec) != 0)
            return -1;
    }

    return 0;
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

int fat32_init(uint8_t ata_drive, uint32_t partition_start_lba)
{
    g_drive    = ata_drive;
    g_part_lba = partition_start_lba;
    sec_cache_invalidate_all();

    /* Read the Volume Boot Record (sector 0 relative to partition start). */
    if (read_sec(0) != 0) {
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

    g_total_sectors = bpb->total_sectors_32;
    g_reserved_secs = bpb->reserved_sectors;
    g_num_fats      = bpb->num_fats;
    g_fat_size      = bpb->fat_size_32;
    g_free_clusters_valid = 0;

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

        /* Fast path: if the read is sector-aligned and we need multiple
         * whole sectors from this cluster, batch them in one ATA command
         * and populate the sector cache for each. */
        if (byte_in_sec == 0 && (len - done) >= 512U) {
            uint8_t secs_left = g_spc - start_sec;
            uint32_t bytes_left = len - done;
            uint8_t batch = secs_left;
            if ((uint32_t)batch * 512U > bytes_left)
                batch = (uint8_t)(bytes_left / 512U);
            if (batch == 0) batch = 1;

            /* Read 'batch' consecutive sectors directly into dst. */
            uint32_t abs_base = g_part_lba + clba + start_sec;
            if (ata_read_sectors(g_drive, abs_base, batch, dst + done) != 0)
                return -1;

            /* Populate the sector cache with each sector we just read. */
            for (uint8_t b = 0; b < batch; b++) {
                uint32_t abs = abs_base + b;
                uint32_t slot = abs & SEC_CACHE_MASK;
                g_sec_cache[slot].abs_lba = abs;
                memcpy(g_sec_cache[slot].data, dst + done + (uint32_t)b * 512U, 512);
                g_sec_cache[slot].valid = 1;
            }

            done += (uint32_t)batch * 512U;
            start_sec += batch;
        }

        /* Byte-granularity path for partial sectors at start/end. */
        for (uint8_t s = start_sec; s < g_spc && done < len; s++) {
            if (read_sec(clba + s) != 0) return -1;

            uint32_t avail = 512U - byte_in_sec;
            uint32_t copy  = avail < (len - done) ? avail : (len - done);

            memcpy(dst + done, g_sec + byte_in_sec, copy);
            done += copy;

            byte_in_sec = 0; /* Only non-zero on the very first sector. */
        }

        byte_in_cluster = 0; /* Subsequent clusters always start at offset 0. */

        if (done < len)
            cluster = next_cluster(cluster);
    }

    *got = done;
    return 0;
}

int fat32_get_stats(fs_stat_t *stat)
{
    uint32_t data_sectors = g_total_sectors
                            - (uint32_t)g_reserved_secs
                            - (uint32_t)g_num_fats * g_fat_size;
    uint32_t total_data_clusters = data_sectors / (uint32_t)g_spc;

    uint32_t free_count;
    if (g_free_clusters_valid) {
        free_count = g_cached_free_clusters;
    } else {
        /* Scan the FAT to count free clusters. */
        free_count = 0;
        uint32_t entries_per_sector = 512U / 4U;  /* 128 */

        for (uint32_t s = 0; s < g_fat_size; s++) {
            if (read_sec(g_fat_lba + s) != 0) return -1;

            uint32_t *entries = (uint32_t *)g_sec;
            for (uint32_t i = 0; i < entries_per_sector; i++) {
                uint32_t ci = s * entries_per_sector + i;
                if (ci < 2) continue;                       /* reserved */
                if (ci >= total_data_clusters + 2) goto done;
                if ((entries[i] & 0x0FFFFFFFU) == 0)
                    free_count++;
            }
        }
done:
        g_cached_free_clusters = free_count;
        g_free_clusters_valid = 1;
    }

    uint32_t cluster_size = (uint32_t)g_spc * 512U;
    uint32_t used_count = total_data_clusters - free_count;

    stat->total_bytes    = (uint64_t)total_data_clusters * cluster_size;
    stat->free_bytes     = (uint64_t)free_count * cluster_size;
    stat->used_bytes     = (uint64_t)used_count * cluster_size;
    stat->cluster_size   = cluster_size;
    stat->total_clusters = total_data_clusters;
    stat->free_clusters  = free_count;
    stat->used_clusters  = used_count;

    return 0;
}

/* ── Write operations ────────────────────────────────────────────────────*/

uint32_t fat32_root_cluster(void)
{
    return g_root_cluster;
}

static uint32_t get_total_data_clusters(void)
{
    uint32_t data_sectors = g_total_sectors
                            - (uint32_t)g_reserved_secs
                            - (uint32_t)g_num_fats * g_fat_size;
    return data_sectors / (uint32_t)g_spc;
}

int fat32_name_to_83(const char *name, char out_83[11])
{
    memset(out_83, ' ', 11);
    if (!name || !name[0]) return -1;

    const char *dot = (void *)0;
    for (const char *p = name; *p; p++)
        if (*p == '.') dot = p;

    int name_len = dot ? (int)(dot - name) : (int)strlen(name);
    if (name_len > 8 || name_len == 0) return -1;

    for (int i = 0; i < name_len; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out_83[i] = c;
    }

    if (dot) {
        const char *ext = dot + 1;
        int ext_len = (int)strlen(ext);
        if (ext_len > 3) return -1;
        for (int i = 0; i < ext_len; i++) {
            char c = ext[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            out_83[8 + i] = c;
        }
    }

    return 0;
}

uint32_t fat32_alloc_cluster(void)
{
    uint32_t total = get_total_data_clusters();
    uint32_t cluster = g_next_free_search;

    for (uint32_t searched = 0; searched < total; searched++) {
        if (cluster >= total + 2) cluster = 2;

        uint32_t entry = read_fat_entry(cluster);
        if (entry == 0) {
            /* Mark as end-of-chain. */
            if (write_fat_entry(cluster, 0x0FFFFFFFU) != 0) return 0;

            /* Zero the cluster's data sectors. */
            uint32_t clba = cluster_lba(cluster);
            memset(g_sec, 0, 512);
            for (uint8_t s = 0; s < g_spc; s++) {
                if (write_sec(clba + s) != 0) {
                    write_fat_entry(cluster, 0);   /* Undo */
                    return 0;
                }
            }

            g_next_free_search = cluster + 1;
            if (g_free_clusters_valid)
                g_cached_free_clusters--;

            serial_puts("[FAT32] Allocated cluster ");
            fat32_put_dec(cluster);
            serial_puts("\n");
            return cluster;
        }
        cluster++;
    }

    serial_puts("[FAT32] ERROR: No free clusters\n");
    return 0;
}

int fat32_free_chain(uint32_t start_cluster)
{
    if (start_cluster < 2) return 0;  /* Empty file */

    uint32_t cluster = start_cluster;
    while (cluster >= 2U && cluster < FAT32_EOC) {
        uint32_t nxt = read_fat_entry(cluster);
        if (write_fat_entry(cluster, 0) != 0) return -1;

        if (g_free_clusters_valid)
            g_cached_free_clusters++;

        cluster = nxt;
    }

    if (start_cluster < g_next_free_search)
        g_next_free_search = start_cluster;

    return 0;
}

uint32_t fat32_extend_chain(uint32_t last_cluster)
{
    uint32_t nc = fat32_alloc_cluster();
    if (nc == 0) return 0;

    if (write_fat_entry(last_cluster, nc) != 0) {
        write_fat_entry(nc, 0);  /* Undo alloc */
        if (g_free_clusters_valid) g_cached_free_clusters++;
        return 0;
    }

    return nc;
}

uint32_t fat32_write_at(uint32_t start_cluster, uint32_t byte_offset,
                         const void *buf, uint32_t count,
                         uint32_t *last_cluster_out)
{
    uint32_t cluster_bytes = (uint32_t)g_spc * 512U;

    uint32_t clusters_to_skip = byte_offset / cluster_bytes;
    uint32_t off_in_cluster   = byte_offset % cluster_bytes;

    uint32_t cur = start_cluster;
    for (uint32_t i = 0; i < clusters_to_skip; i++) {
        uint32_t nxt = read_fat_entry(cur);
        if (nxt >= FAT32_EOC) {
            nxt = fat32_extend_chain(cur);
            if (nxt == 0) { if (last_cluster_out) *last_cluster_out = cur; return 0; }
        }
        cur = nxt;
    }

    const uint8_t *src = (const uint8_t *)buf;
    uint32_t written = 0;

    while (written < count) {
        uint32_t clba = cluster_lba(cur);

        uint32_t avail = cluster_bytes - off_in_cluster;
        uint32_t to_write = count - written;
        if (to_write > avail) to_write = avail;

        uint32_t start_sec  = off_in_cluster / 512U;
        uint32_t off_in_sec = off_in_cluster % 512U;

        uint32_t done_in_cl = 0;
        for (uint32_t s = start_sec; s < g_spc && done_in_cl < to_write; s++) {
            uint32_t src_off = (s == start_sec) ? off_in_sec : 0;
            uint32_t writable = 512U - src_off;
            uint32_t wlen = to_write - done_in_cl;
            if (wlen > writable) wlen = writable;

            /* Partial sector: read-modify-write. */
            if (src_off != 0 || wlen < 512U) {
                if (read_sec(clba + s) != 0)
                    goto out;
            }

            memcpy(g_sec + src_off, src + written + done_in_cl, wlen);

            if (write_sec(clba + s) != 0)
                goto out;

            done_in_cl += wlen;
        }

        written += done_in_cl;

        if (written < count) {
            uint32_t nxt = read_fat_entry(cur);
            if (nxt >= FAT32_EOC) {
                nxt = fat32_extend_chain(cur);
                if (nxt == 0) goto out;
            }
            cur = nxt;
        }

        off_in_cluster = 0;
    }

out:
    if (last_cluster_out) *last_cluster_out = cur;
    return written;
}

/* ── Directory entry operations ──────────────────────────────────────────*/

int fat32_create_dir_entry(uint32_t dir_cluster, const char name_83[11],
                           uint32_t first_cluster, uint32_t file_size,
                           uint8_t attributes)
{
    uint32_t cur = dir_cluster;

    for (;;) {
        uint32_t clba = cluster_lba(cur);

        for (uint8_t s = 0; s < g_spc; s++) {
            if (read_sec(clba + s) != 0)
                return -1;

            for (int e = 0; e < 16; e++) {
                uint8_t *ent = g_sec + e * 32;

                if (ent[0] == 0x00 || ent[0] == 0xE5) {
                    memset(ent, 0, 32);
                    memcpy(ent, name_83, 11);
                    ent[11] = attributes;
                    ent[20] = (uint8_t)(first_cluster >> 16);
                    ent[21] = (uint8_t)(first_cluster >> 24);
                    ent[26] = (uint8_t)(first_cluster);
                    ent[27] = (uint8_t)(first_cluster >> 8);
                    ent[28] = (uint8_t)(file_size);
                    ent[29] = (uint8_t)(file_size >> 8);
                    ent[30] = (uint8_t)(file_size >> 16);
                    ent[31] = (uint8_t)(file_size >> 24);

                    if (write_sec(clba + s) != 0)
                        return -1;

                    serial_puts("[FAT32] Created entry\n");
                    return 0;
                }
            }
        }

        uint32_t nxt = read_fat_entry(cur);
        if (nxt >= FAT32_EOC) {
            nxt = fat32_extend_chain(cur);
            if (nxt == 0) return -1;
        }
        cur = nxt;
    }
}

int fat32_update_dir_entry_size(uint32_t dir_cluster,
                                const char name_83[11], uint32_t new_size)
{
    uint32_t cur = dir_cluster;

    while (cur >= 2U && cur < FAT32_EOC) {
        uint32_t clba = cluster_lba(cur);

        for (uint8_t s = 0; s < g_spc; s++) {
            if (read_sec(clba + s) != 0)
                return -1;

            for (int e = 0; e < 16; e++) {
                uint8_t *ent = g_sec + e * 32;
                if (ent[0] == 0x00) return -1;
                if (ent[0] == 0xE5) continue;
                if (ent[11] == ATTR_LFN) continue;

                if (memcmp(ent, name_83, 11) == 0) {
                    ent[28] = (uint8_t)(new_size);
                    ent[29] = (uint8_t)(new_size >> 8);
                    ent[30] = (uint8_t)(new_size >> 16);
                    ent[31] = (uint8_t)(new_size >> 24);
                    return write_sec(clba + s);
                }
            }
        }

        cur = read_fat_entry(cur);
    }
    return -1;
}

int fat32_update_dir_entry_first_cluster(uint32_t dir_cluster,
                                         const char name_83[11],
                                         uint32_t first_cluster)
{
    uint32_t cur = dir_cluster;

    while (cur >= 2U && cur < FAT32_EOC) {
        uint32_t clba = cluster_lba(cur);

        for (uint8_t s = 0; s < g_spc; s++) {
            if (read_sec(clba + s) != 0)
                return -1;

            for (int e = 0; e < 16; e++) {
                uint8_t *ent = g_sec + e * 32;
                if (ent[0] == 0x00) return -1;
                if (ent[0] == 0xE5) continue;
                if (ent[11] == ATTR_LFN) continue;

                if (memcmp(ent, name_83, 11) == 0) {
                    ent[20] = (uint8_t)(first_cluster >> 16);
                    ent[21] = (uint8_t)(first_cluster >> 24);
                    ent[26] = (uint8_t)(first_cluster);
                    ent[27] = (uint8_t)(first_cluster >> 8);
                    return write_sec(clba + s);
                }
            }
        }

        cur = read_fat_entry(cur);
    }
    return -1;
}

int fat32_delete_dir_entry(uint32_t dir_cluster, const char name_83[11])
{
    uint32_t cur = dir_cluster;

    while (cur >= 2U && cur < FAT32_EOC) {
        uint32_t clba = cluster_lba(cur);

        for (uint8_t s = 0; s < g_spc; s++) {
            if (read_sec(clba + s) != 0)
                return -1;

            for (int e = 0; e < 16; e++) {
                uint8_t *ent = g_sec + e * 32;
                if (ent[0] == 0x00) return -1;
                if (ent[0] == 0xE5) continue;
                if (ent[11] == ATTR_LFN) continue;

                if (memcmp(ent, name_83, 11) == 0) {
                    /* Refuse to delete directories. */
                    if (ent[11] & ATTR_DIRECTORY) return -1;

                    uint32_t fc = ((uint32_t)ent[20] << 16)
                                | ((uint32_t)ent[21] << 24)
                                | ((uint32_t)ent[26])
                                | ((uint32_t)ent[27] << 8);

                    ent[0] = 0xE5;
                    if (write_sec(clba + s) != 0)
                        return -1;

                    if (fc >= 2)
                        fat32_free_chain(fc);

                    serial_puts("[FAT32] Deleted entry\n");
                    return 0;
                }
            }
        }

        cur = read_fat_entry(cur);
    }
    return -1;
}

/* ── Subdirectory support ────────────────────────────────────────────────*/

/* Callback context for fat32_find_entry. */
typedef struct {
    const char         *name11;
    fat32_entry_info_t  result;
    bool                found;
} find_entry_ctx_t;

static int cb_find_entry(const FAT32DirEnt *ent, void *ctx_v)
{
    find_entry_ctx_t *ctx = (find_entry_ctx_t *)ctx_v;

    /* Skip volume labels. */
    if (ent->attr & ATTR_VOLUME_ID) return 0;

    /* Compare 11-char name, case-insensitive. */
    for (int i = 0; i < 8; i++) {
        if (ent->name[i] != to_upper((uint8_t)ctx->name11[i])) return 0;
    }
    for (int i = 0; i < 3; i++) {
        if (ent->ext[i] != to_upper((uint8_t)ctx->name11[8 + i])) return 0;
    }

    ctx->result.first_cluster = ((uint32_t)ent->first_cluster_hi << 16)
                                | (uint32_t)ent->first_cluster_lo;
    ctx->result.size = ent->file_size;
    ctx->result.is_directory = (ent->attr & ATTR_DIRECTORY) != 0;
    memcpy(ctx->result.name_83, ent->name, 8);
    memcpy(ctx->result.name_83 + 8, ent->ext, 3);
    ctx->found = true;
    return 1;
}

int fat32_find_entry(uint32_t dir_cluster, const char *name_83,
                     fat32_entry_info_t *out)
{
    find_entry_ctx_t ctx;
    ctx.name11 = name_83;
    ctx.found  = false;

    int rc = iterate_dir(dir_cluster, cb_find_entry, &ctx);
    if (rc == -1) return -1;
    if (!ctx.found) return -1;

    *out = ctx.result;
    return 0;
}

int fat32_list_dir(uint32_t dir_cluster, fat32_dirent_t *entries,
                   uint32_t max, uint32_t *count)
{
    list_ctx_t ctx = { entries, max, 0 };
    int rc = iterate_dir(dir_cluster, cb_list, &ctx);
    *count = ctx.count;
    return (rc == -1) ? -1 : 0;
}

int fat32_resolve_dir(const char *path, uint32_t *dir_cluster_out,
                      char *final_name_83)
{
    uint32_t current_cluster = g_root_cluster;

    char pathbuf[256];
    strncpy(pathbuf, path, sizeof(pathbuf) - 1);
    pathbuf[sizeof(pathbuf) - 1] = '\0';

    /* Strip leading slashes. */
    char *p = pathbuf;
    while (*p == '/') p++;

    if (*p == '\0') {
        /* Path is just "/" — no final component. */
        if (dir_cluster_out) *dir_cluster_out = g_root_cluster;
        if (final_name_83) memset(final_name_83, ' ', 11);
        return -1;
    }

    /* Split into components. */
    char *components[32];
    int num_components = 0;

    char *token = p;
    while (*p) {
        if (*p == '/') {
            *p = '\0';
            if (token[0] != '\0') {
                components[num_components++] = token;
                if (num_components >= 32) return -1;
            }
            p++;
            token = p;
        } else {
            p++;
        }
    }
    if (token[0] != '\0') {
        components[num_components++] = token;
    }

    if (num_components == 0) return -1;

    /* Walk all components except the last one as directories. */
    for (int i = 0; i < num_components - 1; i++) {
        char name_83[11];
        if (fat32_name_to_83(components[i], name_83) != 0) return -1;

        fat32_entry_info_t entry;
        if (fat32_find_entry(current_cluster, name_83, &entry) != 0)
            return -1;

        if (!entry.is_directory) return -1;

        current_cluster = entry.first_cluster;
    }

    if (dir_cluster_out) *dir_cluster_out = current_cluster;

    if (final_name_83) {
        if (fat32_name_to_83(components[num_components - 1],
                             final_name_83) != 0)
            return -1;
    }

    return 0;
}

int fat32_mkdir(uint32_t parent_cluster, const char *name_83)
{
    /* Check if entry already exists. */
    fat32_entry_info_t existing;
    if (fat32_find_entry(parent_cluster, name_83, &existing) == 0)
        return -1;

    /* Allocate a cluster for the new directory's contents. */
    uint32_t dir_cluster = fat32_alloc_cluster();
    if (dir_cluster == 0) return -1;

    /* Create "." and ".." entries in the first sector. */
    uint8_t sector_buf[512];
    memset(sector_buf, 0, 512);

    /* "." entry — points to this directory itself. */
    uint8_t *dot = sector_buf;
    memset(dot, ' ', 11);
    dot[0] = '.';
    dot[11] = ATTR_DIRECTORY;
    dot[20] = (uint8_t)(dir_cluster >> 16);
    dot[21] = (uint8_t)(dir_cluster >> 24);
    dot[26] = (uint8_t)(dir_cluster);
    dot[27] = (uint8_t)(dir_cluster >> 8);

    /* ".." entry — points to parent (or 0 for root's children). */
    uint8_t *dotdot = sector_buf + 32;
    memset(dotdot, ' ', 11);
    dotdot[0] = '.';
    dotdot[1] = '.';
    dotdot[11] = ATTR_DIRECTORY;
    uint32_t parent_ref = (parent_cluster == g_root_cluster) ? 0 : parent_cluster;
    dotdot[20] = (uint8_t)(parent_ref >> 16);
    dotdot[21] = (uint8_t)(parent_ref >> 24);
    dotdot[26] = (uint8_t)(parent_ref);
    dotdot[27] = (uint8_t)(parent_ref >> 8);

    /* Write first sector of new directory. */
    uint32_t dir_lba = cluster_lba(dir_cluster);
    memcpy(g_sec, sector_buf, 512);
    if (write_sec(dir_lba) != 0) {
        fat32_free_chain(dir_cluster);
        return -1;
    }

    /* Create directory entry in parent. */
    if (fat32_create_dir_entry(parent_cluster, name_83, dir_cluster,
                                0, ATTR_DIRECTORY) != 0) {
        fat32_free_chain(dir_cluster);
        return -1;
    }

    serial_puts("[FAT32] Created directory\n");
    return 0;
}

int fat32_rename_entry(uint32_t dir_cluster, const char *old_name_83,
                       const char *new_name_83)
{
    /* Verify new name doesn't already exist. */
    fat32_entry_info_t existing;
    if (fat32_find_entry(dir_cluster, new_name_83, &existing) == 0)
        return -1;

    uint32_t cur = dir_cluster;

    while (cur >= 2U && cur < FAT32_EOC) {
        uint32_t clba = cluster_lba(cur);

        for (uint8_t s = 0; s < g_spc; s++) {
            if (read_sec(clba + s) != 0)
                return -1;

            for (int e = 0; e < 16; e++) {
                uint8_t *entry = g_sec + e * 32;

                if (entry[0] == 0x00) return -1;
                if (entry[0] == 0xE5) continue;
                if (entry[11] == ATTR_LFN) continue;

                if (memcmp(entry, old_name_83, 11) == 0) {
                    memcpy(entry, new_name_83, 11);
                    return write_sec(clba + s);
                }
            }
        }

        cur = read_fat_entry(cur);
    }

    return -1;
}

int fat32_remove_dir_entry_only(uint32_t dir_cluster, const char *name_83)
{
    uint32_t cur = dir_cluster;

    while (cur >= 2U && cur < FAT32_EOC) {
        uint32_t clba = cluster_lba(cur);

        for (uint8_t s = 0; s < g_spc; s++) {
            if (read_sec(clba + s) != 0)
                return -1;

            for (int e = 0; e < 16; e++) {
                uint8_t *entry = g_sec + e * 32;

                if (entry[0] == 0x00) return -1;
                if (entry[0] == 0xE5) continue;
                if (entry[11] == ATTR_LFN) continue;

                if (memcmp(entry, name_83, 11) == 0) {
                    entry[0] = 0xE5;
                    return write_sec(clba + s);
                }
            }
        }

        cur = read_fat_entry(cur);
    }

    return -1;
}

int fat32_update_dotdot(uint32_t dir_cluster, uint32_t new_parent_cluster)
{
    uint32_t dir_lba = cluster_lba(dir_cluster);

    if (read_sec(dir_lba) != 0)
        return -1;

    uint8_t *dotdot = g_sec + 32;

    /* Verify this is actually the ".." entry. */
    if (dotdot[0] != '.' || dotdot[1] != '.') {
        serial_puts("[FAT32] WARNING: expected .. entry not found\n");
        return -1;
    }

    uint32_t parent_ref = new_parent_cluster;
    if (new_parent_cluster == g_root_cluster)
        parent_ref = 0;

    dotdot[20] = (uint8_t)(parent_ref >> 16);
    dotdot[21] = (uint8_t)(parent_ref >> 24);
    dotdot[26] = (uint8_t)(parent_ref);
    dotdot[27] = (uint8_t)(parent_ref >> 8);

    return write_sec(dir_lba);
}

/* Check if a directory is empty (only ".", "..", deleted, end entries). */
static int dir_is_empty(uint32_t dir_cluster)
{
    uint32_t cluster = dir_cluster;

    while (cluster >= 2U && cluster < FAT32_EOC) {
        uint32_t lba = cluster_lba(cluster);

        for (uint8_t s = 0; s < g_spc; s++) {
            if (read_sec(lba + s) != 0) return 0; /* Error → not empty */

            const FAT32DirEnt *entries = (const FAT32DirEnt *)g_sec;
            int per_sector = 512 / (int)sizeof(FAT32DirEnt);

            for (int e = 0; e < per_sector; e++) {
                const FAT32DirEnt *ent = &entries[e];

                if (ent->name[0] == 0x00) return 1;  /* End → empty */
                if (ent->name[0] == 0xE5) continue;   /* Deleted */

                /* Skip "." and ".." */
                if (ent->name[0] == '.' && ent->name[1] == ' ') continue;
                if (ent->name[0] == '.' && ent->name[1] == '.' &&
                    ent->name[2] == ' ') continue;

                /* Any other entry → not empty */
                return 0;
            }
        }

        cluster = next_cluster(cluster);
    }
    return 1; /* Exhausted chain → empty */
}

int fat32_rmdir(uint32_t parent_cluster, const char *name_83)
{
    /* Find the entry. */
    fat32_entry_info_t entry;
    if (fat32_find_entry(parent_cluster, name_83, &entry) != 0)
        return -1;

    /* Must be a directory. */
    if (!entry.is_directory) return -1;

    /* Must be empty. */
    if (!dir_is_empty(entry.first_cluster)) {
        serial_puts("[FAT32] rmdir: directory not empty\n");
        return -1;
    }

    /* Free the directory's cluster chain. */
    if (entry.first_cluster >= 2)
        fat32_free_chain(entry.first_cluster);

    /* Remove the parent directory entry. */
    if (fat32_remove_dir_entry_only(parent_cluster, name_83) != 0)
        return -1;

    serial_puts("[FAT32] Removed directory\n");
    return 0;
}

int fat32_move_entry(uint32_t src_dir_cluster, const char *src_name_83,
                     uint32_t dst_dir_cluster, const char *dst_name_83)
{
    /* Check destination doesn't already have this name. */
    fat32_entry_info_t existing;
    if (fat32_find_entry(dst_dir_cluster, dst_name_83, &existing) == 0)
        return -1;

    /* Read the source entry. */
    fat32_entry_info_t src_entry;
    if (fat32_find_entry(src_dir_cluster, src_name_83, &src_entry) != 0)
        return -1;

    /* If moving a directory, update its ".." entry. */
    if (src_entry.is_directory) {
        if (fat32_update_dotdot(src_entry.first_cluster,
                                 dst_dir_cluster) != 0)
            return -1;
    }

    /* Create entry in destination. */
    uint8_t attr = src_entry.is_directory ? ATTR_DIRECTORY : ATTR_ARCHIVE;
    if (fat32_create_dir_entry(dst_dir_cluster, dst_name_83,
                                src_entry.first_cluster,
                                src_entry.size, attr) != 0)
        return -1;

    /* Remove source entry WITHOUT freeing clusters. */
    if (fat32_remove_dir_entry_only(src_dir_cluster, src_name_83) != 0) {
        /* Best-effort undo. */
        fat32_remove_dir_entry_only(dst_dir_cluster, dst_name_83);
        return -1;
    }

    serial_puts("[FAT32] Moved entry\n");
    return 0;
}
