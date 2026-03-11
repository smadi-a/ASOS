/*
 * kernel/vfs.c — Minimal VFS layer backed by FAT32.
 *
 * Path handling
 * ─────────────
 * Paths must start with "/".  The leading slash is stripped, and the
 * remaining name is converted to FAT32's 8.3 format:
 *   "HELLO.TXT"  →  "HELLO   TXT"  (8 chars name + 3 chars ext, space-padded)
 *   "README"     →  "README  "     (no extension)
 *
 * Only uppercase ASCII is accepted (FAT32 directory entries store names in
 * uppercase; callers should pass uppercase paths).
 */

#include "vfs.h"
#include "fat32.h"
#include "string.h"
#include <stddef.h>

/* Convert a filename string to the 11-char FAT8.3 format.
 * 'in'  : "HELLO.TXT" or "README" (no leading slash, no path separator).
 * 'out' : exactly 11 bytes, e.g. "HELLO   TXT" or "README     ".
 */
static void to_fat83(const char *in, char out[11])
{
    /* Initialise with spaces. */
    for (int i = 0; i < 11; i++) out[i] = ' ';

    int ni = 0; /* name position (0–7) */
    int ei = 0; /* ext  position (0–2) */
    int in_ext = 0;

    for (; *in; in++) {
        char c = *in;
        if (c == '.') {
            in_ext = 1;
            continue;
        }
        /* Uppercase conversion. */
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);

        if (!in_ext) {
            if (ni < 8) out[ni++] = c;
        } else {
            if (ei < 3) out[8 + ei++] = c;
        }
    }
}

/* ── Public API ───────────────────────────────────────────────────────────*/

int vfs_mount(uint8_t ata_drive, uint32_t esp_lba)
{
    return fat32_init(ata_drive, esp_lba);
}

int vfs_open(const char *path, vfs_file_t *out)
{
    /* Strip leading slash. */
    if (path[0] == '/') path++;

    char name11[11];
    to_fat83(path, name11);

    fat32_file_t fat;
    if (fat32_find(name11, &fat) != 0) return -1;

    out->_fat   = fat;
    out->offset = 0;
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
    /* Only root directory is supported. */
    if (path[0] == '/' && path[1] == '\0')
        return fat32_list_root(entries, max, count);

    *count = 0;
    return -1;
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
