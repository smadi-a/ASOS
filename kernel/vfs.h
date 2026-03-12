/*
 * kernel/vfs.h — VFS layer over FAT32 with subdirectory support.
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
    char         name_83[11]; /* FAT 8.3 name for dir entry updates */
    uint32_t     dir_cluster; /* Directory containing this file */
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

/* Seek constants. */
#define VFS_SEEK_SET  0   /* From beginning   */
#define VFS_SEEK_CUR  1   /* From current pos  */
#define VFS_SEEK_END  2   /* From end of file  */

/*
 * Reposition the file offset.  Returns 0 on success, -1 on error.
 * Clamps to [0, file_size].
 */
int vfs_seek(vfs_file_t *f, int64_t offset, int whence);

/*
 * List entries in the given directory.  path must be "/" (root only).
 * Fills entries[0..max-1] and sets *count.
 *
 * Returns 0 on success, -1 on error.
 */
int vfs_list_dir(const char *path, vfs_dirent_t *entries,
                 uint32_t max, uint32_t *count);

/*
 * Create an empty file.  path must be absolute, e.g. "/HELLO.TXT".
 * Returns 0 on success, -1 on error (file exists, bad name, disk full).
 */
int vfs_create(const char *path);

/*
 * Write 'len' bytes from buf at the current file offset.
 * Extends the file and allocates clusters as needed.
 * Returns bytes written (may be less than len on disk-full).
 */
uint32_t vfs_write(vfs_file_t *f, const void *buf, uint32_t len);

/*
 * Delete a file.  path must be absolute, e.g. "/HELLO.TXT".
 * Returns 0 on success, -1 on error (not found, is a directory).
 */
int vfs_delete(const char *path);

/*
 * Create a directory.  path must be absolute, e.g. "/MYDIR".
 * Returns 0 on success, -1 on error.
 */
int vfs_mkdir(const char *path);

/*
 * Rename or move a file/directory.  Both paths must be absolute.
 * If old and new are in the same directory, performs a rename.
 * If in different directories, performs a move.
 * Returns 0 on success, -1 on error.
 */
int vfs_rename(const char *old_path, const char *new_path);

/*
 * Copy a file.  Both paths must be absolute.
 * Creates an independent copy with its own cluster chain.
 * Returns 0 on success, -1 on error.
 */
int vfs_copy(const char *src_path, const char *dst_path);

/*
 * Remove an empty directory.  path must be absolute.
 * Returns 0 on success, -1 on error (not empty, not found, not a dir).
 */
int vfs_rmdir(const char *path);

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
