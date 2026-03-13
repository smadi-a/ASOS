// i_system_asos.c — DOOM system interface for ASOS.
//
// Replaces the Linux i_system.c with ASOS equivalents.

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "doomdef.h"
#include "m_misc.h"
#include "i_video.h"
#include "i_sound.h"
#include "d_net.h"
#include "g_game.h"
#include "i_system.h"

#include <sys/syscall.h>

// ---------------------------------------------------------------------------
// Zone memory
// ---------------------------------------------------------------------------

int mb_used = 6;

int I_GetHeapSize(void)
{
    return mb_used * 1024 * 1024;
}

byte *I_ZoneBase(int *size)
{
    *size = mb_used * 1024 * 1024;
    return (byte *)malloc(*size);
}

// ---------------------------------------------------------------------------
// Timing — I_GetTime returns time in 1/35th-second tics
// ---------------------------------------------------------------------------

// SYS_UPTIME returns milliseconds since boot (PIT at 1000 Hz).
int I_GetTime(void)
{
    static int64_t basetime = 0;

    int64_t now = __syscall0(SYS_UPTIME);
    if (!basetime)
        basetime = now;

    // Convert milliseconds → 35 Hz tics.
    int64_t elapsed_ms = now - basetime;
    return (int)(elapsed_ms * TICRATE / 1000);
}

// ---------------------------------------------------------------------------
// Misc
// ---------------------------------------------------------------------------

void I_Init(void)
{
    I_InitSound();
}

ticcmd_t emptycmd;
ticcmd_t *I_BaseTiccmd(void)
{
    return &emptycmd;
}

void I_Tactile(int on, int off, int total)
{
    (void)on; (void)off; (void)total;
}

byte *I_AllocLow(int length)
{
    byte *mem = (byte *)malloc(length);
    if (mem)
        memset(mem, 0, length);
    return mem;
}

void I_WaitVBL(int count)
{
    // Yield to the scheduler; DOOM uses this for short pauses.
    (void)count;
    __syscall0(SYS_YIELD);
}

void I_BeginRead(void) { }
void I_EndRead(void)   { }

// ---------------------------------------------------------------------------
// Quit / Error
// ---------------------------------------------------------------------------

extern boolean demorecording;

void I_Quit(void)
{
    D_QuitNetGame();
    I_ShutdownSound();
    I_ShutdownMusic();
    M_SaveDefaults();
    I_ShutdownGraphics();
    exit(0);
}

void I_Error(char *error, ...)
{
    char buf[512];
    va_list argptr;

    va_start(argptr, error);
    vsnprintf(buf, sizeof(buf), error, argptr);
    va_end(argptr);

    printf("Error: %s\n", buf);

    if (demorecording)
        G_CheckDemoStatus();

    D_QuitNetGame();
    I_ShutdownGraphics();

    exit(-1);
}
