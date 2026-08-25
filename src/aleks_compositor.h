#pragma once
#include <stdbool.h>
#include <SDL.h>

/*
 * The single Switch compositor.  It owns the gameplay texture, the companion
 * texture, the destination rectangles, the rotation, the final overlays and
 * the one final present -- and nothing else.  Every pixel of the companion
 * itself is drawn by the donor UI in second_screen_sdl.c; this file never
 * paints a map, an inventory, a gear page or a HUD of its own.
 */

/*
 * THE authority for the physical Switch output size.
 *
 * The Switch operation mode decides it, and nothing else does:
 *     HANDHELD -> 1280x720
 *     DOCKED   -> 1920x1080
 *
 * Nothing is inferred from SDL any more.  SDL_GetRendererOutputSize reports
 * the CURRENT RENDER TARGET when one is bound rather than the display, and a
 * hardware crash report showed 1280x1280 reaching the DUAL layout on a
 * 1280x720 panel.  The window has been seen reporting a square surface too.
 * Both are now read for the log and ignored for the decision, so a bad value
 * cannot reach the layout by construction instead of by guard.
 *
 * FLIP's 720x1280 is a DESIGN CANVAS and never comes from here.
 */
bool AleksDisplay_GetPhysicalOutputSize(int *out_w, int *out_h);

/* One line at boot naming window / renderer / resolved / source. */
void AleksDisplay_LogBoot(void (*log)(const char *fmt, ...));

bool AleksCompositor_Init(SDL_Window *window, SDL_Renderer *renderer);
void AleksCompositor_PrepareRenderer(SDL_Renderer *renderer, const SDL_Rect *game_source);
bool AleksCompositor_Present(SDL_Renderer *renderer, SDL_Texture *game, const SDL_Rect *game_source);
void AleksCompositor_Shutdown(void);

void AleksCompositor_CyclePage(void);
void AleksCompositor_ToggleSettings(void);

/* NORMAL-mode companion overlay: same renderer, same page state, temporarily
 * drawn over a dimmed game. */
void AleksCompositor_ToggleOverlay(void);
bool AleksCompositor_IsOverlayOpen(void);

/* Returns true when the companion consumed the press.  Call this FIRST, ahead
 * of hardware chords, ALEKS shortcuts and gameplay, and stop on true. */
bool AleksCompositor_SettingsInput(int button);

/* Tell the compositor the display mode changed, so an interactive companion
 * session (SETTINGS / SAVE STATES / an open modal) stays visible instead of
 * becoming an invisible menu that still owns the pad.  Ordinary play in
 * DUAL/FLIP does NOT pop an overlay on the way to NORMAL. */
void AleksCompositor_NotifyDisplayModeChanged(int old_mode, int new_mode);

/* Quick Save opens the companion to ask for confirmation (it may be pressed
 * with nothing on screen); Quick Load is immediate and silent. */
void AleksCompositor_ArmQuickSaveConfirm(void);
void AleksCompositor_QuickLoad(void);

/* Press gating: a button a menu consumed stays consumed until it is
 * physically released, so closing a menu with B cannot hand that same press
 * to Zelda, and a chord's still-held components cannot fire their
 * single-button actions on the next frame. */
bool AleksCompositor_IsConsumedUntilRelease(int button);
void AleksCompositor_ConsumeUntilRelease(int button);
void AleksCompositor_ReleaseButton(int button);

/* Touch, in physical output pixels.  TouchDown starts the gesture clock so
 * the donor's tap/hold distinction survives; HandleTouch resolves the release
 * through the live layout. */
void AleksCompositor_TouchDown(int physical_x, int physical_y);
bool AleksCompositor_HandleTouch(int physical_x, int physical_y);
