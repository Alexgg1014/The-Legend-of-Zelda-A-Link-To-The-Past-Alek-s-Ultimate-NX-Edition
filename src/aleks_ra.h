/*
 * aleks_ra.h -- RetroAchievements for Zelda3 NX.
 *
 * Ported from the final ALEKS TMC implementation (port_retroachievements.c).
 * The client, login, token storage, event handling and toast queue are the
 * donor's; what is Zelda3's own is the SNES memory adapter and the ROM
 * identification, because those are the only parts that can be wrong in a way
 * the donor could not have got right for us.
 *
 * EVERYTHING HERE IS OPTIONAL.  Every entry point is a no-op when RA is
 * disabled, uninitialised or offline; nothing in this header can keep Zelda
 * from booting or running.
 */
#pragma once
#include "types.h"

/* Lifecycle.  Init is safe to call when RA is switched off -- it returns
 * without touching the network. */
void AleksRA_Init(void);
void AleksRA_Shutdown(void);

/* Once per rendered frame, from the game thread.  No-op with no game loaded. */
void AleksRA_DoFrame(void);

/* Deferred so the network never sits on the boot path: armed at init, run
 * after the first frame is on screen. */
void AleksRA_AutoLoginAfterFirstPresent(void);

/* Settings rows. */
bool AleksRA_IsEnabled(void);
void AleksRA_SetEnabled(bool on);
bool AleksRA_IsLoggedIn(void);
const char *AleksRA_UserName(void);     /* NULL when logged out */
const char *AleksRA_StatusLine(void);   /* never NULL; safe to draw */
const char *AleksRA_GameLine(void);     /* identified game, or a reason */
void AleksRA_InteractiveLogin(void);    /* opens the system keyboard */
void AleksRA_Logout(void);

/* Hardcore is deliberately NOT offered -- see the note in aleks_ra.c.  This
 * reports the real client state so the UI never claims otherwise. */
bool AleksRA_IsHardcore(void);

/* Toast rendering, called from the ONE final overlay stage in the compositor
 * after gameplay + companion + modal have been composed.  Takes the renderer
 * as void* so this header stays includable by files that have no SDL. */
void AleksRA_DrawToast(void *renderer, int out_w, int out_h);

/* Cheap state for the crash context.  Never includes credentials. */
void AleksRA_GetCrashState(int *enabled, int *initialised, int *identified,
                           int *online);
