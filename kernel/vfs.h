/*
 * kernel/vfs.h — Minimal read-only VFS layer over FAT32.
 *
 * Supports a single mounted volume; only root-directory files are accessible.
 * Paths are always of the form "/" or "/NAME.EXT".
 */

#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>
#include "fat32.h"

/* An open file handle. */
typedef struct {
    fat32_file_t _fat;     /* Underlying FAT32 handle */
    uint32_t     offset;   /* Current read position   */
} vfs_file_t;

/* A directory entry as returned by vfs_list_dir(). */
typedef fat32_dirent_t vfs_dirent_t;

/*
 * Mount the FAT32 volume on 'ata_drive' starting at 'esp_lba' (the first
 * sector of the FAT32 partition, as returned by gpt_find_esp()).
 * Must be called before any other vfs_* function.
 *
 * Returns 0 on success, -1 on error.
 */
int vfs_mount(uint8_t ata_drive, uint32_t esp_lba);

/*
 * Open a file.  path must be an absolute path to a root-directory file,
 * e.g., "/HELLO.TXT" or "/LARGE.TXT".
 *
 * Returns 0 and fills *out on success; returns -1 if not found.
 */
int vfs_open(const char *path, vfs_file_t *out);

/*
 * Read up to 'len' bytes from the current position of *f into buf.
 * Advances f->offset.  Sets *got to the number of bytes copied.
 *
 * Returns 0 on success, -1 on I/O error.
 */
int vfs_read(vfs_file_t *f, void *buf, uint32_t len, uint32_t *got);

/* Return the total size of the open file in bytes. */
static inline uint32_t vfs_size(const vfs_file_t *f) { return f->_fat.size; }

/* Close the file (no-op for read-only; provided for symmetry). */
static inline void vfs_close(vfs_file_t *f) { (void)f; }

/*
 * List entries in the given directory.  path must be "/" (root only).
 * Fills entries[0..max-1] and sets *count.
 *
 * Returns 0 on success, -1 on error.
 */
int vfs_list_dir(const char *path, vfs_dirent_t *entries,
                 uint32_t max, uint32_t *count);

/*
 * Retrieve filesystem usage statistics.  Returns 0 on success, -1 on error.
 */
int vfs_get_stats(fs_stat_t *stat);

/*
 * Resolve a (possibly relative) path against the given cwd,
 * producing a normalized absolute path in output.
 * Returns 0 on success, -1 on error (buffer overflow).
 */
int vfs_resolve_path(const char *input, const char *cwd,
                     char *output, size_t output_size);

#endif /* VFS_H */
