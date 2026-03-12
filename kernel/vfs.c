/*
 * kernel/vfs.c — VFS layer backed by FAT32 with subdirectory support.
 */

#include "vfs.h"
#include "fat32.h"
#include "serial.h"
#include "string.h"
#include <stddef.h>

/* Uppercase a path in-place for FAT32 compatibility. */
static void upper_path(char *buf, size_t max)
{
    for (size_t i = 0; i < max && buf[i]; i++) {
        if (buf[i] >= 'a' && buf[i] <= 'z')
            buf[i] = (char)(buf[i] - 32);
    }
}

/* ── Public API ───────────────────────────────────────────────────────────*/

int vfs_mount(uint8_t ata_drive, uint32_t esp_lba)
{
    return fat32_init(ata_drive, esp_lba);
}

int vfs_open(const char *path, vfs_file_t *out)
{
    char upper[256];
    strncpy(upper, path, 255);
    upper[255] = '\0';
    upper_path(upper, 256);

    uint32_t dir_cluster;
    char name_83[11];
    if (fat32_resolve_dir(upper, &dir_cluster, name_83) != 0)
        return -1;

    fat32_entry_info_t entry;
    if (fat32_find_entry(dir_cluster, name_83, &entry) != 0)
        return -1;

    if (entry.is_directory) return -1;  /* Can't open a directory as a file */

    out->_fat.first_cluster = entry.first_cluster;
    out->_fat.size          = entry.size;
    out->offset             = 0;
    memcpy(out->name_83, name_83, 11);
    out->dir_cluster        = dir_cluster;

    return 0;
}

int vfs_read(vfs_file_t *f, void *buf, uint32_t len, uint32_t *got)
{
    int rc = fat32_read(&f->_fat, f->offset, buf, len, got);
    if (rc == 0) f->offset += *got;
    return rc;
}

int vfs_list_dir(const char *path, vfs_dirent_t *entries,
                 uint32_t max, uint32_t *count)
{
    uint32_t dir_cluster;

    if (path[0] == '/' && path[1] == '\0') {
        dir_cluster = fat32_root_cluster();
    } else {
        char upper[256];
        strncpy(upper, path, 255);
        upper[255] = '\0';
        upper_path(upper, 256);

        uint32_t parent_cluster;
        char name_83[11];
        if (fat32_resolve_dir(upper, &parent_cluster, name_83) != 0) {
            *count = 0;
            return -1;
        }

        fat32_entry_info_t dir_entry;
        if (fat32_find_entry(parent_cluster, name_83, &dir_entry) != 0) {
            *count = 0;
            return -1;
        }

        if (!dir_entry.is_directory) {
            *count = 0;
            return -1;
        }

        dir_cluster = dir_entry.first_cluster;
    }

    return fat32_list_dir(dir_cluster, entries, max, count);
}

int vfs_seek(vfs_file_t *f, int64_t offset, int whence)
{
    if (!f) return -1;

    int64_t new_off;
    switch (whence) {
    case VFS_SEEK_SET: new_off = offset; break;
    case VFS_SEEK_CUR: new_off = (int64_t)f->offset + offset; break;
    case VFS_SEEK_END: new_off = (int64_t)f->_fat.size + offset; break;
    default: return -1;
    }

    if (new_off < 0) new_off = 0;
    if (new_off > (int64_t)f->_fat.size) new_off = (int64_t)f->_fat.size;

    f->offset = (uint32_t)new_off;
    return 0;
}

int vfs_create(const char *path)
{
    char upper[256];
    strncpy(upper, path, 255);
    upper[255] = '\0';
    upper_path(upper, 256);

    uint32_t dir_cluster;
    char name_83[11];
    if (fat32_resolve_dir(upper, &dir_cluster, name_83) != 0) return -1;

    /* Check if file already exists. */
    fat32_entry_info_t tmp;
    if (fat32_find_entry(dir_cluster, name_83, &tmp) == 0) return -1;

    /* Create empty entry (first_cluster=0, size=0, ARCHIVE attr). */
    return fat32_create_dir_entry(dir_cluster, name_83, 0, 0, 0x20);
}

uint32_t vfs_write(vfs_file_t *f, const void *buf, uint32_t len)
{
    if (!f || !buf || len == 0) return 0;

    uint32_t dir = f->dir_cluster;

    /* If file has no clusters yet, allocate the first one. */
    if (f->_fat.first_cluster == 0) {
        uint32_t fc = fat32_alloc_cluster();
        if (fc == 0) return 0;
        f->_fat.first_cluster = fc;
        fat32_update_dir_entry_first_cluster(dir, f->name_83, fc);
    }

    uint32_t last = 0;
    uint32_t written = fat32_write_at(f->_fat.first_cluster, f->offset,
                                       buf, len, &last);
    f->offset += written;

    if (f->offset > f->_fat.size) {
        f->_fat.size = f->offset;
        fat32_update_dir_entry_size(dir, f->name_83, f->_fat.size);
    }

    return written;
}

int vfs_delete(const char *path)
{
    char upper[256];
    strncpy(upper, path, 255);
    upper[255] = '\0';
    upper_path(upper, 256);

    uint32_t dir_cluster;
    char name_83[11];
    if (fat32_resolve_dir(upper, &dir_cluster, name_83) != 0) return -1;

    return fat32_delete_dir_entry(dir_cluster, name_83);
}

int vfs_mkdir(const char *path)
{
    char upper[256];
    strncpy(upper, path, 255);
    upper[255] = '\0';
    upper_path(upper, 256);

    uint32_t parent_cluster;
    char name_83[11];
    if (fat32_resolve_dir(upper, &parent_cluster, name_83) != 0)
        return -1;

    return fat32_mkdir(parent_cluster, name_83);
}

int vfs_rename(const char *old_path, const char *new_path)
{
    char old_upper[256], new_upper[256];
    strncpy(old_upper, old_path, 255);
    old_upper[255] = '\0';
    upper_path(old_upper, 256);
    strncpy(new_upper, new_path, 255);
    new_upper[255] = '\0';
    upper_path(new_upper, 256);

    uint32_t old_dir, new_dir;
    char old_name[11], new_name[11];

    if (fat32_resolve_dir(old_upper, &old_dir, old_name) != 0) return -1;
    if (fat32_resolve_dir(new_upper, &new_dir, new_name) != 0) return -1;

    if (old_dir == new_dir) {
        return fat32_rename_entry(old_dir, old_name, new_name);
    } else {
        return fat32_move_entry(old_dir, old_name, new_dir, new_name);
    }
}

int vfs_copy(const char *src_path, const char *dst_path)
{
    /* Open source for reading. */
    vfs_file_t src;
    if (vfs_open(src_path, &src) != 0) return -1;

    /* Create destination file. */
    if (vfs_create(dst_path) != 0) {
        /* Might already exist — that's an error for copy. */
        return -1;
    }

    vfs_file_t dst;
    if (vfs_open(dst_path, &dst) != 0) return -1;

    /* Copy in chunks. */
    uint8_t buf[512];
    uint32_t total_copied = 0;

    while (1) {
        uint32_t got = 0;
        if (vfs_read(&src, buf, sizeof(buf), &got) != 0) break;
        if (got == 0) break;

        uint32_t written = vfs_write(&dst, buf, got);
        if (written != got) {
            serial_puts("[VFS] Copy write error\n");
            return -1;
        }
        total_copied += written;
    }

    serial_puts("[VFS] Copied file\n");
    return 0;
}

int vfs_rmdir(const char *path)
{
    char upper[256];
    strncpy(upper, path, 255);
    upper[255] = '\0';
    upper_path(upper, 256);

    uint32_t parent_cluster;
    char name_83[11];
    if (fat32_resolve_dir(upper, &parent_cluster, name_83) != 0)
        return -1;

    return fat32_rmdir(parent_cluster, name_83);
}

int vfs_get_stats(fs_stat_t *stat)
{
    return fat32_get_stats(stat);
}

/* ── Path resolution ─────────────────────────────────────────────────────*/

int vfs_resolve_path(const char *input, const char *cwd,
                     char *output, size_t output_size)
{
    if (output_size < 2) return -1;

    /* Handle empty input as "." */
    if (!input || input[0] == '\0')
        input = ".";

    /* Build the full unresolved path in temp. */
    char temp[512];
    if (input[0] == '/') {
        /* Absolute path — copy directly. */
        strncpy(temp, input, sizeof(temp) - 1);
        temp[sizeof(temp) - 1] = '\0';
    } else {
        /* Relative path — prepend cwd. */
        size_t cwd_len = strlen(cwd);
        size_t inp_len = strlen(input);
        if (cwd_len + 1 + inp_len >= sizeof(temp)) return -1;

        memcpy(temp, cwd, cwd_len);
        /* Ensure separator between cwd and input. */
        if (cwd_len > 0 && cwd[cwd_len - 1] != '/') {
            temp[cwd_len] = '/';
            memcpy(temp + cwd_len + 1, input, inp_len + 1);
        } else {
            memcpy(temp + cwd_len, input, inp_len + 1);
        }
    }

    /* Normalize: split on '/' and process components with a stack. */
    char *components[64];
    int depth = 0;

    /* Tokenize by '/' manually (no strtok in kernel). */
    char *p = temp;
    while (*p) {
        /* Skip slashes. */
        while (*p == '/') p++;
        if (*p == '\0') break;

        /* Find end of this component. */
        char *start = p;
        while (*p && *p != '/') p++;
        /* Null-terminate this component. */
        if (*p == '/') *p++ = '\0';

        if (start[0] == '.' && start[1] == '\0') {
            /* "." — skip */
        } else if (start[0] == '.' && start[1] == '.' && start[2] == '\0') {
            /* ".." — go up, but not above root */
            if (depth > 0) depth--;
        } else {
            if (depth < 64)
                components[depth++] = start;
        }
    }

    /* Reassemble. */
    if (depth == 0) {
        output[0] = '/';
        output[1] = '\0';
    } else {
        size_t pos = 0;
        for (int i = 0; i < depth; i++) {
            size_t clen = strlen(components[i]);
            if (pos + 1 + clen >= output_size) return -1;
            output[pos++] = '/';
            memcpy(output + pos, components[i], clen);
            pos += clen;
        }
        output[pos] = '\0';
    }

    return 0;
}
