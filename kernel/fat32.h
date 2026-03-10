/*
 * kernel/fat32.h — Read-only FAT32 filesystem driver.
 *
 * Supports a single mounted volume (raw FAT32, no partition table).
 * Only regular files in the root directory are accessible (no subdirs).
 * Long File Name (LFN) entries are silently skipped.
 */

#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include <stdbool.h>

/* Maximum number of directory entries returned by fat32_list_root(). */
#define FAT32_MAX_DIR_ENTRIES  64

/* Represents an open file (cluster chain position + size). */
typedef struct {
    uint32_t first_cluster;
    uint32_t size;
} fat32_file_t;

/* A directory entry as seen by the caller (display-ready 8.3 name). */
typedef struct {
    char     name[13];    /* "FILENAME.EXT\0", up to 12 chars + NUL */
    uint32_t size;        /* File size in bytes (0 for directories)  */
    bool     is_dir;
} fat32_dirent_t;

/*
 * Mount a FAT32 volume from the given ATA drive.
 * 'partition_start_lba' is the first sector of the FAT32 partition (e.g.
 * the ESP start LBA from GPT).  All internal sector numbers are relative
 * to this offset.  Pass 0 for a raw, unpartitioned FAT32 image.
 *
 * Reads the BPB from sector partition_start_lba+0, validates the FAT32
 * signature, and caches key geometry.  Call once before any other fat32_*
 * function.
 *
 * Returns 0 on success, -1 if the drive is missing or not FAT32.
 */
int fat32_init(uint8_t ata_drive, uint32_t partition_start_lba);

/*
 * List all entries in the root directory.
 * Fills 'entries[0..max-1]'; sets *count to the number found.
 * Returns 0 on success, -1 on I/O error.
 */
int fat32_list_root(fat32_dirent_t *entries, uint32_t max, uint32_t *count);

/*
 * Find a file in the root directory by its 8.3 name in FAT format:
 * exactly 11 characters, name padded with spaces to 8, ext padded to 3.
 * Example: "HELLO   TXT"  (not "HELLO.TXT").
 *
 * Returns 0 on success and fills *out, or -1 if not found.
 */
int fat32_find(const char *name11, fat32_file_t *out);

/*
 * Read up to 'len' bytes starting at byte 'offset' from file *f into buf.
 * Sets *got to the number of bytes actually copied (may be less than len at
 * EOF or on I/O error).
 *
 * Returns 0 on success, -1 on I/O error.
 */
int fat32_read(const fat32_file_t *f, uint32_t offset,
               void *buf, uint32_t len, uint32_t *got);

/* Filesystem usage statistics. */
typedef struct {
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint64_t used_bytes;
    uint32_t cluster_size;
    uint32_t total_clusters;
    uint32_t free_clusters;
    uint32_t used_clusters;
} fs_stat_t;

/*
 * Retrieve filesystem statistics (total/free/used space).
 * Returns 0 on success, -1 on error.
 */
int fat32_get_stats(fs_stat_t *stat);

#endif /* FAT32_H */
