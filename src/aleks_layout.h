#ifndef ALEKS_LAYOUT_H_
#define ALEKS_LAYOUT_H_

/*
 * aleks_layout.h -- THE authority for where anything lands on the screen.
 *
 * There is exactly one physical coordinate system in this build: the
 * renderer's output in pixels.  Every rectangle below is in that space, and
 * every mode -- NORMAL included -- is computed here.
 *
 * WHY NORMAL IS HERE NOW.  It used to be presented through
 * SDL_RenderSetLogicalSize while DUAL and FLIP used explicit rectangles.  That
 * is two coordinate systems layered on one renderer: the compositor computed
 * destination rects in output pixels while SDL believed the world was 256x224,
 * and a mode or aspect change had to remember to switch between them.  The
 * logical size is gone; nothing calls SDL_RenderSetLogicalSize any more.
 *
 * PIXEL ASPECT, TWICE OVER.  Two separate corrections are needed, and missing
 * either one produces a wrong picture:
 *
 *   1. THE SNES FRAME.  256x224 fills a 4:3 display, so its pixels are not
 *      square: PAR = (4/3)/(256/224) = 7/6 exactly.  Fitting 256x224 by its own
 *      numbers gives 8:7, which is 14% too narrow.
 *
 *   2. THE DRAW SURFACE.  On this platform SDL reports a 1280x1280 window and
 *      renderer while the panel is 16:9 -- measured on hardware and reproduced
 *      in Eden.  The surface is stretched to the display, so a square in
 *      surface coordinates is NOT square on screen; the vertical squash is
 *      0.5625, the same in handheld (720/1280) and docked (1080/1280 over
 *      1920/1280).  A shape that is correct in surface space is therefore
 *      wrong on the panel, which is exactly the "wide and squashed" picture.
 *
 * AleksLayout_SetSurface() supplies the second correction and every fit in
 * this file applies both, so callers only ever think in DISPLAY aspect.
 */

#include <stdbool.h>

/* The Esteban companion's logical surface: the 3DS bottom screen the donor UI
 * was written against, and what second_screen_sdl.c still draws. */
#define ALEKS_COMPANION_W 320
#define ALEKS_COMPANION_H 240

/* Design canvases.  Layout is worked out on a fixed canvas and mapped
 * uniformly onto the real output, so a docked 1080p surface behaves like the
 * handheld one. */
#define ALEKS_DUAL_DESIGN_W 1280
#define ALEKS_DUAL_DESIGN_H 720
#define ALEKS_FLIP_CANVAS_W 720
#define ALEKS_FLIP_CANVAS_H 1280

/* SNES pixel aspect ratio, exact: 256 columns x 224 rows displayed as 4:3. */
#define ALEKS_PAR_NUM 7
#define ALEKS_PAR_DEN 6

typedef enum AleksLayoutMode {
  ALEKS_LAYOUT_NORMAL = 0,   /* game alone, aspect-fitted to the output */
  ALEKS_LAYOUT_DUAL,         /* game and companion side by side */
  ALEKS_LAYOUT_FLIP,         /* portrait canvas, rotated 270 for Flip Grip */
} AleksLayoutMode;

typedef struct AleksRect { int x, y, w, h; } AleksRect;

typedef struct AleksLayout {
  AleksLayoutMode mode;
  /* The space game/companion are expressed in: the physical output for NORMAL
   * and DUAL, ALEKS_FLIP_CANVAS_W/H for FLIP. */
  int logical_w, logical_h;
  int rotation_degrees;      /* 0, or 270 for FLIP */
  AleksRect game;
  AleksRect companion;       /* w == 0 when the mode shows no companion */
  int effective_gap;         /* after clamping, so a settings row can be honest */
  /* FLIP: the canvas is drawn into this rect (surface coords) and then rotated.
   * Non-uniform on purpose -- the surface is not the shape of the panel. */
  AleksRect flip_dst;
  float flip_scale_x, flip_scale_y;   /* canvas -> surface, per axis */
} AleksLayout;

/*
 * Tell the layout what it is drawing into and what that ends up on.  Called
 * once per frame from the resolver, before anything is computed.  surface_* is
 * the renderer's own coordinate space; display_* is the real panel.  When they
 * disagree in shape, every fit below compensates.
 */
void AleksLayout_SetSurface(int surface_w, int surface_h,
                            int display_w, int display_h);

/*
 * The DISPLAYED aspect of a frame that is src_w x src_h engine pixels, as a
 * ratio num:den.  256x224 gives 4:3; the widescreen frame gives the same
 * vertical scale with more width.  This is the aspect the PLAYER should see --
 * the surface correction is applied later, inside the fit.
 */
void AleksLayout_FrameAspect(int src_w, int src_h, int *num, int *den);

/*
 * The one aspect-fit helper.  Largest rectangle that fits inside box_w x box_h
 * and APPEARS as num:den on the panel, centred on (box_x, box_y).  Everything
 * that places a picture goes through this -- there is no second fit anywhere.
 */
void AleksLayout_Fit(int box_x, int box_y, int box_w, int box_h,
                     int num, int den, AleksRect *out);

/* Fill *out for a display of out_w x out_h given the live engine frame size.
 * Pure arithmetic, keeps no state, safe every frame. */
void AleksLayout_Compute(AleksLayoutMode mode, int out_w, int out_h,
                         int src_w, int src_h, AleksLayout *out);

/* Physical point -> companion-local (0..319, 0..239).  False when the point is
 * outside the companion, which is what keeps a tap on the game half from
 * reaching the UI.  Undoes the FLIP rotation first. */
bool AleksLayout_MapTouch(const AleksLayout *layout, int out_w, int out_h,
                          int phys_x, int phys_y, int *local_x, int *local_y);

#endif  /* ALEKS_LAYOUT_H_ */
