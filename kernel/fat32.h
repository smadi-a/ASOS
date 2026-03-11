/*
 * kernel/fat32.h — FAT32 filesystem driver with read/write and subdirectory
 *                  support.
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

/* Internal directory entry info returned by fat32_find_entry(). */
typedef struct {
    uint32_t first_cluster;
    uint32_t size;
    bool     is_directory;
    char     name_83[11];
} fat32_entry_info_t;

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

/* Return the root directory cluster number. */
uint32_t fat32_root_cluster(void);

/*
 * Convert a human-readable filename ("HELLO.TXT") to the 11-byte
 * FAT32 internal 8.3 format ("HELLO   TXT").
 * Returns 0 on success, -1 on invalid name.
 */
int fat32_name_to_83(const char *name, char out_83[11]);

/*
 * Allocate a free cluster, mark it end-of-chain, zero its data.
 * Returns the cluster number, or 0 on failure (disk full).
 */
uint32_t fat32_alloc_cluster(void);

/*
 * Free an entire cluster chain starting at start_cluster.
 * Returns 0 on success, -1 on I/O error.
 */
int fat32_free_chain(uint32_t start_cluster);

/*
 * Allocate a new cluster and link it after last_cluster.
 * Returns the new cluster number, or 0 on failure.
 */
uint32_t fat32_extend_chain(uint32_t last_cluster);

/*
 * Write 'count' bytes from 'buf' at byte_offset within the cluster
 * chain starting at start_cluster.  Extends the chain as needed.
 * If last_cluster_out is non-NULL, stores the last cluster written.
 * Returns bytes actually written.
 */
uint32_t fat32_write_at(uint32_t start_cluster, uint32_t byte_offset,
                         const void *buf, uint32_t count,
                         uint32_t *last_cluster_out);

/*
 * Create a directory entry in the directory starting at dir_cluster.
 * Returns 0 on success, -1 on error.
 */
int fat32_create_dir_entry(uint32_t dir_cluster, const char name_83[11],
                           uint32_t first_cluster, uint32_t file_size,
                           uint8_t attributes);

/*
 * Update the size field of a directory entry identified by name_83.
 * Returns 0 on success, -1 if not found.
 */
int fat32_update_dir_entry_size(uint32_t dir_cluster,
                                const char name_83[11], uint32_t new_size);

/*
 * Update the first_cluster field of a directory entry.
 * Returns 0 on success, -1 if not found.
 */
int fat32_update_dir_entry_first_cluster(uint32_t dir_cluster,
                                         const char name_83[11],
                                         uint32_t first_cluster);

/*
 * Delete a file: mark its directory entry as deleted (0xE5) and free
 * its cluster chain.  Does not delete directories.
 * Returns 0 on success, -1 on error.
 */
int fat32_delete_dir_entry(uint32_t dir_cluster, const char name_83[11]);

/*
 * Search directory at dir_cluster for an entry matching name_83.
 * Finds both files and directories.
 * Returns 0 on success, -1 if not found.
 */
int fat32_find_entry(uint32_t dir_cluster, const char *name_83,
                     fat32_entry_info_t *out);

/*
 * List entries in the directory starting at dir_cluster.
 * Fills entries[0..max-1]; sets *count to the number found.
 * Returns 0 on success, -1 on I/O error.
 */
int fat32_list_dir(uint32_t dir_cluster, fat32_dirent_t *entries,
                   uint32_t max, uint32_t *count);

/*
 * Resolve a full path: walk subdirectories to find the parent directory
 * cluster and extract the final component in 8.3 format.
 * Returns 0 on success, -1 on error (component not found, bad name).
 */
int fat32_resolve_dir(const char *path, uint32_t *dir_cluster_out,
                      char *final_name_83);

/*
 * Create a new subdirectory with "." and ".." entries.
 * Returns 0 on success, -1 on error.
 */
int fat32_mkdir(uint32_t parent_cluster, const char *name_83);

/*
 * Rename a file or directory within the same directory.
 * Returns 0 on success, -1 on error.
 */
int fat32_rename_entry(uint32_t dir_cluster, const char *old_name_83,
                       const char *new_name_83);

/*
 * Move a file or directory between directories.
 * Returns 0 on success, -1 on error.
 */
int fat32_move_entry(uint32_t src_dir_cluster, const char *src_name_83,
                     uint32_t dst_dir_cluster, const char *dst_name_83);

/*
 * Mark a directory entry as deleted (0xE5) WITHOUT freeing the cluster
 * chain.  Used by move operations.
 * Returns 0 on success, -1 if not found.
 */
int fat32_remove_dir_entry_only(uint32_t dir_cluster, const char *name_83);

/*
 * Update the ".." entry in a directory to point to new_parent_cluster.
 * Returns 0 on success, -1 on error.
 */
int fat32_update_dotdot(uint32_t dir_cluster, uint32_t new_parent_cluster);

#endif /* FAT32_H */
