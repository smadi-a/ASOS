// i_net_asos.c — DOOM network stubs for ASOS.
//
// ASOS has no TCP/IP stack yet.  Force single-player mode.

#include <stdlib.h>
#include <string.h>

#include "doomstat.h"
#include "d_event.h"
#include "d_net.h"
#include "m_argv.h"
#include "i_system.h"
#include "i_net.h"

void I_InitNetwork(void)
{
    doomcom = malloc(sizeof(*doomcom));
    memset(doomcom, 0, sizeof(*doomcom));

    doomcom->ticdup       = 1;
    doomcom->extratics    = 0;
    doomcom->id           = DOOMCOM_ID;
    doomcom->numplayers   = 1;
    doomcom->numnodes     = 1;
    doomcom->deathmatch   = 0;
    doomcom->consoleplayer = 0;

    netgame = false;
}

void I_NetCmd(void)
{
    // Unreachable in single-player.
    I_Error("I_NetCmd: network not supported on ASOS");
}
