/*
 * gfx.c — User-space graphics syscall wrappers.
 */

#include <gfx.h>
#include <sys/syscall.h>
#include <stdint.h>

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
