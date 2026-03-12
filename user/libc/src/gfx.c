/*
 * gfx.c — User-space graphics syscall wrappers.
 */

#include <gfx.h>
#include <sys/syscall.h>
#include <stdint.h>

int gfx_screen_info(uint32_t *width, uint32_t *height)
{
    uint32_t buf[2];
    int64_t ret = __syscall1(SYS_GFX_INFO, (uint64_t)(uintptr_t)buf);
    if (ret == 0) {
        *width  = buf[0];
        *height = buf[1];
    }
    return (int)ret;
}

int gfx_draw(const GfxCmd *cmd)
{
    int64_t ret = __syscall1(SYS_GFX_DRAW, (uint64_t)(uintptr_t)cmd);
    return (int)ret;
}

int gfx_flush_display(void)
{
    int64_t ret = __syscall0(SYS_GFX_FLUSH);
    return (int)ret;
}
