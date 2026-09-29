/* switch_safeboot.c -- the ways out of a renderer that draws no Settings.
 *
 * With OutputMethod=OpenGL the game presents straight to the window (opengl.c
 * never runs AleksCompositor), so the companion -- and SETTINGS with it -- is
 * not drawn at all.  Two escapes:
 *
 *   1. HOLDING ZL+R3 in game.  The primary one.  That chord normally opens
 *      SETTINGS; when SETTINGS cannot be shown it means "put me back on the
 *      dual screen" instead.  It is a hold, not a tap, and it paints a banner
 *      with a progress bar into the game's own picture while it is held --
 *      see the note on the banner below for why that matters.
 *   2. Holding ZL+R3 while the game boots, for someone who would rather not
 *      load into the room at all.
 *
 * v1.2.1 shipped only (2), and it did not work: it sampled the pad ONCE, at a
 * single instant, and it ran AFTER SDL_Init had brought up its own HID
 * session, so the libnx pad opened here was fighting SDL for it and usually
 * read nothing.  It now runs before SDL_Init and polls a real window.
 */
#include <switch.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

bool SwitchSafeBoot_HeldZLR3(void) {
  PadState pad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);
  /* ~600 ms.  The first padUpdate after initialising usually reports nothing,
   * and nobody can be expected to hold a chord on an exact frame, so sample
   * long enough to be caught -- the loop returns as soon as it sees it. */
  for (int i = 0; i < 60; i++) {
    padUpdate(&pad);
    uint64_t b = padGetButtons(&pad);
    if ((b & HidNpadButton_ZL) && (b & HidNpadButton_StickR))
      return true;
    svcSleepThread(10000000ull);   /* 10 ms */
  }
  return false;
}

/* ---- the on-screen banner ----------------------------------------------
 *
 * The first build of this escape just closed the game.  That is a clean
 * shutdown -- SRAM is flushed and the autosave is written, exactly as when
 * you quit normally -- but from the player's seat an app that vanishes with
 * no explanation is indistinguishable from a crash, and someone who reads it
 * as a crash will never touch shaders again.
 *
 * So the escape says what it is doing, in the picture, before it does it.
 * The companion cannot be used for that (it is the thing that is missing),
 * and libnx's only message applet is the error one, which writes an error
 * report into the console's system log every time it is shown.  Both are out.
 * What is left is the game's own framebuffer, which on this path is a plain
 * ARGB8888 buffer we are handed every frame -- so the text is blitted
 * straight into it with a built-in 5x7 font, the same approach main.c
 * already uses for the FPS counter.
 */
#define GLYPH_W 5
#define GLYPH_H 7

/* A-Z then space, 5x7, one byte per row, bit 4 is the leftmost pixel. */
static const uint8_t kGlyphs[27][GLYPH_H] = {
  {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, /* A */
  {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, /* B */
  {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* C */
  {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, /* D */
  {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* E */
  {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, /* F */
  {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, /* G */
  {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, /* H */
  {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* I */
  {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, /* J */
  {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* K */
  {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, /* L */
  {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, /* M */
  {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, /* N */
  {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* O */
  {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, /* P */
  {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, /* Q */
  {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, /* R */
  {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, /* S */
  {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
  {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /* U */
  {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, /* V */
  {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, /* W */
  {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, /* X */
  {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, /* Y */
  {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, /* Z */
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* space */
};

static void FillRect(uint8_t *px, int pitch, int w, int h,
                     int x0, int y0, int rw, int rh, uint32_t color) {
  for (int y = y0; y < y0 + rh; y++) {
    if (y < 0 || y >= h) continue;
    uint32_t *row = (uint32_t *)(px + (size_t)y * pitch);
    for (int x = x0; x < x0 + rw; x++)
      if (x >= 0 && x < w) row[x] = color;
  }
}

static int TextWidth(const char *s, int scale) {
  return (int)strlen(s) * (GLYPH_W + 1) * scale;
}

static void DrawText(uint8_t *px, int pitch, int w, int h,
                     int x, int y, const char *s, int scale, uint32_t color) {
  for (; *s; s++) {
    char c = *s;
    if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    int index = (c >= 'A' && c <= 'Z') ? c - 'A' : 26;
    const uint8_t *g = kGlyphs[index];
    for (int gy = 0; gy < GLYPH_H; gy++)
      for (int gx = 0; gx < GLYPH_W; gx++)
        if (g[gy] & (1 << (GLYPH_W - 1 - gx)))
          FillRect(px, pitch, w, h, x + gx * scale, y + gy * scale,
                   scale, scale, color);
    x += (GLYPH_W + 1) * scale;
  }
}

/* progress is 0..1000; the bar fills as the chord is held. */
void SwitchSafeBoot_DrawEscapeBanner(uint8_t *px, int pitch, int w, int h,
                                     unsigned progress) {
  if (!px || w <= 0 || h <= 0) return;
  int scale = w >= 512 ? 2 : 1;
  const char *l1 = "RETURN TO DUAL SCREEN";
  const char *l2 = progress >= 1000 ? "REOPEN THE GAME" : "KEEP HOLDING ZL R3";

  int pad = 6 * scale;
  int line_h = GLYPH_H * scale;
  int bar_h = 3 * scale;
  int box_w = TextWidth(l1, scale) + pad * 2;
  int inner = TextWidth(l1, scale);
  int box_h = pad * 2 + line_h * 2 + bar_h + pad;
  if (box_w > w) { box_w = w; inner = w - pad * 2; }
  int bx = (w - box_w) / 2;
  int by = h - box_h - 8 * scale;
  if (by < 0) by = 0;

  FillRect(px, pitch, w, h, bx, by, box_w, box_h, 0xff101010);
  FillRect(px, pitch, w, h, bx, by, box_w, scale, 0xffe8c260);
  FillRect(px, pitch, w, h, bx, by + box_h - scale, box_w, scale, 0xffe8c260);

  DrawText(px, pitch, w, h, bx + pad, by + pad, l1, scale, 0xffffffff);
  DrawText(px, pitch, w, h, bx + pad, by + pad + line_h + 2 * scale, l2, scale,
           0xffc0c0c0);

  int bar_y = by + pad + line_h * 2 + 4 * scale;
  FillRect(px, pitch, w, h, bx + pad, bar_y, inner, bar_h, 0xff303030);
  if (progress > 1000) progress = 1000;
  FillRect(px, pitch, w, h, bx + pad, bar_y,
           (int)((long)inner * progress / 1000), bar_h, 0xffe8c260);
}
