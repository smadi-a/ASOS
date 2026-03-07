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
