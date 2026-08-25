/*
 * aleks_ra_toast.c -- the achievement toast, and the ONLY place it is drawn.
 *
 * Donor architecture (TMC's port_debug_menu.cpp RaRenderToast + the single
 * Port_PPU_DrawAppOverlays stage): the toast is armed by the unlock event and
 * drawn once, at the very end of the frame, after gameplay + companion +
 * modal have already been composed.  NORMAL, DUAL and FLIP therefore get the
 * toast for free and identically -- there is no per-mode toast code, because
 * there is only one call site (aleks_compositor.c, end of Present).
 *
 * Drawn with plain renderer primitives rather than the companion's atlas: the
 * companion draws into its own render target, and the toast must land on the
 * backbuffer after that target has been resolved.
 */
#include "aleks_ra.h"

#ifdef __SWITCH__
#include <SDL.h>
#include <string.h>
#include <stdio.h>
#include "aleks_ra_badge.h"

#define TOAST_MS 4000u

static char sTitle[96];
static char sSub[64];
static uint32_t sUntil;
static SDL_Texture *sBadge;
static char sBadgeName[64];
static char sBadgeWanted[64];

void AleksRA_Toast(const char *title, int points, const char *badge_name) {
  if (!title || !title[0]) return;
  snprintf(sTitle, sizeof sTitle, "%s", title);
  if (points > 0)
    snprintf(sSub, sizeof sSub, "ACHIEVEMENT UNLOCKED  %d PTS", points);
  else
    snprintf(sSub, sizeof sSub, "ACHIEVEMENT UNLOCKED");
  sUntil = SDL_GetTicks() + TOAST_MS;
  /* Remember which badge this toast wants; the worker may still be fetching
   * it, and it is collected in the draw rather than waited for here. */
  snprintf(sBadgeWanted, sizeof sBadgeWanted, "%s", badge_name ? badge_name : "");
}

/* 5x7 uppercase strokes, enough for a toast line.  Self-contained so the
 * toast never reaches into the companion's atlas, which lives on a render
 * target that is not bound at this point in the frame. */
static const uint8_t *toast_glyph(char ch) {
  static const uint8_t letters[26][7] = {
    {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30}, {14,17,16,16,16,17,14},
    {30,17,17,17,17,17,30}, {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
    {14,17,16,23,17,17,14}, {17,17,17,31,17,17,17}, {14,4,4,4,4,4,14},
    {7,2,2,2,18,18,12},     {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17}, {14,17,17,17,17,17,14},
    {30,17,17,30,16,16,16}, {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30},   {31,4,4,4,4,4,4},       {17,17,17,17,17,17,14},
    {17,17,17,17,17,10,4},  {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4},     {31,1,2,4,8,16,31},
  };
  static const uint8_t digits[10][7] = {
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},  {14,17,1,2,4,8,31},
    {30,1,1,14,1,1,30},     {2,6,10,18,31,2,2}, {31,16,30,1,1,17,14},
    {6,8,16,30,17,17,14},   {31,1,2,4,8,8,8},   {14,17,17,14,17,17,14},
    {14,17,17,15,1,2,12},
  };
  if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
  if (ch >= 'A' && ch <= 'Z') return letters[ch - 'A'];
  if (ch >= '0' && ch <= '9') return digits[ch - '0'];
  return NULL;
}

static void toast_text(SDL_Renderer *r, const char *s, int x, int y, int px) {
  for (; *s; s++) {
    const uint8_t *g = toast_glyph(*s);
    if (g) {
      for (int row = 0; row < 7; row++)
        for (int col = 0; col < 5; col++)
          if (g[row] & (16 >> col)) {
            SDL_Rect p = { x + col * px, y + row * px, px, px };
            SDL_RenderFillRect(r, &p);
          }
    }
    x += 6 * px;
  }
}

void AleksRA_DrawToast(void *rr, int out_w, int out_h) {
  SDL_Renderer *r = (SDL_Renderer *)rr;
  uint32_t now;
  int px, w, h, x, y, pad, badge;

  if (!r || !sUntil) return;
  now = SDL_GetTicks();
  if (now >= sUntil) { sUntil = 0; return; }

  /* Collect the badge if the worker finished since the last frame. */
  if (sBadgeWanted[0]) {
    const char *ready_name = NULL;
    void *pixels = NULL;
    int bw = 0, bh = 0;
    if (AleksRA_Badge_TakeReady(&ready_name, &pixels, &bw, &bh) && pixels) {
      if (sBadge) SDL_DestroyTexture(sBadge);
      sBadge = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888,
                                 SDL_TEXTUREACCESS_STATIC, bw, bh);
      if (sBadge) SDL_UpdateTexture(sBadge, NULL, pixels, bw * 4);
      snprintf(sBadgeName, sizeof sBadgeName, "%s", ready_name ? ready_name : "");
      AleksRA_Badge_FreeReady(pixels);
    }
  }

  px  = out_h >= 900 ? 3 : 2;          /* docked gets the larger stroke */
  pad = 10 * px;
  badge = 16 * px;
  w = badge + pad * 3 + 6 * px * (int)(strlen(sTitle) > strlen(sSub)
                                       ? strlen(sTitle) : strlen(sSub));
  if (w > out_w - 2 * pad) w = out_w - 2 * pad;
  h = badge + pad;
  x = (out_w - w) / 2;
  y = out_h - h - pad * 2;             /* bottom centre, clear of the HUD */

  /* Fade the last half second so it leaves rather than vanishes. */
  {
    uint32_t left = sUntil - now;
    int alpha = left < 500 ? (int)(left * 255 / 500) : 255;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 12, 14, 20, (Uint8)(alpha * 232 / 255));
    { SDL_Rect box = { x, y, w, h }; SDL_RenderFillRect(r, &box); }
    SDL_SetRenderDrawColor(r, 212, 175, 55, (Uint8)alpha);
    { SDL_Rect box = { x, y, w, h }; SDL_RenderDrawRect(r, &box); }

    if (sBadge && sBadgeName[0] && strcmp(sBadgeName, sBadgeWanted) == 0) {
      SDL_Rect dst = { x + pad / 2, y + pad / 4, badge, badge };
      SDL_SetTextureAlphaMod(sBadge, (Uint8)alpha);
      SDL_RenderCopy(r, sBadge, NULL, &dst);
    }
    SDL_SetRenderDrawColor(r, 212, 175, 55, (Uint8)alpha);
    toast_text(r, sSub, x + badge + pad, y + pad / 3, px);
    SDL_SetRenderDrawColor(r, 240, 240, 240, (Uint8)alpha);
    toast_text(r, sTitle, x + badge + pad, y + pad / 3 + 9 * px, px);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
  }
}
#endif
