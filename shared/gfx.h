/*
 * shared/gfx.h — Graphics command structure shared between kernel and user space.
 *
 * Included by kernel/syscall.c and by user-space programs via user/libc/include/gfx.h.
 */

#ifndef SHARED_GFX_H
#define SHARED_GFX_H

#include <stdint.h>

/* ── GFX operation codes ───────────────────────────────────────────────── */

#define GFX_OP_CLEAR        0   /* color                           */
#define GFX_OP_FILL_RECT    1   /* x,y,w,h, color                  */
#define GFX_OP_DRAW_RECT    2   /* x,y,w,h, color                  */
#define GFX_OP_HLINE        3   /* x,y, w=len, color               */
#define GFX_OP_VLINE        4   /* x,y, h=len, color               */
#define GFX_OP_PUT_PIXEL    5   /* x,y, color                      */
#define GFX_OP_DRAW_STRING  6   /* x,y, color=fg, w=bg,
                                   ptr=char*, ptr_len=strlen+1     */
#define GFX_OP_BLIT         7   /* x,y, w=src_w, h=src_h,
                                   ptr=uint32_t pixels, ptr_len=w*h*4 */

/* ── GFX command packet ────────────────────────────────────────────────── */

typedef struct {
    uint32_t op;         /* GFX_OP_* constant                              */
    int32_t  x, y;       /* Top-left origin for the operation              */
    int32_t  w, h;       /* Width / height  (or bg_color / src dims)       */
    uint32_t color;      /* Foreground colour 0xAARRGGBB (alpha ignored)   */
    uint64_t ptr;        /* User VA: char* for strings, uint32_t* for blit */
    uint32_t ptr_len;    /* Byte length of ptr data (0 if unused)          */
} GfxCmd;

#endif /* SHARED_GFX_H */
