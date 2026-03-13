// i_sound_asos.c — DOOM sound stubs for ASOS.
//
// ASOS does not yet have an audio driver.  All sound functions are
// stubbed to no-ops so the game runs silently.

#include "doomdef.h"
#include "doomstat.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// SFX
// ---------------------------------------------------------------------------

static int lengths[NUMSFX];

void I_InitSound(void)
{
    // Pre-cache sound data from the WAD so the rest of the engine
    // doesn't crash when it tries to dereference S_sfx[].data.
    for (int i = 1; i < NUMSFX; i++) {
        if (!S_sfx[i].link) {
            char name[20];
            snprintf(name, sizeof(name), "ds%s", S_sfx[i].name);
            int lump = W_CheckNumForName(name);
            if (lump == -1)
                lump = W_GetNumForName("dspistol");
            int size = W_LumpLength(lump);
            S_sfx[i].data = W_CacheLumpNum(lump, PU_STATIC);
            lengths[i] = size;
        } else {
            S_sfx[i].data = S_sfx[i].link->data;
            lengths[i] = lengths[(S_sfx[i].link - S_sfx) / sizeof(sfxinfo_t)];
        }
    }
}

void I_ShutdownSound(void) { }

void I_SetChannels(void) { }

void I_SetSfxVolume(int volume)  { (void)volume; }

int I_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char namebuf[9];
    snprintf(namebuf, sizeof(namebuf), "ds%s", sfx->name);
    return W_GetNumForName(namebuf);
}

int I_StartSound(int id, int vol, int sep, int pitch, int priority)
{
    (void)vol; (void)sep; (void)pitch; (void)priority;
    return id;
}

void I_StopSound(int handle)       { (void)handle; }
int  I_SoundIsPlaying(int handle)  { (void)handle; return 0; }

void I_UpdateSound(void)  { }
void I_SubmitSound(void)  { }

void I_UpdateSoundParams(int handle, int vol, int sep, int pitch)
{
    (void)handle; (void)vol; (void)sep; (void)pitch;
}

// ---------------------------------------------------------------------------
// Music — all no-ops
// ---------------------------------------------------------------------------

void I_InitMusic(void)         { }
void I_ShutdownMusic(void)     { }
void I_SetMusicVolume(int v)   { (void)v; }
void I_PauseSong(int h)        { (void)h; }
void I_ResumeSong(int h)       { (void)h; }
void I_StopSong(int h)         { (void)h; }
void I_UnRegisterSong(int h)   { (void)h; }
int  I_RegisterSong(void *d)   { (void)d; return 1; }
void I_PlaySong(int h, int l)  { (void)h; (void)l; }

int I_QrySongPlaying(int handle)
{
    (void)handle;
    return 0;
}
