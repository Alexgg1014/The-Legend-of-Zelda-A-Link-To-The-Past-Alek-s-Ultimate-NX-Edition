/*
 * aleks_compositor.c -- one renderer, one final present.
 *
 * WHAT THIS FILE IS NOT: it is not a UI.  The companion screen is drawn in
 * full by second_screen_sdl.c, the Esteban 3DS bottom-screen renderer, into
 * its own 320x240 surface.  The map, the dungeon automap, the item grid, the
 * gear page, the sidebar HUD, the tab bar, the cinema Triforce card, the
 * guide page and the settings menu all live there and are the donor's.
 * Nothing here may grow a second copy of any of them.
 *
 * WHAT THIS FILE IS: where that finished frame and the finished gameplay
 * frame land on the physical Switch display, how a physical touch gets back
 * to a companion-local coordinate, and where the one final present happens.
 * The arithmetic is in aleks_layout.c (adapted from the final ALEKS TMC
 * worktree, the physically tested authority for NORMAL / DUAL / FLIP).
 */

#include "aleks_compositor.h"
#include "aleks_layout.h"
#include "config.h"
#include "second_screen.h"
#include "story_guide.h"
#include "aleks_crashctx.h"
#include "aleks_ra.h"
#include "variables.h"

#ifdef __SWITCH__
#include <switch.h>   /* appletGetOperationMode: the authoritative output size */
#endif

/* From second_screen_sdl.c (the donor UI, Switch entry points). */
bool SecondScreenSDL_InitSwitch(SDL_Window *window, SDL_Renderer *renderer);
void SecondScreenSDL_Update(int logic_frames);
void SecondScreenSDL_Shutdown(void);
SDL_Texture *SecondScreenSDL_GetTexture(void);
void SecondScreenSDL_TapLocal(int x, int y, int long_press);
void SecondScreenSDL_CycleTab(void);
void SecondScreenSDL_CycleTabDir(int dir);
void SecondScreenSDL_OpenSettings(void);
void SecondScreenSDL_ToggleSettings(void);
void SecondScreenSDL_SetTab(int page);
int SecondScreenSDL_GetTab(void);
void SecondScreenSDL_MoveSelection(int delta);
bool SecondScreenSDL_ActivateSelection(int dir);
bool SecondScreenSDL_LeaveSubmenu(void);
int SecondScreenSDL_SettingsRowCount(void);
bool SecondScreenSDL_TogglePinAtLink(void);
bool SecondScreenSDL_IsSaveTab(void);
bool SecondScreenSDL_IsMapTab(void);
bool SecondScreenSDL_IsSettingsTab(void);
void SecondScreenSDL_ToggleWholeMap(void);
void SecondScreenSDL_LeaveTransientPage(void);
bool SecondScreenSDL_ConfirmActive(void);
void SecondScreenSDL_ConfirmMove(int dir);
bool SecondScreenSDL_ConfirmCommit(void);
bool SecondScreenSDL_ConfirmCancel(void);
void SecondScreenSDL_SaveArmSave(void);
void SecondScreenSDL_SaveLoadSelected(void);
void SecondScreenSDL_ArmQuickSave(int restore_tab, bool close_overlay_after);
void SecondScreenSDL_QuickLoad(void);

/*
 * SWITCH A/B, AND WHY THE NAMES LOOK BACKWARDS.
 *
 * SDL names controller buttons POSITIONALLY, after the Xbox layout:
 * SDL_CONTROLLER_BUTTON_A is the BOTTOM face button and _B is the RIGHT one.
 * On a Switch pad the bottom button is physically B and the right button is
 * physically A -- the opposite of the labels.  Zelda3's kGamepadBtn_A/_B come
 * straight from those SDL indices, so binding "confirm" to kGamepadBtn_A gave
 * the hardware bug this pass fixes: pressing physical A cancelled.
 *
 * Nintendo UI semantics are physical A = confirm, physical B = back, so the
 * ALEKS UI layer binds confirm to kGamepadBtn_B and back to kGamepadBtn_A.
 * This mapping is UI-ONLY: Zelda gameplay bindings are untouched and stay
 * whatever the player has set in CONTROLS.
 */
#define ALEKS_UI_CONFIRM kGamepadBtn_B
#define ALEKS_UI_CANCEL  kGamepadBtn_A

/*
 * The same swap, for the save-state page.  The player is told "X saves, Y
 * loads" and means the letters printed on the controller, so:
 *   physical X = SDL Y = kGamepadBtn_Y
 *   physical Y = SDL X = kGamepadBtn_X
 * Getting this backwards would make the save button load.  second_screen_sdl
 * .c's kSwitchButtonLabel is the same mapping on the display side.
 */
#define ALEKS_UI_SAVE kGamepadBtn_Y
#define ALEKS_UI_LOAD kGamepadBtn_X

static SDL_Window *g_window;
static SDL_Renderer *g_renderer;
static SDL_Texture *g_flip_canvas;
static bool g_ready;

/* The two physical Switch presentation sizes.  Centralised here so no layout
 * file ever hardcodes a resolution of its own.  FLIP's 720x1280 is a DESIGN
 * CANVAS (aleks_layout.c) and is deliberately not one of these. */
#define ALEKS_FALLBACK_OUTPUT_W 1280   /* handheld */
#define ALEKS_FALLBACK_OUTPUT_H 720
#define ALEKS_DOCKED_OUTPUT_W   1920
#define ALEKS_DOCKED_OUTPUT_H   1080

/* What the resolver last returned, for the boot log and the crash record. */
static int g_resolved_w, g_resolved_h;
static const char *g_resolved_source = "unresolved";

/* NORMAL-mode companion overlay: the same renderer and the same page state,
 * just a different destination rect and a dimmed game behind it. */
static bool g_overlay_open;
static SDL_Rect g_overlay_rect;

/* Touch gesture timing, so the donor's tap/hold distinction (hold = map pin)
 * survives the trip through the compositor.  Same threshold TMC uses. */
#define ALEKS_LONGPRESS_MS 500
static bool g_press_active;
static uint32_t g_press_started;

/* The layout the last present used.  Touch resolves against this rather than
 * against a fresh computation, so a tap can never disagree with what the user
 * is looking at. */
static AleksLayout g_live_layout;
static int g_live_out_w, g_live_out_h;

static AleksLayoutMode current_mode(void) {
  switch (g_config.aleks_display_mode) {
  case 1:  return ALEKS_LAYOUT_DUAL;
  case 2:  return ALEKS_LAYOUT_FLIP;
  default: return ALEKS_LAYOUT_NORMAL;
  }
}

/*
 * g_overlay_open is a NORMAL-PRESENTATION flag and nothing else.  It can still
 * be set while DUAL/FLIP is active (the mode changed while it was open), so
 * testing it bare would let a stale flag capture the controller in a mode that
 * never uses it.  Every test pairs it with the mode.
 */
static bool normal_overlay_active(void) {
  return current_mode() == ALEKS_LAYOUT_NORMAL && g_overlay_open;
}

/* Is the companion surface on screen at all?  DUAL/FLIP always show it. */
static bool companion_visible(void) {
  return current_mode() != ALEKS_LAYOUT_NORMAL || normal_overlay_active();
}

/*
 * UI that is INTERACTIVE, as opposed to merely visible.  Browsing MAP/ITEMS/
 * GEAR in DUAL is not this: the pad still belongs to Link, because the player
 * is playing.  SETTINGS, SAVE STATES and the modal are.
 *
 * This is also what the NORMAL presentation consults, which is the whole
 * point -- a page that owns input is therefore always drawn.
 */
static bool companion_ui_state_active(void) {
  /* Asks which PAGE is selected, never how many rows were painted.
   * SettingsRowCount() is only filled in while the page is being drawn, so
   * using it here was circular: in NORMAL with the overlay closed it reads 0,
   * the page is therefore not drawn, and it stays 0 forever -- the SETTINGS
   * shortcut looked like it did nothing at all. */
  return SecondScreenSDL_ConfirmActive() ||
         SecondScreenSDL_IsSettingsTab() ||
         SecondScreenSDL_IsSaveTab();
}

static bool companion_ui_owns_input(void) {
  return companion_ui_state_active() || normal_overlay_active();
}

/*
 * Buttons that a menu consumed and that must stay consumed until physically
 * released.  Without this, B closing a menu is still down on the next frame
 * and Zelda reads it as a fresh press; likewise the ZR still held from a
 * ZR+L3 chord would fire ZR's single-button Quick Item.
 */
static uint32_t g_ui_consumed_until_release;

static void consume_until_release(int button) {
  if (button >= 0 && button < 32)
    g_ui_consumed_until_release |= 1u << button;
}

bool AleksCompositor_IsConsumedUntilRelease(int button) {
  if (button < 0 || button >= 32) return false;
  return (g_ui_consumed_until_release & (1u << button)) != 0;
}

void AleksCompositor_ReleaseButton(int button) {
  if (button >= 0 && button < 32)
    g_ui_consumed_until_release &= ~(1u << button);
}

void AleksCompositor_ConsumeUntilRelease(int button) {
  consume_until_release(button);
}

/* Close the NORMAL overlay and forget its rect. */
static void close_overlay(void) {
  g_overlay_open = false;
  g_overlay_rect.w = g_overlay_rect.h = 0;
}

/*
 * Resolve the physical output size.  See the header for why this exists.
 *
 * THE SWITCH OPERATION MODE IS AUTHORITATIVE.  Nothing is inferred any more:
 * handheld is 1280x720 and docked is 1920x1080, full stop.  SDL's window and
 * renderer are still read, but ONLY to log a disagreement -- they can no
 * longer decide the number.  That is what keeps the 1280x1280 reading (a
 * square drawing surface, or a bound render target answering with its own
 * texture size) out of the layout by construction rather than by guard.
 *
 * Queried every resolve, so docking or undocking mid-session is picked up on
 * the next frame.
 */
bool AleksDisplay_GetPhysicalOutputSize(int *out_w, int *out_h) {
  static bool logged_mismatch;
  int win_w = 0, win_h = 0, ren_w = 0, ren_h = 0;
  const char *source;
  int w, h;

  if (g_window)
    SDL_GetWindowSize(g_window, &win_w, &win_h);

  /* Diagnostic only.  Skipped while a target is bound, where the renderer
   * would report the texture instead of the screen. */
  if (g_renderer && SDL_GetRenderTarget(g_renderer) == NULL) {
    if (SDL_GetRendererOutputSize(g_renderer, &ren_w, &ren_h) != 0)
      ren_w = ren_h = 0;
  }

#ifdef __SWITCH__
  if (appletGetOperationMode() == AppletOperationMode_Console) {
    w = ALEKS_DOCKED_OUTPUT_W; h = ALEKS_DOCKED_OUTPUT_H; source = "DOCKED";
  } else {
    w = ALEKS_FALLBACK_OUTPUT_W; h = ALEKS_FALLBACK_OUTPUT_H; source = "HANDHELD";
  }
  /*
   * The mode says what the display IS; the window says what we can actually
   * draw into.  main.c creates the window at the operation mode's resolution,
   * so these normally agree exactly.  They can still diverge after a dock
   * transition, because this backend will not re-report a window resized
   * after creation -- and rects computed for 1920x1080 drawn into a 1280x720
   * surface would push the picture off the screen.
   *
   * So the mode remains authoritative for WHICH pair we use, and the window
   * caps it.  Both are 16:9, so this is a uniform scale and the picture is
   * geometrically identical either way.  What can never happen is a square
   * 1280x1280 reaching the layout: it is not one of the two pairs above.
   */
  if (win_w > 0 && win_h > 0 && (win_w != w || win_h != h)) {
    bool window_is_16_9 = (long)win_w * 9 == (long)win_h * 16;
    if (window_is_16_9) {
      w = win_w; h = win_h;
      source = (source[0] == 'D') ? "DOCKED (window)" : "HANDHELD (window)";
    }
    /* A window that is not 16:9 (the 1280x1280 case) is ignored outright. */
  }
#else
  /* Desktop/dev build: there is no operation mode, so the window is the
   * display.  Unchanged behaviour off-console. */
  if (win_w > 0 && win_h > 0) {
    w = win_w; h = win_h; source = "window";
  } else {
    w = ALEKS_FALLBACK_OUTPUT_W; h = ALEKS_FALLBACK_OUTPUT_H; source = "fallback";
  }
#endif

  /* One line, once, when SDL disagrees with the platform -- kept because it
   * is how the 1280x1280 reading was found in the first place. */
  if (!logged_mismatch &&
      ((win_w > 0 && (win_w != w || win_h != h)) ||
       (ren_w > 0 && (ren_w != w || ren_h != h)))) {
    logged_mismatch = true;
    AleksCrash_Breadcrumb("SWITCH OUTPUT", ren_h, win_h);
    AleksCrash_LogOutputMismatch(ren_w, ren_h, win_w, win_h, w, h,
                                 "operation mode is authoritative; SDL values are diagnostic");
  }

  g_resolved_w = w;
  g_resolved_h = h;
  g_resolved_source = source;

  /*
   * The surface we draw into is not necessarily the shape of the panel: this
   * platform has been seen reporting a square 1280x1280 drawing surface.  The
   * layout works in PHYSICAL units (w,h above), so tell it the real surface
   * as well and let it carry the correction -- that is exactly what
   * AleksLayout_SetSurface exists for.  Equal sizes reduce to 1:1 and nothing
   * changes.
   */
  {
    int surf_w = win_w > 0 ? win_w : w;
    int surf_h = win_h > 0 ? win_h : h;
    AleksLayout_SetSurface(surf_w, surf_h, w, h);
  }
  AleksCrash_PublishOutput(w, h, source);

  if (out_w) *out_w = w;
  if (out_h) *out_h = h;
  return w > 0 && h > 0;
}

void AleksDisplay_LogBoot(void (*log)(const char *fmt, ...)) {
  int win_w = 0, win_h = 0, ren_w = 0, ren_h = 0, w = 0, h = 0;
  if (!log) return;
  if (g_window) SDL_GetWindowSize(g_window, &win_w, &win_h);
  if (g_renderer && SDL_GetRenderTarget(g_renderer) == NULL)
    SDL_GetRendererOutputSize(g_renderer, &ren_w, &ren_h);
  AleksDisplay_GetPhysicalOutputSize(&w, &h);
  log("SWITCH OUTPUT: mode=%s physical=%dx%d sdl_window=%dx%d "
      "sdl_renderer=%dx%d using=%dx%d",
      g_resolved_source, w, h, win_w, win_h, ren_w, ren_h, w, h);
}

bool AleksCompositor_Init(SDL_Window *window, SDL_Renderer *renderer) {
  if (g_ready) return true;
  if (!window || !renderer) return false;
  if (!SecondScreenSDL_InitSwitch(window, renderer)) return false;
  g_window = window;
  g_renderer = renderer;
  g_ready = true;
  return true;
}

void AleksCompositor_Shutdown(void) {
  if (g_flip_canvas) { SDL_DestroyTexture(g_flip_canvas); g_flip_canvas = NULL; }
  SecondScreenSDL_Shutdown();
  g_renderer = NULL;
  g_ready = false;
}

bool AleksCompositor_IsOverlayOpen(void) { return g_overlay_open; }

/*
 * There is no logical size any more, in any mode.
 *
 * It used to be set for NORMAL and cleared for DUAL/FLIP, which meant SDL and
 * the compositor disagreed about what a coordinate meant depending on the
 * mode -- and every mode or aspect change had to remember to switch between
 * the two.  Every rectangle is now computed in renderer-output pixels by
 * aleks_layout.c, so this only has to guarantee the renderer is in that
 * un-transformed state.  Called once; cheap enough to keep idempotent.
 */
void AleksCompositor_PrepareRenderer(SDL_Renderer *renderer, const SDL_Rect *src) {
  static bool cleared;
  (void)src;
  if (!renderer || cleared) return;
  SDL_RenderSetLogicalSize(renderer, 0, 0);
  SDL_RenderSetViewport(renderer, NULL);
  SDL_RenderSetScale(renderer, 1.0f, 1.0f);
  cleared = true;
}

/* SCREEN ORDER swaps which side each half takes.  Done on the finished
 * rectangles so the layout arithmetic (and the touch inverse that reads it)
 * stays a single implementation. */
static void apply_screen_order(AleksLayout *l) {
  AleksRect g = l->game, c = l->companion;
  if (!g_config.aleks_screen_order || c.w <= 0) return;
  if (l->rotation_degrees == 270) {
    /* FLIP stacks vertically on its canvas. */
    int gh = g.h, ch = c.h;
    int top = g.y < c.y ? g.y : c.y;
    int gap = (g.y < c.y) ? (c.y - (g.y + gh)) : (g.y - (c.y + ch));
    l->companion.y = top;
    l->game.y = top + ch + gap;
  } else {
    int left = g.x < c.x ? g.x : c.x;
    int gap = (g.x < c.x) ? (c.x - (g.x + g.w)) : (g.x - (c.x + c.w));
    l->companion.x = left;
    l->game.x = left + c.w + gap;
  }
}

static SDL_Rect to_sdl(AleksRect r) {
  SDL_Rect out = { r.x, r.y, r.w, r.h };
  return out;
}

static void present_dual(SDL_Renderer *r, SDL_Texture *game, const SDL_Rect *src,
                         SDL_Texture *companion) {
  SDL_Rect g = to_sdl(g_live_layout.game);
  SDL_Rect c = to_sdl(g_live_layout.companion);
  SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
  SDL_RenderClear(r);
  SDL_RenderCopy(r, game, src, &g);
  if (companion) SDL_RenderCopy(r, companion, NULL, &c);
}

/* NORMAL: one aspect-fitted rectangle, the same fit DUAL and FLIP use. */
static void present_normal(SDL_Renderer *r, SDL_Texture *game, const SDL_Rect *src) {
  SDL_Rect g = to_sdl(g_live_layout.game);
  SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
  SDL_RenderClear(r);
  SDL_RenderCopy(r, game, src, &g);
}

/*
 * FLIP composes the whole 720x1280 portrait canvas into an offscreen texture
 * and then rotates that single texture onto the display.  Composing first and
 * rotating once is what keeps the inverse touch transform in aleks_layout.c a
 * single well-defined operation instead of one per viewport.
 */
static bool present_flip(SDL_Renderer *r, SDL_Texture *game, const SDL_Rect *src,
                         SDL_Texture *companion) {
  SDL_Rect g = to_sdl(g_live_layout.game);
  SDL_Rect c = to_sdl(g_live_layout.companion);
  /* Where the canvas lands, already corrected for the surface shape by
   * aleks_layout.c -- the compositor does not second-guess it. */
  SDL_Rect dst = to_sdl(g_live_layout.flip_dst);

  if (!g_flip_canvas) {
    g_flip_canvas = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_TARGET,
                                      ALEKS_FLIP_CANVAS_W, ALEKS_FLIP_CANVAS_H);
    if (!g_flip_canvas) return false;
    SDL_SetTextureBlendMode(g_flip_canvas, SDL_BLENDMODE_NONE);
  }
  if (SDL_SetRenderTarget(r, g_flip_canvas) != 0) return false;
  SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
  SDL_RenderClear(r);
  SDL_RenderCopy(r, game, src, &g);
  if (companion) SDL_RenderCopy(r, companion, NULL, &c);
  SDL_SetRenderTarget(r, NULL);

  SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
  SDL_RenderClear(r);
  /* Centred on the display and turned clockwise about that centre; the
   * inverse of exactly this is what AleksLayout_MapTouch undoes. */
  if (dst.w <= 0 || dst.h <= 0) return false;
  SDL_RenderCopyEx(r, g_flip_canvas, NULL, &dst, 270, NULL, SDL_FLIP_NONE);
  return true;
}

/*
 * NORMAL overlay: the game full-screen and dimmed, with the SAME companion
 * texture drawn large over it.  No second renderer, no second UI, no second
 * page state -- only a different destination rect, which is also what the
 * touch mapping reads, so taps keep working without a special case.
 */
static void present_overlay(SDL_Renderer *r, SDL_Texture *game, const SDL_Rect *src,
                            SDL_Texture *companion) {
  int side = g_live_out_h * 92 / 100;
  int w = side * ALEKS_COMPANION_W / ALEKS_COMPANION_H;
  SDL_Rect full = { 0, 0, g_live_out_w, g_live_out_h };
  SDL_Rect fit;

  /* Game underneath, aspect-fitted, then dimmed. */
  {
    int gh = g_live_out_w * src->h / src->w;
    if (gh > g_live_out_h) {
      fit.h = g_live_out_h;
      fit.w = g_live_out_h * src->w / src->h;
    } else {
      fit.w = g_live_out_w;
      fit.h = gh;
    }
    fit.x = (g_live_out_w - fit.w) / 2;
    fit.y = (g_live_out_h - fit.h) / 2;
  }
  SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
  SDL_RenderClear(r);
  SDL_RenderCopy(r, game, src, &fit);
  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(r, 0, 0, 0, 190);
  SDL_RenderFillRect(r, &full);
  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

  if (w > g_live_out_w) { w = g_live_out_w; side = w * ALEKS_COMPANION_H / ALEKS_COMPANION_W; }
  g_overlay_rect.w = w;
  g_overlay_rect.h = side;
  g_overlay_rect.x = (g_live_out_w - w) / 2;
  g_overlay_rect.y = (g_live_out_h - side) / 2;
  if (companion) SDL_RenderCopy(r, companion, NULL, &g_overlay_rect);
}

/*
 * EVERY mode presents from here now, NORMAL included.  main.c has no fallback
 * blit any more, so there is one final present and one place that decides
 * where the picture goes.
 */
bool AleksCompositor_Present(SDL_Renderer *r, SDL_Texture *game, const SDL_Rect *src) {
  AleksLayoutMode mode = current_mode();
  SDL_Texture *companion = NULL;
  int out_w = 0, out_h = 0;

  if (!r || !game || !src) return false;

  /* ONE authority for the physical size -- see AleksDisplay_GetPhysicalOutputSize.
   * Resolved before the companion draws, because that binds a render target and
   * the renderer would then report the texture instead of the screen. */
  if (!AleksDisplay_GetPhysicalOutputSize(&out_w, &out_h))
    return false;

  /* The companion draws into its own target and hands the renderer back before
   * returning, so everything below is on the backbuffer.  Skipped entirely in
   * plain NORMAL -- nothing would consume it. */
  if (g_ready && (mode != ALEKS_LAYOUT_NORMAL || g_overlay_open ||
                  companion_ui_state_active())) {
    SecondScreenSDL_Update(1);
    companion = SecondScreenSDL_GetTexture();
  }

  g_live_out_w = out_w;
  g_live_out_h = out_h;
  AleksLayout_Compute(mode, out_w, out_h, src->w, src->h, &g_live_layout);
  apply_screen_order(&g_live_layout);

  /* Interactive UI is ALWAYS drawn, even if the overlay flag was never set --
   * that is what makes "menu owns input" and "menu is on screen" the same
   * statement instead of two that can drift apart. */
  if (mode == ALEKS_LAYOUT_NORMAL && (g_overlay_open || companion_ui_state_active())) {
    present_overlay(r, game, src, companion);
  } else if (mode == ALEKS_LAYOUT_FLIP) {
    if (!present_flip(r, game, src, companion)) {
      /* Fall back to the mode that cannot fail rather than showing nothing --
       * but recompute first: g_live_layout still holds FLIP canvas
       * coordinates, and presenting those as output pixels would draw one
       * visibly wrong frame. */
      AleksCrash_LogSdlOnce("flip canvas");
      AleksCrash_Breadcrumb("FLIP FALLBACK", 0, 0);
      g_config.aleks_display_mode = 0;
      AleksLayout_Compute(ALEKS_LAYOUT_NORMAL, out_w, out_h, src->w, src->h,
                          &g_live_layout);
      present_normal(r, game, src);
    }
  } else if (mode == ALEKS_LAYOUT_DUAL) {
    present_dual(r, game, src, companion);
  } else {
    present_normal(r, game, src);
  }

  /* Everything the crash reporter wants to know about the picture, captured
   * while the renderer is healthy (see aleks_crashctx.h). */
  {
    static int logged;
    if (logged < 3) {
      SDL_DisplayMode m;
      int dw = out_w, dh = out_h;
      if (SDL_GetCurrentDisplayMode(0, &m) == 0) { dw = m.w; dh = m.h; }
      logged++;
      /* In FLIP the rects are CANVAS coordinates, not surface ones: they go
       * through flip_dst and the rotation first.  Hand the log that mapping
       * so it reports what the player actually sees. */
      float kx = 1.0f, ky = 1.0f;
      if (g_live_layout.rotation_degrees == 270 && out_w > 0 && out_h > 0) {
        kx = g_live_layout.flip_scale_x * ((float)dh / (float)out_h);
        ky = g_live_layout.flip_scale_y * ((float)dw / (float)out_w);
      }
      AleksCrash_LogGeometry(mode, src->w, src->h, out_w, out_h, dw, dh,
                             g_live_layout.game.x, g_live_layout.game.y,
                             g_live_layout.game.w, g_live_layout.game.h,
                             g_live_layout.companion.w, g_live_layout.companion.h,
                             g_live_layout.rotation_degrees, kx, ky);
    }
  }
  AleksCrash_PublishDisplay(
    (int)mode, src->w, src->h, out_w, out_h,
    g_live_layout.game.x, g_live_layout.game.y,
    g_live_layout.game.w, g_live_layout.game.h,
    g_live_layout.companion.x, g_live_layout.companion.y,
    g_live_layout.companion.w, g_live_layout.companion.h,
    g_live_layout.rotation_degrees);

  /*
   * THE final overlay stage (donor: Port_PPU_DrawAppOverlays).  Gameplay,
   * companion and any modal have all been composed onto the backbuffer by
   * now, and no render target is bound, so the toast draws in real output
   * pixels and lands identically in NORMAL, DUAL and FLIP -- one call site,
   * so it can never be drawn twice or missed in a mode.
   */
  AleksRA_DrawToast(r, out_w, out_h);

  SDL_RenderPresent(r);
  return true;
}

/* ---- touch ------------------------------------------------------------- */

void AleksCompositor_TouchDown(int physical_x, int physical_y) {
  (void)physical_x; (void)physical_y;
  g_press_active = true;
  g_press_started = SDL_GetTicks();
}

bool AleksCompositor_HandleTouch(int physical_x, int physical_y) {
  uint32_t held = g_press_active ? (SDL_GetTicks() - g_press_started) : 0;
  int lx = 0, ly = 0;
  g_press_active = false;

  if (!g_ready || !g_config.aleks_touch_ui) return false;

  /* While the NORMAL overlay is up, the overlay rect IS the companion; same
   * surface and the same scaling below, only the destination differs. */
  if (current_mode() == ALEKS_LAYOUT_NORMAL) {
    if ((!g_overlay_open && !companion_ui_state_active()) ||
        g_overlay_rect.w <= 0) return false;
    if (physical_x < g_overlay_rect.x ||
        physical_x >= g_overlay_rect.x + g_overlay_rect.w ||
        physical_y < g_overlay_rect.y ||
        physical_y >= g_overlay_rect.y + g_overlay_rect.h)
      return false;
    lx = (physical_x - g_overlay_rect.x) * ALEKS_COMPANION_W / g_overlay_rect.w;
    ly = (physical_y - g_overlay_rect.y) * ALEKS_COMPANION_H / g_overlay_rect.h;
  } else {
    /* The layout's companion rect IS the destination rect -- there is no
     * second derivation to keep in step. */
    if (!AleksLayout_MapTouch(&g_live_layout, g_live_out_w, g_live_out_h,
                              physical_x, physical_y, &lx, &ly))
      return false;
  }
  SecondScreenSDL_TapLocal(lx, ly, held >= ALEKS_LONGPRESS_MS ? 1 : 0);
  return true;
}

/* ---- controller -------------------------------------------------------- */

void AleksCompositor_CyclePage(void) {
  if (!g_ready) return;
  SecondScreenSDL_CycleTab();
}

void AleksCompositor_ToggleSettings(void) {
  if (!g_ready) return;
  SecondScreenSDL_ToggleSettings();
}

/*
 * GLOBAL SETTINGS (ZL+R3).  v1.0.0 required the companion to already be on
 * screen -- main.c gated the chord on companion_visible -- so from plain
 * NORMAL gameplay the shortcut did nothing at all, which is exactly what the
 * community reported.  The chord is now unconditional and this is what it
 * does:
 *
 *   companion NOT on screen (NORMAL, overlay closed)
 *       -> open the overlay AND land on SETTINGS, never toggle away from it
 *   companion on screen, any content page
 *       -> SETTINGS
 *   already on SETTINGS
 *       -> the existing toggle, back to MAP.  Same behaviour as before, so
 *          the shortcut still has a way out and cannot recurse.
 */
void AleksCompositor_OpenSettings(void) {
  bool was_visible;

  if (!g_ready) return;
  was_visible = companion_visible();
  if (current_mode() == ALEKS_LAYOUT_NORMAL && !g_overlay_open)
    g_overlay_open = true;
  if (!was_visible)
    SecondScreenSDL_OpenSettings();
  else
    SecondScreenSDL_ToggleSettings();
}

/*
 * LEFT STICK, companion navigation.  main.c turns the analog stick straight
 * into gameplay direction bits and never into button events, so the companion
 * never saw a single stick movement -- "the joystick doesn't seem to work at
 * all" in the v1.0.0 report.  This is the stick's own door into the SAME
 * navigation the D-Pad uses.
 *
 * Edge-triggered by the caller.  Deliberately NOT run through
 * consume_until_release(), which latches a BUTTON index: a synthetic press
 * there would never see a release and would deafen the real D-Pad.
 */
bool AleksCompositor_StickNav(int dx, int dy) {
  if (!g_ready) return false;

  if (SecondScreenSDL_ConfirmActive()) {
    if (dx) SecondScreenSDL_ConfirmMove(1);
    return true;          /* the modal owns the stick too */
  }
  /* TRUE MEANS "THE COMPANION HAS THE STICK", not "something moved".  The
   * caller blanks the gameplay direction bits on true, so Link must not walk
   * on a movement the menu is using -- including the frames between edges
   * while the stick is simply held over. */
  if (!companion_ui_owns_input()) return false;

  if (dy)
    SecondScreenSDL_MoveSelection(dy < 0 ? -1 : 1);
  /* Horizontal only walks the tab bar, and only on a content page.  It must
   * not activate a SETTINGS row: a stray stick nudge changing a setting is
   * the one thing worse than the stick doing nothing. */
  else if (dx && !SecondScreenSDL_IsSettingsTab() && !SecondScreenSDL_IsSaveTab())
    SecondScreenSDL_CycleTabDir(dx < 0 ? -1 : 1);
  return true;
}

/* NORMAL-mode companion: open it, and close it back to plain gameplay. */
void AleksCompositor_ToggleOverlay(void) {
  if (!g_ready) return;
  if (current_mode() != ALEKS_LAYOUT_NORMAL) {
    /* In DUAL/FLIP the companion is already on screen; the shortcut just
     * jumps to SETTINGS instead of doing nothing. */
    SecondScreenSDL_ToggleSettings();
    return;
  }
  if (g_overlay_open || companion_ui_state_active()) {
    /* Closing also drops any interactive page, so nothing is left logically
     * active behind plain gameplay. */
    SecondScreenSDL_LeaveTransientPage();
    close_overlay();
  } else {
    g_overlay_open = true;
  }
}

/*
 * DISPLAY MODE CHANGED.
 *
 * Continuity, but only for UI the player was actually using.  DUAL and FLIP
 * always DISPLAY the companion; that is not the same as navigating it, so
 * switching to NORMAL from ordinary play must give ordinary NORMAL play and
 * not pop an overlay nobody asked for.  What must survive is an interactive
 * session -- SETTINGS, SAVE STATES, an open confirmation -- which would
 * otherwise keep owning the pad with nothing on screen.  That was the
 * reported hardware bug.
 *
 * Nothing else needs preserving: tab, selection, slot and modal state all
 * live in second_screen_sdl.c and a mode change never touched them.  Only the
 * presentation flag was missing.
 */
void AleksCompositor_NotifyDisplayModeChanged(int old_mode, int new_mode) {
  if (!g_ready) return;
  if (new_mode == ALEKS_DISPLAY_NORMAL && old_mode != ALEKS_DISPLAY_NORMAL) {
    if (companion_ui_state_active()) g_overlay_open = true;
  }
}

/*
 * QUICK SAVE.  Can be pressed in plain NORMAL gameplay with nothing on
 * screen, so it OPENS the companion to ask -- arming a modal that the player
 * cannot see would be the invisible-menu bug wearing a different hat.
 * Resolving the modal restores whatever was on screen before.
 */
void AleksCompositor_ArmQuickSaveConfirm(void) {
  bool opened_overlay = false;
  int restore_tab;

  if (!g_ready) return;
  restore_tab = SecondScreenSDL_GetTab();

  if (current_mode() == ALEKS_LAYOUT_NORMAL && !g_overlay_open) {
    g_overlay_open = true;
    opened_overlay = true;
  }
  SecondScreenSDL_ArmQuickSave(restore_tab, opened_overlay);
}

/* QUICK LOAD.  Immediate, no modal, nothing to show. */
void AleksCompositor_QuickLoad(void) {
  if (!g_ready) return;
  SecondScreenSDL_QuickLoad();
}

/*
 * THE COMPANION INPUT LADDER.
 *
 * Returns true when the companion consumed the press, which keeps the same
 * press from also reaching the game.  main.c calls this FIRST, ahead of the
 * hardware chords, the ALEKS shortcuts and gameplay, and stops on true.
 *
 * The rule this file now enforces, and the bug it fixes: presentation and
 * input ownership must consult the SAME facts.  They used to disagree --
 * input asked "is tab SETTINGS?" (mode-independent) while presentation asked
 * "is the NORMAL overlay open?", so changing Display Mode to NORMAL from
 * inside DUAL/FLIP Settings left a menu that was invisible but still ate
 * every press.  Both sides now go through companion_ui_state_active().
 *
 * Priority, first match wins:
 *   1. confirmation modal   -- owns ALL input, in every display mode
 *   2. SETTINGS
 *   3. SAVE STATES
 *   4. NORMAL overlay page actions
 *   5. contextual page actions (physical R on MAP)
 *   6. not consumed -> chords, then shortcuts, then Zelda
 */
bool AleksCompositor_SettingsInput(int button) {
  bool settings_open, save_tab;

  if (!g_ready) return false;

  /* 1. The modal owns everything while it is up: the page underneath must not
   *    move and gameplay must not see a single press of it. */
  if (SecondScreenSDL_ConfirmActive()) {
    if (button == kGamepadBtn_DpadLeft || button == kGamepadBtn_DpadRight) {
      SecondScreenSDL_ConfirmMove(1);
    } else if (button == ALEKS_UI_CONFIRM) {
      if (SecondScreenSDL_ConfirmCommit()) close_overlay();
    } else if (button == ALEKS_UI_CANCEL) {
      if (SecondScreenSDL_ConfirmCancel()) close_overlay();
    }
    consume_until_release(button);
    return true;    /* anything else is swallowed, deliberately */
  }

  /*
   * 5. Contextual: physical R toggles Follow/Whole on the MAP page.
   *
   * ONLY while the NORMAL overlay owns the pad.  R1 is the DEFAULT binding
   * for the SNES "R" command (config.c, kDefaultGamepadCmds), so claiming it
   * whenever MAP is merely visible would eat R during ordinary DUAL/FLIP play
   * -- the companion is always on screen there and MAP is the default page.
   * That would break the same rule the shortcut binder enforces: an ALEKS
   * control never takes a button gameplay is bound to.  In the NORMAL overlay
   * the pad already belongs to the companion, so there is nothing to steal.
   */
  if (button == kGamepadBtn_R1 && normal_overlay_active() &&
      SecondScreenSDL_IsMapTab() && !SecondScreenSDL_IsSettingsTab()) {
    SecondScreenSDL_ToggleWholeMap();
    consume_until_release(button);
    return true;
  }

  if (!companion_ui_owns_input()) return false;

  /* Which page is selected, not how many rows were painted -- see
   * companion_ui_state_active(). */
  settings_open = SecondScreenSDL_IsSettingsTab();
  save_tab = SecondScreenSDL_IsSaveTab();

  /* 3. SAVE STATES.  Physical X saves (always confirmed), physical Y loads
   *    immediately.  See the button-label note above for why these are the
   *    SDL names they are. */
  if (save_tab && !settings_open) {
    if (button == kGamepadBtn_DpadUp) {
      SecondScreenSDL_MoveSelection(-1);
    } else if (button == kGamepadBtn_DpadDown) {
      SecondScreenSDL_MoveSelection(1);
    } else if (button == ALEKS_UI_SAVE) {
      SecondScreenSDL_SaveArmSave();
    } else if (button == ALEKS_UI_LOAD) {
      SecondScreenSDL_SaveLoadSelected();
    } else if (button == ALEKS_UI_CANCEL) {
      SecondScreenSDL_LeaveTransientPage();
      if (normal_overlay_active()) close_overlay();
    } else if (button == kGamepadBtn_L1 || button == kGamepadBtn_R1) {
      SecondScreenSDL_CycleTab();
    } else {
      return false;
    }
    consume_until_release(button);
    return true;
  }

  /* 4b. A content page (MAP / ITEMS / GEAR / GUIDE) has no row list, so left
   *     and right walk the tab bar rather than activating nothing.  This is
   *     the other half of "the cross pad doesn't seem to work at all": on
   *     those pages it genuinely did nothing at all. */
  if (!settings_open &&
      (button == kGamepadBtn_DpadLeft || button == kGamepadBtn_DpadRight)) {
    SecondScreenSDL_CycleTabDir(button == kGamepadBtn_DpadRight ? 1 : -1);
    consume_until_release(button);
    return true;
  }

  /* 2 / 4. SETTINGS rows, and the NORMAL overlay's page actions. */
  if (button == kGamepadBtn_DpadUp) {
    SecondScreenSDL_MoveSelection(-1);
  } else if (button == kGamepadBtn_DpadDown) {
    SecondScreenSDL_MoveSelection(1);
  } else if (button == ALEKS_UI_CONFIRM || button == kGamepadBtn_DpadRight) {
    if (!SecondScreenSDL_ActivateSelection(1) && normal_overlay_active()) {
      consume_until_release(button);
      return true;   /* overlay page with nothing to activate: swallow it */
    }
  } else if (button == kGamepadBtn_DpadLeft) {
    SecondScreenSDL_ActivateSelection(-1);
  } else if (button == ALEKS_UI_CANCEL) {
    /* Back out one menu; at the root, close SETTINGS; with SETTINGS closed,
     * close the overlay.  One button, one predictable ladder. */
    if (!SecondScreenSDL_LeaveSubmenu()) {
      if (settings_open) {
        SecondScreenSDL_ToggleSettings();
        /* Closing the last menu in NORMAL closes the overlay with it, so no
         * page can stay logically active behind plain gameplay. */
        if (normal_overlay_active()) close_overlay();
      } else if (normal_overlay_active()) {
        AleksCompositor_ToggleOverlay();
      }
    }
  } else if (button == kGamepadBtn_L1 || button == kGamepadBtn_R1) {
    if (!settings_open && normal_overlay_active()) SecondScreenSDL_CycleTab();
    else return false;
  } else if (button == kGamepadBtn_X) {
    /* MAP page: drop or lift a pin where Link stands.  Touch keeps the
     * precise long-press placement; this is the pad's version. */
    if (!settings_open && SecondScreenSDL_TogglePinAtLink()) {
      consume_until_release(button);
      return true;
    }
    return false;
  } else {
    return false;
  }
  consume_until_release(button);
  return true;
}
