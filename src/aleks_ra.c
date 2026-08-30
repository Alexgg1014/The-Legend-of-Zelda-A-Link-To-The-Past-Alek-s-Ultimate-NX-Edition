/*
 * aleks_ra.c -- RetroAchievements for Zelda3 NX.
 *
 * PORTED, not redesigned.  The donor is the final ALEKS TMC
 * (Hayato-tmc-switch-dev/port/port_retroachievements.c): rc_client lifecycle,
 * the two callbacks it needs, token-only credential storage, the unlock event
 * handler and the deferred auto-login are all the donor's shape, with its
 * Minish Cap specifics replaced.
 *
 * WHAT IS ZELDA3'S OWN, and therefore what to read carefully:
 *
 *   read_memory()      the SNES address model, over this port's real RAM
 *   rom_hash()         identification from the user's own zelda3.sfc
 *
 * Everything else is deliberately the donor's, because the donor's has been
 * run on hardware and mine has not.
 *
 * HARDCORE IS NOT OFFERED.  rc_client is created softcore and there is no UI
 * to change it.  The donor never solved hardcore either (its own notes call it
 * an open question), and hardcore's contract requires disabling save states,
 * quick load and rewind on the RA client's terms -- a set of restrictions this
 * build's save-state system has never been tested against.  A toggle that
 * unlocks hardcore achievements while save states still work would be a false
 * claim to the RA service, so there is none.  Softcore that is honest beats
 * hardcore that is not.
 */
#include "aleks_ra.h"

#ifdef __SWITCH__

#include "rc_client.h"
#include "rc_api_request.h"
#include "rc_hash.h"
#include "rc_consoles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dirent.h>
#include <SDL.h>

#include "config.h"
#include "zelda_rtl.h"
#include "aleks_crashctx.h"
#include "aleks_ra_badge.h"

/* Zelda3's emulated console RAM.  g_ram is the SNES 128 KB work RAM and
 * g_zenv.sram the cartridge's 8 KB save RAM (zelda_rtl.c). */
extern uint8 g_ram[131072];

/* Platform helpers (platform/switch/aleks_net.c), the donor's switch_net.c. */
extern void Port_Net_Init(void);
extern void Port_Net_Exit(void);
extern long Port_Net_HttpRequest(const char *url, const char *post_data,
                                 const char *content_type,
                                 char **out_body, size_t *out_len);
extern int Port_Swkbd_Get(const char *header, int password,
                          char *out, size_t out_cap);

#define RA_TOKEN_PATH "ra_token"
/* The name the PC build insists on.  On Switch the ROM keeps whatever name the
 * player's dump has -- see rom_open() -- so this is only a fast path. */
#define ZELDA_ROM_PREFERRED "zelda3.sfc"

static rc_client_t *sClient;
static bool sEnabled;
static bool sNetUp;
static bool sIdentified;
static bool sSawServerError;
static int  sAutoLoginQueued;
static char sStatus[64] = "LOGGED OUT";
static char sGameLine[80] = "NO GAME LOADED";
static char sRomHash[33];

/* ---- credentials -------------------------------------------------------
 * The TOKEN is stored, never the password -- exactly the donor's rule.  Its
 * own file rather than zelda3.ini, so wiping credentials cannot disturb
 * settings and a settings rewrite cannot leak a credential.
 */
static int save_token(const char *username, const char *token) {
  FILE *f = fopen(RA_TOKEN_PATH, "w");
  if (!f) return -1;
  fprintf(f, "%s\n%s\n", username, token);
  return fclose(f) == 0 ? 0 : -1;
}

static int load_token(char *user, size_t user_cap, char *token, size_t token_cap) {
  FILE *f = fopen(RA_TOKEN_PATH, "r");
  int ok = 0;
  if (!f) return 0;
  if (fgets(user, (int)user_cap, f) && fgets(token, (int)token_cap, f)) {
    user[strcspn(user, "\r\n")] = 0;
    token[strcspn(token, "\r\n")] = 0;
    ok = user[0] && token[0];
  }
  fclose(f);
  return ok;
}

/* ---- the Zelda3 memory adapter -----------------------------------------
 *
 * rcheevos presents the SNES as a flat space (consoleinfo.c,
 * _rc_memory_regions_snes):
 *
 *   0x000000..0x01FFFF  System RAM     128 KB, native 0x7E0000  -> g_ram
 *   0x020000..0x09FFFF  Cartridge RAM  (ALttP populates the first 8 KB)
 *   0x0A0000..0x0A07FF  SA-1 I-RAM     -- no SA-1 here, unmapped
 *
 * The sizes line up exactly: g_ram is 0x20000 and the SRAM allocation is
 * 0x2000, so no translation table is needed, only bounds.  A short read is
 * the sanctioned way to tell rc_client it reached a region boundary, so
 * anything unmapped stops the loop rather than inventing a byte -- there is
 * no path here that dereferences an address it has not range-checked, and no
 * pointer arithmetic that can leave its own buffer.
 */
#define RA_SNES_WRAM_END   0x020000u
#define RA_SNES_SRAM_BASE  0x020000u
#define ZELDA_SRAM_SIZE    0x2000u

static uint32_t read_memory(uint32_t address, uint8_t *buffer,
                            uint32_t num_bytes, rc_client_t *client) {
  uint32_t read = 0;
  (void)client;
  if (!buffer) return 0;
  while (read < num_bytes) {
    uint32_t a = address + read;
    if (a < RA_SNES_WRAM_END) {
      buffer[read++] = g_ram[a];
    } else if (a >= RA_SNES_SRAM_BASE && a < RA_SNES_SRAM_BASE + ZELDA_SRAM_SIZE) {
      /* The engine allocates SRAM at reset; before that there is nothing to
       * read and the boundary is reported instead. */
      if (!g_zenv.sram) break;
      buffer[read++] = g_zenv.sram[a - RA_SNES_SRAM_BASE];
    } else {
      break;    /* unmapped: cartridge RAM past 8 KB, SA-1, or out of range */
    }
  }
  return read;
}

/* ---- server calls ------------------------------------------------------ */
static void server_call(const rc_api_request_t *request,
                        rc_client_server_callback_t callback,
                        void *callback_data, rc_client_t *client) {
  char *body = NULL;
  size_t len = 0;
  const char *post;
  long status;
  rc_api_server_response_t response;
  (void)client;

  post = (request->post_data && request->post_data[0]) ? request->post_data : NULL;
  status = Port_Net_HttpRequest(request->url, post, request->content_type, &body, &len);

  memset(&response, 0, sizeof response);
  response.body = body ? body : "";
  response.body_length = len;
  response.http_status_code = (int)status;   /* negative = transport failure */

  if (status < 0) {
    /* Offline is a state, not an error: it is recorded for the status line
     * and the game is never told about it. */
    sSawServerError = true;
    StartupLog("RA: server unreachable (%ld)", status);
  }
  callback(&response, callback_data);
  free(body);
}

/* ---- unlock events ----------------------------------------------------- */
static void event_handler(const rc_client_event_t *event, rc_client_t *client) {
  (void)client;
  switch (event->type) {
  case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED: {
    const rc_client_achievement_t *a = event->achievement;
    if (a && a->title) {
      char badge_url[512];
      AleksRA_Toast(a->title, (int)a->points, a->badge_name);
      if (rc_client_achievement_get_image_url(a, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED,
                                              badge_url, sizeof badge_url) == RC_OK)
        AleksRA_Badge_Request(a->badge_name, badge_url);
      StartupLog("RA UNLOCK: %s (%u pts)", a->title, a->points);
    }
    break;
  }
  case RC_CLIENT_EVENT_GAME_COMPLETED:
    AleksRA_Toast("ALL ACHIEVEMENTS UNLOCKED", 0, NULL);
    break;
  default:
    break;
  }
}

/* ---- identification ----------------------------------------------------
 *
 * RA identifies a SNES game by hashing the ROM.  The ROM is the user's own
 * file, is not bundled with this build, and is not kept resident: it is read,
 * hashed and freed here, so RA costs no permanent memory and no copyrighted
 * bytes ever leave the card.  rcheevos' SNES hasher handles the 512-byte
 * copier header itself, so both headered and headerless dumps identify.
 */
/*
 * FIND THE ROM (public issue #4, "RetroAchievements not working").
 *
 * This used to be a bare fopen("zelda3.sfc").  On Switch that name is never
 * required: SwitchFirstRun_EnsureAssets() scans for ANY *.sfc / *.smc, lets
 * the player pick one, extracts zelda3_assets.dat from it and leaves the file
 * under its own name -- "Zelda3.smc", "alttp.sfc", the full No-Intro title,
 * whatever the player's dump is called.  Every later boot loads the cached
 * .dat and never touches the ROM again.
 *
 * So for anyone whose ROM is not literally named zelda3.sfc, rom_hash()
 * returned false and load_game() gave up WITHOUT LOGGING A SINGLE LINE.  The
 * reporter's startup.log shows exactly that: "RA: logged in (token stored)"
 * and then nothing at all -- login fine, session never started, no rich
 * presence, no unlocks.
 *
 * Resolution order: the preferred name first (unchanged behaviour, no scan
 * cost for anyone already set up that way), then the first SNES image in the
 * runtime root.  languages/ is a subdirectory and is deliberately not walked,
 * so a translation ROM can never be hashed in place of the base game.
 */
static FILE *rom_open(char *name_out, size_t name_size) {
  DIR *dir;
  struct dirent *ent;
  FILE *f = fopen(ZELDA_ROM_PREFERRED, "rb");

  if (f) {
    snprintf(name_out, name_size, "%s", ZELDA_ROM_PREFERRED);
    return f;
  }
  dir = opendir(".");
  if (!dir) return NULL;
  while ((ent = readdir(dir)) != NULL) {
    const char *dot = strrchr(ent->d_name, '.');
    if (!dot) continue;
    if (SDL_strcasecmp(dot, ".sfc") != 0 && SDL_strcasecmp(dot, ".smc") != 0)
      continue;
    f = fopen(ent->d_name, "rb");
    if (f) {
      snprintf(name_out, name_size, "%s", ent->d_name);
      break;
    }
  }
  closedir(dir);
  return f;
}

static bool rom_hash(void) {
  char rom_name[256] = {0};
  FILE *f = rom_open(rom_name, sizeof rom_name);
  long size;
  unsigned char *rom;
  bool ok;

  if (!f) {
    snprintf(sGameLine, sizeof sGameLine, "NO ROM TO IDENTIFY");
    StartupLog("RA: no .sfc/.smc in the runtime root; cannot identify the game");
    return false;
  }
  StartupLog("RA: identifying from %s", rom_name);
  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);
  /* A sane SNES cartridge; anything else is not what we think it is. */
  if (size <= 0 || size > 8 * 1024 * 1024) {
    fclose(f);
    snprintf(sGameLine, sizeof sGameLine, "ROM SIZE UNSUPPORTED");
    StartupLog("RA: %s is %ld bytes, not a usable SNES image", rom_name, size);
    return false;
  }
  rom = (unsigned char *)malloc((size_t)size);
  if (!rom) { fclose(f); return false; }
  ok = fread(rom, 1, (size_t)size, f) == (size_t)size;
  fclose(f);

  if (ok)
    ok = rc_hash_generate_from_buffer(sRomHash, RC_CONSOLE_SUPER_NINTENDO,
                                      rom, (size_t)size) != 0;
  free(rom);                       /* not kept resident */
  if (!ok) {
    snprintf(sGameLine, sizeof sGameLine, "ROM NOT RECOGNISED");
    StartupLog("RA: could not hash %s", rom_name);
  }
  return ok;
}

static void load_game_done(int result, const char *error_message,
                           rc_client_t *client, void *userdata) {
  (void)userdata;
  if (result == RC_OK) {
    const rc_client_game_t *game = rc_client_get_game_info(client);
    rc_client_user_game_summary_t summary;
    memset(&summary, 0, sizeof summary);
    rc_client_get_user_game_summary(client, &summary);
    sIdentified = true;
    snprintf(sGameLine, sizeof sGameLine, "%.40s  %u/%u",
             game && game->title ? game->title : "IDENTIFIED",
             summary.num_unlocked_achievements, summary.num_core_achievements);
    StartupLog("RA: game id=%u core=%u", game ? game->id : 0u,
               summary.num_core_achievements);
  } else {
    /* An unsupported ROM, a translation hack or a server that will not answer
     * all land here, and all of them mean the same thing to the player: play
     * on, without achievements. */
    sIdentified = false;
    snprintf(sGameLine, sizeof sGameLine, "UNSUPPORTED OR OFFLINE");
    StartupLog("RA: load game failed (%d) %s", result,
               error_message ? error_message : "");
  }
}

static void load_game(void) {
  if (!sClient) return;
  if (!rom_hash()) {
    /* rom_hash has already logged WHY and filled sGameLine for the RA page.
     * The one thing that must never happen again is this failing in silence. */
    sIdentified = false;
    return;
  }
  StartupLog("RA: loading game for hash %s", sRomHash);
  rc_client_begin_load_game(sClient, sRomHash, load_game_done, NULL);
}

/* ---- login ------------------------------------------------------------- */
static void login_done(int result, const char *error_message,
                       rc_client_t *client, void *userdata) {
  (void)userdata;
  if (result != RC_OK) {
    snprintf(sStatus, sizeof sStatus, "LOGIN FAILED");
    StartupLog("RA: login failed (%d) %s", result,
               error_message ? error_message : "");
    return;
  }
  {
    const rc_client_user_t *user = rc_client_get_user_info(client);
    if (user && user->username && user->token) {
      save_token(user->username, user->token);
      snprintf(sStatus, sizeof sStatus, "%.40s",
               user->display_name ? user->display_name : user->username);
      StartupLog("RA: logged in (token stored)");   /* never the token itself */
    }
  }
  load_game();
}

void AleksRA_InteractiveLogin(void) {
  char user[64], password[128];
  if (!sClient) return;
  if (!Port_Swkbd_Get("RetroAchievements username", 0, user, sizeof user)) {
    snprintf(sStatus, sizeof sStatus, "LOGIN CANCELLED");
    return;
  }
  if (!Port_Swkbd_Get("RetroAchievements password", 1, password, sizeof password)) {
    snprintf(sStatus, sizeof sStatus, "LOGIN CANCELLED");
    return;
  }
  snprintf(sStatus, sizeof sStatus, "LOGGING IN");
  rc_client_begin_login_with_password(sClient, user, password, login_done, NULL);
  /* password never leaves this stack frame */
  memset(password, 0, sizeof password);
}

void AleksRA_Logout(void) {
  if (sClient) rc_client_logout(sClient);
  remove(RA_TOKEN_PATH);
  sAutoLoginQueued = 0;
  sIdentified = false;
  snprintf(sStatus, sizeof sStatus, "LOGGED OUT");
  snprintf(sGameLine, sizeof sGameLine, "NO GAME LOADED");
}

/* ---- lifecycle --------------------------------------------------------- */
void AleksRA_Init(void) {
  sEnabled = g_config.aleks_ra_enable != 0;
  if (!sEnabled) {
    snprintf(sStatus, sizeof sStatus, "DISABLED");
    StartupLog("RA: disabled by config");
    return;
  }
  /* Networking comes up before the client, and its failure is survivable:
   * rc_client is still created so the menu works and reports honestly. */
  Port_Net_Init();
  sNetUp = true;

  sClient = rc_client_create(read_memory, server_call);
  if (!sClient) {
    snprintf(sStatus, sizeof sStatus, "UNAVAILABLE");
    StartupLog("RA: rc_client_create failed");
    return;
  }
  /* Softcore, permanently -- see the file header. */
  rc_client_set_hardcore_enabled(sClient, 0);
  rc_client_set_event_handler(sClient, event_handler);
  snprintf(sStatus, sizeof sStatus, "LOGGED OUT");
  sAutoLoginQueued = 1;
  StartupLog("RA: client ready (softcore)");
}

void AleksRA_AutoLoginAfterFirstPresent(void) {
  char user[64], token[256];
  if (!sAutoLoginQueued || !sClient) return;
  sAutoLoginQueued = 0;
  /* Only ever a silent token re-login.  With no saved token this does
   * nothing at all, which is what keeps a non-RA player's boot clean. */
  if (load_token(user, sizeof user, token, sizeof token)) {
    snprintf(sStatus, sizeof sStatus, "SIGNING IN");
    rc_client_begin_login_with_token(sClient, user, token, login_done, NULL);
  }
}

void AleksRA_DoFrame(void) {
  if (sClient) rc_client_do_frame(sClient);
}

void AleksRA_Shutdown(void) {
  AleksRA_Badge_Shutdown();
  if (sClient) { rc_client_destroy(sClient); sClient = NULL; }
  if (sNetUp) { Port_Net_Exit(); sNetUp = false; }
}

/* ---- status ------------------------------------------------------------ */
bool AleksRA_IsEnabled(void) { return sEnabled; }

void AleksRA_SetEnabled(bool on) {
  /* Turning it on mid-session brings the client up immediately; turning it
   * off tears the client down so no further frame does any RA work. */
  if (on == sEnabled) return;
  g_config.aleks_ra_enable = on ? 1 : 0;
  if (on) {
    AleksRA_Init();
  } else {
    AleksRA_Shutdown();
    sEnabled = false;
    sIdentified = false;
    snprintf(sStatus, sizeof sStatus, "DISABLED");
    snprintf(sGameLine, sizeof sGameLine, "NO GAME LOADED");
  }
}

bool AleksRA_IsLoggedIn(void) {
  return sClient && rc_client_get_user_info(sClient) != NULL;
}

const char *AleksRA_UserName(void) {
  const rc_client_user_t *user;
  if (!sClient) return NULL;
  user = rc_client_get_user_info(sClient);
  return user ? user->display_name : NULL;
}

const char *AleksRA_StatusLine(void) {
  if (!sEnabled) return "DISABLED";
  if (sSawServerError && !AleksRA_IsLoggedIn()) return "OFFLINE";
  return sStatus;
}

const char *AleksRA_GameLine(void) { return sGameLine; }

bool AleksRA_IsHardcore(void) {
  return sClient && rc_client_get_hardcore_enabled(sClient) != 0;
}

void AleksRA_GetCrashState(int *enabled, int *initialised, int *identified,
                           int *online) {
  if (enabled)     *enabled = sEnabled ? 1 : 0;
  if (initialised) *initialised = sClient ? 1 : 0;
  if (identified)  *identified = sIdentified ? 1 : 0;
  if (online)      *online = (sClient && !sSawServerError) ? 1 : 0;
}

#else   /* not Switch: RA is Switch-only, every entry point is inert */

void AleksRA_Init(void) {}
void AleksRA_Shutdown(void) {}
void AleksRA_DoFrame(void) {}
void AleksRA_AutoLoginAfterFirstPresent(void) {}
bool AleksRA_IsEnabled(void) { return false; }
void AleksRA_SetEnabled(bool on) { (void)on; }
bool AleksRA_IsLoggedIn(void) { return false; }
const char *AleksRA_UserName(void) { return NULL; }
const char *AleksRA_StatusLine(void) { return "UNAVAILABLE"; }
const char *AleksRA_GameLine(void) { return "UNAVAILABLE"; }
void AleksRA_InteractiveLogin(void) {}
void AleksRA_Logout(void) {}
bool AleksRA_IsHardcore(void) { return false; }
void AleksRA_DrawToast(void *r, int w, int h) { (void)r; (void)w; (void)h; }
void AleksRA_GetCrashState(int *a, int *b, int *c, int *d) {
  if (a) *a = 0; if (b) *b = 0; if (c) *c = 0; if (d) *d = 0;
}

#endif
