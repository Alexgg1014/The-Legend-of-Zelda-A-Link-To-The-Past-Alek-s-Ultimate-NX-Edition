/*
 * aleks_layout.c -- every rectangle in this build, in one file.
 *
 * THE MODEL, in one paragraph.  The engine hands us a frame that is src_w
 * columns by src_h rows, where 256 columns means 4:3 (SNES pixels are 7:6, not
 * square).  AleksLayout_FrameAspect turns that into a displayed aspect;
 * AleksLayout_Fit puts that aspect in a box; every mode then differs only in
 * WHICH boxes exist.  Widescreen changes the aspect -- it never changes the
 * zoom rule, which is why extra columns now show extra world instead of
 * magnifying everything.
 *
 * SIZE SEMANTICS.  100% means "as large as this layout allows", not "some
 * fraction of a hardcoded pixel count".  The companion takes a share of the
 * canvas, the game takes the space that is left and aspect-fits inside it, and
 * the percentages scale down from there.  That is what makes the defaults
 * defensible instead of tuned: at 100% nothing is left over by accident.
 */

#include "aleks_layout.h"
#include "config.h"

/*
 * SURFACE CORRECTION.
 *
 * The renderer's coordinate space is not necessarily the shape of the panel.
 * On this platform SDL reports a 1280x1280 surface while the display is 16:9,
 * so the surface is stretched vertically by 0.5625 on its way to the screen.
 *
 * Stored as an exact rational so the fits stay integer arithmetic:
 *     surface_w/h that APPEARS as num/den  =>  w/h = (num/den) * (Sw*Dh)/(Dw*Sh)
 *
 * Defaults to 1:1, so a platform whose surface really is the display needs no
 * special case and behaves exactly as before.
 */
static long long g_corr_num = 1, g_corr_den = 1;

static long long gcd_ll(long long a, long long b) {
  while (b) { long long t = a % b; a = b; b = t; }
  return a < 0 ? -a : a;
}

void AleksLayout_SetSurface(int surface_w, int surface_h,
                            int display_w, int display_h) {
  long long n, d, g;
  if (surface_w <= 0 || surface_h <= 0 || display_w <= 0 || display_h <= 0) {
    g_corr_num = g_corr_den = 1;
    return;
  }
  n = (long long)surface_w * display_h;
  d = (long long)display_w * surface_h;
  g = gcd_ll(n, d);
  if (g > 0) { n /= g; d /= g; }
  g_corr_num = n;
  g_corr_den = d;
}

static int clamp_int(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static int pct_or(int v, int fallback) {
  return v ? v : fallback;
}

void AleksLayout_FrameAspect(int src_w, int src_h, int *num, int *den) {
  if (src_w <= 0) src_w = 256;
  if (src_h <= 0) src_h = 224;
  /* width * PAR : height.  At 256x224 this is 256*7 : 224*6 = 4:3. */
  *num = src_w * ALEKS_PAR_NUM;
  *den = src_h * ALEKS_PAR_DEN;
}

/*
 * Plain aspect fit, no surface correction.
 *
 * Used INSIDE a space that already carries the correction in its own mapping
 * -- the FLIP canvas is the case that matters.  Applying the correction twice
 * is what shrank FLIP: a 4:3 game came out 543x724 in the canvas instead of
 * 720x540, so the stack no longer filled the portrait screen.
 */
static void fit_raw(int box_x, int box_y, int box_w, int box_h,
                    long long n, long long d, AleksRect *out) {
  int w, h;
  if (box_w <= 0 || box_h <= 0 || n <= 0 || d <= 0) {
    out->x = box_x; out->y = box_y; out->w = 0; out->h = 0;
    return;
  }
  /* Height-limited when the box is wider than the aspect, else width-limited. */
  if ((long long)box_w * d >= (long long)box_h * n) {
    h = box_h;
    w = (int)(((long long)box_h * n) / d);
  } else {
    w = box_w;
    h = (int)(((long long)box_w * d) / n);
  }
  out->w = w;
  out->h = h;
  out->x = box_x + (box_w - w) / 2;
  out->y = box_y + (box_h - h) / 2;
}

void AleksLayout_Fit(int box_x, int box_y, int box_w, int box_h,
                     int num, int den, AleksRect *out) {
  long long n, d, g;
  if (num <= 0 || den <= 0) {
    out->x = box_x; out->y = box_y; out->w = 0; out->h = 0;
    return;
  }
  /* num:den is what the player must SEE.  Convert it to the surface shape that
   * produces it, then fit that.  64-bit and reduced because the two ratios
   * multiplied would otherwise overflow. */
  n = (long long)num * g_corr_num;
  d = (long long)den * g_corr_den;
  g = gcd_ll(n, d);
  if (g > 0) { n /= g; d /= g; }
  fit_raw(box_x, box_y, box_w, box_h, n, d, out);
}

/* The companion surface is 4:3 and must never be stretched, so it is always an
 * aspect fit inside whatever slot the mode gave it.  CLASSIC and TALL differ
 * in the SLOT, which is the caller's business -- not in this rule. */
static void fit_companion(AleksRect slot, AleksRect *out) {
  AleksLayout_Fit(slot.x, slot.y, slot.w, slot.h,
                  ALEKS_COMPANION_W, ALEKS_COMPANION_H, out);
}

/* The same, inside the FLIP canvas, where the correction must not be applied
 * a second time. */
static void fit_companion_raw(AleksRect slot, AleksRect *out) {
  fit_raw(slot.x, slot.y, slot.w, slot.h,
          ALEKS_COMPANION_W, ALEKS_COMPANION_H, out);
}

/*
 * NORMAL: the whole output is the box.  No logical size, no special
 * fullscreen path -- the same fit every other mode uses.
 */
static void compute_normal(int out_w, int out_h, int src_w, int src_h,
                           AleksLayout *out) {
  int num, den;
  AleksLayout_FrameAspect(src_w, src_h, &num, &den);
  AleksLayout_Fit(0, 0, out_w, out_h, num, den, &out->game);
}

/*
 * DUAL, computed DIRECTLY in surface coordinates.
 *
 * It used to be laid out on a 1280x720 design canvas and then mapped onto the
 * output.  That mapping assumed the surface had the display's shape, which on
 * this platform it does not (1280x1280), so the whole stack ended up letterboxed
 * into the middle 607 of 1080 panel pixels -- a lot of black for no reason.
 * There is no design canvas any more; the corrected fits already make the
 * result identical at any output size.
 *
 * Layout:
 *
 *   +--------------------------------------------------------------+
 *   |      |  game: fills the column left over, aspect-fitted |gap| companion |
 *   +--------------------------------------------------------------+
 *
 * The companion takes a share of the WIDTH; the game takes what is left.  That
 * ordering is deliberate: the companion has a fixed 4:3 shape and a legibility
 * floor, while the game can use any column, so sizing the companion first is
 * what stops a widescreen frame from squeezing the UI into nothing.  At 100%
 * the two plus the gap fill the canvas exactly.
 */
static void compute_dual(int out_w, int out_h, int src_w, int src_h,
                         AleksLayout *out) {
  int comp_pct = clamp_int(pct_or(g_config.aleks_dual_companion_scale, 100), 50, 130);
  int game_pct = clamp_int(pct_or(g_config.aleks_dual_game_scale, 100), 50, 100);
  int gap = clamp_int(g_config.aleks_dual_gap, 0, 128);
  int comp_w, column, num, den;
  AleksRect slot;

  /* The companion's share of the WIDTH, as a fraction of the real surface.
   * 37.5% at 100%, which is the proportion the donor panel wants. */
  comp_w = (int)((long long)out_w * 375 * comp_pct / 100000);
  if (comp_w > out_w / 2) comp_w = out_w / 2;   /* the game keeps half */

  /* The sizes win and the gap gives way (the final TMC fit rule). */
  column = out_w - comp_w - gap;
  if (column < out_w / 4) {
    gap = 0;
    column = out_w - comp_w;
  }
  out->effective_gap = gap;

  slot.x = 0; slot.y = 0; slot.w = comp_w; slot.h = out_h;
  fit_companion(slot, &out->companion);

  AleksLayout_FrameAspect(src_w, src_h, &num, &den);
  AleksLayout_Fit(0, 0, column * game_pct / 100, out_h * game_pct / 100,
                  num, den, &out->game);

  /* Centre the whole stack as a unit so the spare width splits evenly. */
  {
    int total = out->game.w + gap + out->companion.w;
    int stack_x = (out_w - total) / 2;
    if (stack_x < 0) stack_x = 0;
    out->game.x = stack_x;
    out->game.y = (out_h - out->game.h) / 2;
    out->companion.x = stack_x + out->game.w + gap;
    out->companion.y = (out_h - out->companion.h) / 2;
  }
}

/*
 * FLIP, on the fixed 720x1280 portrait canvas the compositor rotates once:
 *
 *   +------------------+  0
 *   |       game       |  aspect-fitted into the height left over
 *   +------------------+
 *   |       gap        |
 *   +------------------+
 *   |    companion     |  full canvas width at 100%
 *   +------------------+  1280
 *
 * Same ordering as DUAL, on the other axis: the companion takes a share of the
 * HEIGHT and the game fills the rest.  100% companion means the full 720 canvas
 * width, which is what stops the panel reading as a narrow strip with black
 * either side.
 */
static void compute_flip(int out_w, int out_h, int src_w, int src_h,
                         AleksLayout *out) {
  int comp_pct = clamp_int(pct_or(g_config.aleks_flip_companion_scale, 100), 50, 100);
  int game_pct = clamp_int(pct_or(g_config.aleks_flip_game_scale, 100), 50, 100);
  int gap = clamp_int(g_config.aleks_flip_gap, 0, 128);
  int comp_w, comp_h, remaining, num, den, stack_y, total;
  AleksRect slot;

  out->logical_w = ALEKS_FLIP_CANVAS_W;
  out->logical_h = ALEKS_FLIP_CANVAS_H;
  out->rotation_degrees = 270;

  /*
   * The canvas is portrait 720x1280 and is rotated onto a landscape panel, so
   * what must LOOK right is the rotated footprint: 1280 wide by 720 tall.  Fit
   * that aspect (through the surface correction, like everything else), then
   * un-rotate it to get the rectangle the canvas is actually drawn into --
   * width and height swap across the rotation.
   *
   * The two axes end up scaled differently, which is correct and unavoidable:
   * the surface is not the shape of the panel.
   */
  {
    AleksRect footprint;
    AleksLayout_Fit(0, 0, out_w, out_h,
                    ALEKS_FLIP_CANVAS_H, ALEKS_FLIP_CANVAS_W, &footprint);
    out->flip_dst.w = footprint.h;   /* swapped by the rotation */
    out->flip_dst.h = footprint.w;
    out->flip_dst.x = (out_w - out->flip_dst.w) / 2;
    out->flip_dst.y = (out_h - out->flip_dst.h) / 2;
    out->flip_scale_x = (float)out->flip_dst.w / (float)ALEKS_FLIP_CANVAS_W;
    out->flip_scale_y = (float)out->flip_dst.h / (float)ALEKS_FLIP_CANVAS_H;
    if (out->flip_scale_x <= 0.0f) out->flip_scale_x = 1.0f;
    if (out->flip_scale_y <= 0.0f) out->flip_scale_y = 1.0f;
  }

  comp_w = ALEKS_FLIP_CANVAS_W * comp_pct / 100;
  comp_h = comp_w * ALEKS_COMPANION_H / ALEKS_COMPANION_W;

  remaining = ALEKS_FLIP_CANVAS_H - comp_h - gap;
  if (remaining < 200) {
    gap = 0;
    remaining = ALEKS_FLIP_CANVAS_H - comp_h;
  }
  out->effective_gap = gap;

  /* RAW fits: the canvas is mapped onto the panel uniformly by flip_dst below,
   * so the surface correction belongs there and nowhere else.  Correcting here
   * as well is what made the portrait stack shrink. */
  AleksLayout_FrameAspect(src_w, src_h, &num, &den);
  fit_raw(0, 0, ALEKS_FLIP_CANVAS_W * game_pct / 100,
          remaining * game_pct / 100, num, den, &out->game);

  slot.x = 0; slot.y = 0; slot.w = comp_w; slot.h = comp_h;
  fit_companion_raw(slot, &out->companion);

  total = out->game.h + gap + out->companion.h;
  stack_y = (ALEKS_FLIP_CANVAS_H - total) / 2;
  if (stack_y < 0) stack_y = 0;
  out->game.x = (ALEKS_FLIP_CANVAS_W - out->game.w) / 2;
  out->game.y = stack_y;
  out->companion.x = (ALEKS_FLIP_CANVAS_W - out->companion.w) / 2;
  out->companion.y = stack_y + out->game.h + gap;
}

void AleksLayout_Compute(AleksLayoutMode mode, int out_w, int out_h,
                         int src_w, int src_h, AleksLayout *out) {
  if (!out) return;
  if (src_w <= 0) src_w = 256;
  if (src_h <= 0) src_h = 224;

  out->mode = mode;
  out->game.x = out->game.y = out->game.w = out->game.h = 0;
  out->companion.x = out->companion.y = out->companion.w = out->companion.h = 0;
  out->logical_w = out_w;
  out->logical_h = out_h;
  out->rotation_degrees = 0;
  out->effective_gap = 0;
  out->flip_dst.x = out->flip_dst.y = out->flip_dst.w = out->flip_dst.h = 0;
  out->flip_scale_x = out->flip_scale_y = 1.0f;

  if (out_w <= 0 || out_h <= 0) return;
  switch (mode) {
  case ALEKS_LAYOUT_FLIP:   compute_flip(out_w, out_h, src_w, src_h, out); break;
  case ALEKS_LAYOUT_DUAL:   compute_dual(out_w, out_h, src_w, src_h, out); break;
  default:                  compute_normal(out_w, out_h, src_w, src_h, out); break;
  }
}

bool AleksLayout_MapTouch(const AleksLayout *layout, int out_w, int out_h,
                          int phys_x, int phys_y, int *local_x, int *local_y) {
  int lx, ly;

  if (!layout || !local_x || !local_y) return false;
  if (layout->companion.w <= 0 || layout->companion.h <= 0) return false;

  /* Undo the presentation rotation first, so the viewport test and the scaling
   * below are shared by every mode; the unrotated modes take the identity
   * branch.
   *
   * The compositor draws the canvas centred on the display and rotates it by
   * +270 degrees -- positive being CLOCKWISE -- about the destination centre.
   * The forward map of a canvas point (lx, ly) is
   *     sx = out_w/2 + (ly - logical_h/2) * f
   *     sy = out_h/2 - (lx - logical_w/2) * f
   * and this is that pair solved for lx, ly. */
  if (layout->rotation_degrees == 270) {
    /* The canvas is drawn into flip_dst and turned clockwise about the screen
     * centre, so the canvas X axis ends up along the screen's Y and vice
     * versa.  Each axis has its own scale (see compute_flip). */
    float fx = layout->flip_scale_x > 0.0f ? layout->flip_scale_x : 1.0f;
    float fy = layout->flip_scale_y > 0.0f ? layout->flip_scale_y : 1.0f;
    lx = (int)(layout->logical_w / 2.0f - (phys_y - out_h / 2.0f) / fx);
    ly = (int)(layout->logical_h / 2.0f + (phys_x - out_w / 2.0f) / fy);
  } else {
    lx = phys_x;
    ly = phys_y;
  }

  if (lx < layout->companion.x || lx >= layout->companion.x + layout->companion.w ||
      ly < layout->companion.y || ly >= layout->companion.y + layout->companion.h)
    return false;

  *local_x = (lx - layout->companion.x) * ALEKS_COMPANION_W / layout->companion.w;
  *local_y = (ly - layout->companion.y) * ALEKS_COMPANION_H / layout->companion.h;
  return true;
}
