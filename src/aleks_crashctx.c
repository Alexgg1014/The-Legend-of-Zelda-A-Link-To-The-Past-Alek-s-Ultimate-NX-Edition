/*
 * aleks_crashctx.c -- display/runtime crash context, log rotation, and the
 * two-stage crash reporter.
 *
 * Ported from the final ALEKS TMC worktree: port/port_crashctx.c (publish /
 * capture split, sealed CRC record, edge-driven breadcrumbs) and
 * port/port_crashreport.c (stage A weak __libnx_exception_handler with its own
 * exception stack, raw open/write/fsync, stage B conversion at next boot).
 *
 * WHAT IS TMC'S: the whole mechanism, including the reasoning that stage A
 * must not touch stdio, malloc, SDL or any engine structure, and that the
 * record must be fully assembled in static memory before any I/O is attempted
 * so a failed write loses the file and not the diagnosis.
 *
 * WHAT IS OURS: the contents.  TMC records entity state; this records the
 * display pipeline, because that is what has been failing here.
 *
 * HONEST LIMITATION, inherited and still true: open/write on the sdmc devoptab
 * is not async-signal-safe -- it takes fsdev locks.  A crash inside fsdev
 * holding one can make stage A fail.  It is best effort, and deliberately last.
 */

#include "aleks_crashctx.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/stat.h>

#ifdef __SWITCH__
#include <switch.h>
#include <fcntl.h>
#include <unistd.h>
#include <SDL.h>
#else
#include <SDL.h>
#endif

/*
 * ABSOLUTE paths on purpose.  This runs before main() chdir's into the runtime
 * root, and the exception handler must not depend on the working directory of
 * a process that has already failed.  (TMC pins its crash path the same way.)
 */
#ifdef __SWITCH__
#define RUNTIME_ROOT       "sdmc:/switch/Zelda3"
#define CRASH_PENDING_PATH RUNTIME_ROOT "/logs/crash_pending.bin"
#define CRASH_REPORT_PATH  RUNTIME_ROOT "/logs/crash-latest.log"
#define SESSION_LOG        RUNTIME_ROOT "/startup.log"
#define PREVIOUS_LOG       RUNTIME_ROOT "/logs/previous-session.log"
#define DISPLAY_JOURNAL    RUNTIME_ROOT "/logs/display-last.log"
#define SESSION_FLAG       RUNTIME_ROOT "/logs/session-active.flag"
#define UNCLEAN_REPORT     RUNTIME_ROOT "/logs/unclean-previous.log"
#define SELFTEST_REPORT    RUNTIME_ROOT "/logs/crash-selftest.log"
#define LAST_STATE         RUNTIME_ROOT "/logs/last-state.txt"
#else
#define CRASH_PENDING_PATH "logs/crash_pending.bin"
#define CRASH_REPORT_PATH  "logs/crash-latest.log"
#define SESSION_LOG        "startup.log"
#define PREVIOUS_LOG       "logs/previous-session.log"
#define DISPLAY_JOURNAL    "logs/display-last.log"
#define SESSION_FLAG       "logs/session-active.flag"
#define UNCLEAN_REPORT     "logs/unclean-previous.log"
#define SELFTEST_REPORT    "logs/crash-selftest.log"
#define LAST_STATE         "logs/last-state.txt"
#endif

static const char *ModeName(int m);

static AleksCrashContext sCtx;
static bool sSdlLogged[16];
static const char *sSdlOp[16];
static int sSdlOpCount;

/* ------------------------------------------------------------------ */
/*  publish                                                            */
/* ------------------------------------------------------------------ */

static void copy_tag(char *dst, size_t cap, const char *src) {
  size_t i = 0;
  if (!src) { dst[0] = 0; return; }
  for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
  dst[i] = 0;
}

void AleksCrash_PublishDisplay(int mode, int src_w, int src_h,
                               int out_w, int out_h,
                               int gx, int gy, int gw, int gh,
                               int cx, int cy, int cw, int ch,
                               int rotation) {
  sCtx.mode = mode;
  sCtx.src_w = src_w; sCtx.src_h = src_h;
  /* out_w/out_h belong to AleksCrash_PublishOutput: the resolver decides
   * them, and a second writer here would let an unresolved value back in. */
  (void)out_w; (void)out_h;
  sCtx.game_dst[0] = gx; sCtx.game_dst[1] = gy;
  sCtx.game_dst[2] = gw; sCtx.game_dst[3] = gh;
  sCtx.comp_dst[0] = cx; sCtx.comp_dst[1] = cy;
  sCtx.comp_dst[2] = cw; sCtx.comp_dst[3] = ch;
  sCtx.rotation = rotation;
  sCtx.frame++;
  sCtx.uptime_ms = SDL_GetTicks();
}

void AleksCrash_PublishOutput(int w, int h, const char *source) {
  sCtx.out_w = w;
  sCtx.out_h = h;
  /* There is no SDL logical size in this build; keeping these equal to the
   * resolved output says so plainly rather than leaving a stale field. */
  sCtx.logical_w = w;
  sCtx.logical_h = h;
  copy_tag(sCtx.output_source, sizeof(sCtx.output_source), source);
}

void AleksCrash_LogOutputMismatch(int ren_w, int ren_h, int win_w, int win_h,
                                  int used_w, int used_h, const char *reason) {
  FILE *f = fopen(SESSION_LOG, "ab");
  if (!f) return;
  fprintf(f,
          "OUTPUT MISMATCH:\n"
          "  renderer=%dx%d\n  window=%dx%d\n  using=%dx%d\n  reason=%s\n",
          ren_w, ren_h, win_w, win_h, used_w, used_h,
          reason ? reason : "(unspecified)");
  fflush(f);
  fclose(f);
}

void AleksCrash_LogGeometry(int mode, int src_w, int src_h,
                            int surf_w, int surf_h, int disp_w, int disp_h,
                            int gx, int gy, int gw, int gh,
                            int cw, int ch, int rotation,
                            float kx, float ky) {
  FILE *f = fopen(SESSION_LOG, "ab");
  double sx, sy;
  const char *space;
  if (!f) return;
  if (rotation == 270) {
    /* FLIP: canvas coordinates, scaled by the canvas->panel mapping.  The
     * player has the console turned, so these are already in their frame. */
    sx = kx; sy = ky;
    space = "canvas";
  } else {
    sx = surf_w > 0 ? (double)disp_w / surf_w : 1.0;
    sy = surf_h > 0 ? (double)disp_h / surf_h : 1.0;
    space = "surface";
  }
  fprintf(f,
          "GEOMETRY: mode=%d rot=%d src=%dx%d surface=%dx%d display=%dx%d\n"
          "  game %s=%d,%d %dx%d -> seen %.0fx%.0f aspect %.3f\n"
          "  comp %s=%dx%d -> seen %.0fx%.0f aspect %.3f\n",
          mode, rotation, src_w, src_h, surf_w, surf_h, disp_w, disp_h,
          space, gx, gy, gw, gh, gw * sx, gh * sy,
          gh > 0 ? (gw * sx) / (gh * sy) : 0.0,
          space, cw, ch, cw * sx, ch * sy,
          ch > 0 ? (cw * sx) / (ch * sy) : 0.0);
  fflush(f);
  fclose(f);
}

void AleksCrash_PublishTexture(int tex_w, int tex_h) {
  sCtx.tex_w = tex_w;
  sCtx.tex_h = tex_h;
}

void AleksCrash_PublishRuntime(int aspect_wide, int wide_camera,
                               int autosave, int save_state_op, int page) {
  sCtx.aspect_wide = aspect_wide;
  sCtx.wide_camera = wide_camera;
  sCtx.autosave_enabled = (uint8_t)autosave;
  sCtx.save_state_op = (uint8_t)save_state_op;
  sCtx.companion_page = (uint8_t)page;
}

void AleksCrash_Breadcrumb(const char *tag, int a, int b) {
  AleksCrashCrumb *c = &sCtx.crumbs[sCtx.crumb_head];
  c->ms = SDL_GetTicks();
  copy_tag(c->tag, sizeof(c->tag), tag);
  c->a = a;
  c->b = b;
  sCtx.crumb_head = (uint8_t)((sCtx.crumb_head + 1) % ALEKS_CRASH_CRUMBS);
  if (sCtx.crumb_head == 0) sCtx.crumb_wrapped = 1;
}

void AleksCrash_Step(const char *step) {
  copy_tag(sCtx.last_step, sizeof(sCtx.last_step), step);
  AleksCrash_Breadcrumb(step, 0, 0);
}

void AleksCrash_DisplayRequest(const char *from, const char *to) {
  char buf[ALEKS_CRASH_TAG];
  snprintf(buf, sizeof(buf), "%s>%s", from ? from : "?", to ? to : "?");
  copy_tag(sCtx.last_display_request, sizeof(sCtx.last_display_request), buf);
  AleksCrash_Breadcrumb("DISPLAY REQ", 0, 0);
}

void AleksCrash_AspectRequest(const char *from, const char *to) {
  char buf[ALEKS_CRASH_TAG];
  snprintf(buf, sizeof(buf), "%s>%s", from ? from : "?", to ? to : "?");
  copy_tag(sCtx.last_aspect_request, sizeof(sCtx.last_aspect_request), buf);
  AleksCrash_Breadcrumb("ASPECT REQ", 0, 0);
}

/* First occurrence per operation.  A display failure that repeats every frame
 * would otherwise bury the one line that mattered. */
void AleksCrash_LogSdlOnce(const char *operation) {
  int slot = -1;
  for (int i = 0; i < sSdlOpCount; i++)
    if (sSdlOp[i] == operation) { slot = i; break; }
  if (slot < 0) {
    if (sSdlOpCount >= (int)(sizeof(sSdlOp) / sizeof(sSdlOp[0]))) return;
    slot = sSdlOpCount++;
    sSdlOp[slot] = operation;
  }
  if (sSdlLogged[slot]) return;
  sSdlLogged[slot] = true;
  copy_tag(sCtx.sdl_error, sizeof(sCtx.sdl_error), SDL_GetError());
  {
    FILE *f = fopen(SESSION_LOG, "ab");
    if (f) {
      fprintf(f, "SDL FAILURE (first occurrence): %s: %s\n",
              operation, SDL_GetError());
      fclose(f);
    }
  }
}

/* Write a small file and make it survive an uncontrolled termination.
 * fflush alone is not enough on fsdev: the data has to reach the card. */
static void WriteDurable(const char *path, const char *text) {
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fputs(text, f);
  fflush(f);
#ifdef __SWITCH__
  {
    int fd = fileno(f);
    if (fd >= 0) fsync(fd);
  }
#endif
  fclose(f);
}

/*
 * The transition journal.  Rewritten in full at every step so the file always
 * holds exactly one snapshot -- the last one reached.  Cheap because
 * transitions are rare; this is the file that survives when nothing else does.
 */
void AleksDisplayJournal(const char *what, const char *step) {
  char buf[1024];
  char what_s[32], step_s[224];
  const AleksCrashContext *c = &sCtx;
  copy_tag(what_s, sizeof(what_s), what ? what : "?");
  copy_tag(step_s, sizeof(step_s), step ? step : "?");
  snprintf(buf, sizeof(buf),
           "%s TRANSITION\n"
           "step=%s\n"
           "uptime_ms=%u\nframe=%u\n"
           "mode=%s\naspect=%s\nwide_camera=%s\n"
           "source=%dx%d\ntexture=%dx%d\noutput=%dx%d (%s)\n"
           "game_dst=%d,%d %dx%d\ncomp_dst=%d,%d %dx%d\nrotation=%d\n"
           "last_display_request=%s\nlast_aspect_request=%s\n",
           what_s, step_s,
           c->uptime_ms, c->frame,
           ModeName(c->mode), c->aspect_wide ? "Wide" : "4:3",
           c->wide_camera ? "Fixed" : "Standard",
           c->src_w, c->src_h, c->tex_w, c->tex_h, c->out_w, c->out_h,
           c->output_source[0] ? c->output_source : "unknown",
           c->game_dst[0], c->game_dst[1], c->game_dst[2], c->game_dst[3],
           c->comp_dst[0], c->comp_dst[1], c->comp_dst[2], c->comp_dst[3],
           c->rotation,
           c->last_display_request[0] ? c->last_display_request : "(none)",
           c->last_aspect_request[0] ? c->last_aspect_request : "(none)");
  WriteDurable(DISPLAY_JOURNAL, buf);
  WriteDurable(LAST_STATE, buf);
}

const AleksCrashContext *AleksCrash_Get(void) { return &sCtx; }

static uint32_t Crc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
  }
  return ~crc;
}

void AleksCrash_Seal(AleksCrashContext *ctx, uint8_t from_exception) {
  ctx->magic = ALEKS_CRASH_MAGIC;
  ctx->version = ALEKS_CRASH_VERSION;
  ctx->size = (uint32_t)sizeof(*ctx);
  ctx->from_exception = from_exception;
  ctx->crc = 0;
  ctx->crc = Crc32((const uint8_t *)ctx + sizeof(uint32_t) * 4,
                   sizeof(*ctx) - sizeof(uint32_t) * 4);
}

/* ------------------------------------------------------------------ */
/*  the readable formatter -- shared by stage B and any manual dump     */
/* ------------------------------------------------------------------ */

static const char *ModeName(int m) {  /* fwd-declared above */
  return m == 1 ? "Dual" : m == 2 ? "Flip" : "Normal";
}

static void WriteReadable(FILE *f, const AleksCrashContext *c,
                          const char *regs_text) {
  fprintf(f, "ALEKS ZELDA3 NX CRASH REPORT\n");
  fprintf(f, "============================\n\n");
  fprintf(f, "BUILD: %s\n", c->build[0] ? c->build : "(unknown)");
  /* Raw PC/LR mean nothing without the load bias.  Reporting the runtime
   * address of a known symbol is exact and needs no syscall: subtract the same
   * symbol's value from the matching .elf and you have the bias. */
  if (c->module_base) {
    fprintf(f,
      "LOAD BIAS REFERENCE: AleksCrash_Init @ 0x%016llx\n"
      "  bias = that MINUS the nm value of AleksCrash_Init in the matching .elf\n"
      "  then addr2line -e <elf> -f -C $((ADDR - bias))\n",
      (unsigned long long)c->module_base);
  }
  fprintf(f, "FROM EXCEPTION: %s\n", c->from_exception ? "yes" : "no (manual)");
  fprintf(f, "UPTIME MS: %u\n", c->uptime_ms);
  fprintf(f, "FRAME: %u\n", c->frame);
  fprintf(f, "LAST SESSION LOG: %s\n\n", PREVIOUS_LOG);

  fprintf(f, "DISPLAY MODE: %s\n", ModeName(c->mode));
  fprintf(f, "ASPECT: %s\n", c->aspect_wide ? "Wide" : "4:3");
  fprintf(f, "WIDE CAMERA: %s\n", c->wide_camera ? "Fixed" : "Standard");
  fprintf(f, "ENGINE SOURCE: %dx%d\n", c->src_w, c->src_h);
  fprintf(f, "TEXTURE: %dx%d\n", c->tex_w, c->tex_h);
  fprintf(f, "RENDER OUTPUT: %dx%d  (resolved via %s)\n", c->out_w, c->out_h,
          c->output_source[0] ? c->output_source : "unknown");
  fprintf(f, "SDL LOGICAL SIZE: %dx%d (logical size is unused in this build)\n",
          c->logical_w, c->logical_h);
  fprintf(f, "GAME DST RECT: %d,%d %dx%d\n",
          c->game_dst[0], c->game_dst[1], c->game_dst[2], c->game_dst[3]);
  fprintf(f, "COMP DST RECT: %d,%d %dx%d\n",
          c->comp_dst[0], c->comp_dst[1], c->comp_dst[2], c->comp_dst[3]);
  fprintf(f, "FLIP ROTATION: %d\n\n", c->rotation);

  fprintf(f, "LAST DISPLAY REQUEST: %s\n",
          c->last_display_request[0] ? c->last_display_request : "(none)");
  fprintf(f, "LAST ASPECT REQUEST: %s\n",
          c->last_aspect_request[0] ? c->last_aspect_request : "(none)");
  fprintf(f, "LAST DISPLAY STEP: %s\n\n",
          c->last_step[0] ? c->last_step : "(none)");

  fprintf(f, "AUTOSAVE: %s\n", c->autosave_enabled ? "on" : "off");
  fprintf(f, "SAVE-STATE OP: %s\n",
          c->save_state_op == 1 ? "save" : c->save_state_op == 2 ? "load" : "idle");
  fprintf(f, "COMPANION PAGE: %u\n", c->companion_page);
  fprintf(f, "SDL ERROR: %s\n\n", c->sdl_error[0] ? c->sdl_error : "(none)");

  fprintf(f, "BREADCRUMBS (oldest first)\n");
  {
    int n = c->crumb_wrapped ? ALEKS_CRASH_CRUMBS : c->crumb_head;
    int start = c->crumb_wrapped ? c->crumb_head : 0;
    for (int i = 0; i < n; i++) {
      const AleksCrashCrumb *b = &c->crumbs[(start + i) % ALEKS_CRASH_CRUMBS];
      if (!b->tag[0]) continue;
      fprintf(f, "  [%8u ms] %-22s a=%d b=%d\n", b->ms, b->tag, b->a, b->b);
    }
  }
  if (regs_text && regs_text[0])
    fprintf(f, "\n%s", regs_text);
}

/* ------------------------------------------------------------------ */
/*  stage A -- the exception handler                                   */
/* ------------------------------------------------------------------ */

typedef struct AleksCrashRecord {
  AleksCrashContext ctx;
#ifdef __SWITCH__
  uint32_t error_desc, pstate, esr, have_regs;
  uint64_t gprs[29];
  uint64_t fp, lr, sp, pc, far;
#endif
} AleksCrashRecord;

static AleksCrashRecord sRecord;

#ifdef __SWITCH__

/* Our own exception stack: the default is small and shared, so a
 * stack-overflow crash would have nowhere to run.  Overrides libnx's weak
 * definitions (TMC does the same, for the same reason). */
alignas(16) u8 __nx_exception_stack[0x2000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

/* Left at 0 on purpose: with a debugger attached the debugger should get the
 * exception, not us. */
u32 __nx_exception_ignoredebug = 0;

/* Reentrancy guard: a fault inside the handler must not re-enter it. */
static volatile int sHandlerActive;

/* Raw, bounded, best effort.  No stdio. */
static void CrashWriteRaw(const void *data, size_t len) {
  int fd = open(CRASH_PENDING_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0) return;
  {
    const uint8_t *p = (const uint8_t *)data;
    size_t left = len;
    while (left > 0) {
      ssize_t n = write(fd, p, left);
      if (n <= 0) break;
      p += (size_t)n;
      left -= (size_t)n;
    }
  }
  (void)fsync(fd);
  (void)close(fd);
}

void __libnx_exception_handler(ThreadExceptionDump *ctx) {
  if (sHandlerActive) return;      /* never recurse into crash handling */
  sHandlerActive = 1;

  /* Copy first, touch the filesystem last, walk nothing. */
  memcpy(&sRecord.ctx, &sCtx, sizeof(sRecord.ctx));
  AleksCrash_Seal(&sRecord.ctx, 1);

  sRecord.have_regs = 0;
  if (ctx != NULL) {
    sRecord.error_desc = ctx->error_desc;
    sRecord.pstate = ctx->pstate;
    sRecord.esr = ctx->esr;
    for (int i = 0; i < 29; i++) sRecord.gprs[i] = ctx->cpu_gprs[i].x;
    sRecord.fp = ctx->fp.x;
    sRecord.lr = ctx->lr.x;
    sRecord.sp = ctx->sp.x;
    sRecord.pc = ctx->pc.x;
    sRecord.far = ctx->far.x;
    sRecord.have_regs = 1;
  }

  CrashWriteRaw(&sRecord, sizeof(sRecord));
  /* Return; __libnx_exception_returnentry then calls svcBreak, which still
   * hands the crash to Atmosphere/creport.  This supplements the system
   * report rather than swallowing it. */
}

static const char *ErrorDescName(uint32_t d) {
  switch (d) {
  case 0x100: return "InstructionAbort";
  case 0x101: return "Other/DataAbort";
  case 0x102: return "MisalignedPC";
  case 0x103: return "MisalignedSP";
  case 0x104: return "Trap";
  case 0x106: return "SError";
  case 0x301: return "BadSVC";
  default:    return "Unknown";
  }
}
#endif /* __SWITCH__ */

/* ------------------------------------------------------------------ */
/*  stage B + log rotation -- at the next healthy boot                 */
/* ------------------------------------------------------------------ */

static void RotateSessionLog(void) {
  FILE *in = fopen(SESSION_LOG, "rb");
  FILE *out;
  char buf[4096];
  size_t n;
  if (!in) return;
  out = fopen(PREVIOUS_LOG, "wb");
  if (!out) {
    /* Never destroy the old evidence because the rotation target is
     * unavailable -- take a different name instead. */
    out = fopen(RUNTIME_ROOT "/previous-session.log", "wb");
  }
  if (!out) { fclose(in); return; }
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
    fwrite(buf, 1, n, out);
  fclose(out);
  fclose(in);
}

static void ConvertPending(void) {
#ifdef __SWITCH__
  static AleksCrashRecord rec;
  FILE *in = fopen(CRASH_PENDING_PATH, "rb");
  size_t got;
  uint32_t crc;
  if (!in) return;
  got = fread(&rec, 1, sizeof(rec), in);
  fclose(in);
  remove(CRASH_PENDING_PATH);
  if (got != sizeof(rec)) return;
  if (rec.ctx.magic != ALEKS_CRASH_MAGIC || rec.ctx.version != ALEKS_CRASH_VERSION)
    return;
  /* A damaged record is rejected here rather than formatted into a
   * plausible-looking but wrong report. */
  crc = rec.ctx.crc;
  rec.ctx.crc = 0;
  if (Crc32((const uint8_t *)&rec.ctx + sizeof(uint32_t) * 4,
            sizeof(rec.ctx) - sizeof(uint32_t) * 4) != crc)
    return;
  rec.ctx.crc = crc;

  {
    FILE *f = fopen(CRASH_REPORT_PATH, "wb");
    char regs[1400];
    size_t at = 0;
    if (!f) return;
    if (rec.have_regs) {
      at += snprintf(regs + at, sizeof(regs) - at,
                     "EXCEPTION\n  type = 0x%03x (%s)\n  esr  = 0x%08x\n"
                     "  pstate = 0x%08x\n  far  = 0x%016llx\n",
                     rec.error_desc, ErrorDescName(rec.error_desc),
                     rec.esr, rec.pstate, (unsigned long long)rec.far);
      at += snprintf(regs + at, sizeof(regs) - at,
                     "  PC   = 0x%016llx\n  LR   = 0x%016llx\n"
                     "  SP   = 0x%016llx\n  FP   = 0x%016llx\n",
                     (unsigned long long)rec.pc, (unsigned long long)rec.lr,
                     (unsigned long long)rec.sp, (unsigned long long)rec.fp);
      at += snprintf(regs + at, sizeof(regs) - at,
                     "  (addresses are raw; resolve with addr2line against the"
                     " matching .elf)\nREGISTERS\n");
      for (int i = 0; i < 29 && at < sizeof(regs) - 32; i++)
        at += snprintf(regs + at, sizeof(regs) - at, "  x%-2d = 0x%016llx\n",
                       i, (unsigned long long)rec.gprs[i]);
    } else {
      snprintf(regs, sizeof(regs), "EXCEPTION: no register context captured\n");
    }
    WriteReadable(f, &rec.ctx, regs);
    fclose(f);
  }
#endif
}

/*
 * CONTROLLED fatal.  This is the path an ordinary Die()/exit(1) used to take
 * silently.  Same record, same formatter, different termination type.
 */
static volatile int sFatalActive;

void AleksFatalf(const char *reason_fmt, ...) {
  char reason[192];
  va_list ap;

  if (sFatalActive) _exit(1);   /* never recurse into fatal handling */
  sFatalActive = 1;

  va_start(ap, reason_fmt);
  vsnprintf(reason, sizeof(reason), reason_fmt, ap);
  va_end(ap);

  copy_tag(sCtx.sdl_error, sizeof(sCtx.sdl_error), SDL_GetError());
  AleksCrash_Seal(&sCtx, 0);
  AleksDisplayJournal("FATAL", reason);

  {
    FILE *f = fopen(CRASH_REPORT_PATH, "wb");
    if (f) {
      char why[320];
      snprintf(why, sizeof(why),
               "TERMINATION: APPLICATION_FATAL\nREASON: %s\n", reason);
      WriteReadable(f, &sCtx, why);
      fflush(f);
#ifdef __SWITCH__
      { int fd = fileno(f); if (fd >= 0) fsync(fd); }
#endif
      fclose(f);
    }
  }
  {
    FILE *f = fopen(SESSION_LOG, "ab");
    if (f) { fprintf(f, "FATAL: %s\n", reason); fflush(f); fclose(f); }
  }
  remove(SESSION_FLAG);   /* this end was reported, so not "unclean" */
  _exit(1);
}

/* Prove the writer works on this filesystem without crashing anything. */
bool AleksCrash_WriteTestReport(void) {
  FILE *f = fopen(SELFTEST_REPORT, "wb");
  if (!f) return false;
  AleksCrash_Seal(&sCtx, 0);
  WriteReadable(f, &sCtx,
                "TERMINATION: SELF TEST (no failure occurred)\n"
                "This file existing proves the report path and format work.\n");
  fflush(f);
#ifdef __SWITCH__
  { int fd = fileno(f); if (fd >= 0) fsync(fd); }
#endif
  fclose(f);
  return true;
}

/* Neither crash path ran?  The flag left behind still says so. */
static void ReportUncleanPrevious(void) {
  FILE *flag = fopen(SESSION_FLAG, "rb");
  char previous[640];
  size_t n;
  if (!flag) return;
  n = fread(previous, 1, sizeof(previous) - 1, flag);
  previous[n] = 0;
  fclose(flag);

  {
    FILE *f = fopen(UNCLEAN_REPORT, "wb");
    if (f) {
      fprintf(f,
        "PREVIOUS SESSION ENDED UNCLEANLY\n"
        "================================\n\n"
        "The previous launch never reached its normal shutdown, and neither\n"
        "the exception handler nor the controlled fatal path reported it.\n"
        "What follows is the last state that reached the card.\n\n"
        "%s\n", previous);
      /* The journal is the freshest evidence; copy it in verbatim. */
      {
        FILE *j = fopen(DISPLAY_JOURNAL, "rb");
        if (j) {
          char buf[512];
          size_t got;
          fprintf(f, "LAST DISPLAY JOURNAL\n--------------------\n");
          while ((got = fread(buf, 1, sizeof(buf), j)) > 0)
            fwrite(buf, 1, got, f);
          fclose(j);
        } else {
          fprintf(f, "LAST DISPLAY JOURNAL: (none written)\n");
        }
      }
      fflush(f);
      fclose(f);
    }
  }
}

static void MarkSessionActive(const char *build_id) {
  char buf[320];
  snprintf(buf, sizeof(buf),
           "session build=%s\nstarted=running\n", build_id ? build_id : "?");
  WriteDurable(SESSION_FLAG, buf);
}

void AleksCrash_MarkCleanShutdown(void) {
  remove(SESSION_FLAG);
}

void AleksCrash_Init(const char *build_id) {
  memset(&sCtx, 0, sizeof(sCtx));
  copy_tag(sCtx.build, sizeof(sCtx.build), build_id);
  /* Raw PC/LR are useless without the load bias.  Rather than guess at a
   * region base, record the RUNTIME address of a known function: the bias is
   * that minus the same symbol's address in the .elf, which nm reports.  This
   * cannot be wrong, and needs no syscall. */
  sCtx.module_base = (uint64_t)(uintptr_t)&AleksCrash_Init;
#ifdef __SWITCH__
  /* Earliest code in the process: nothing else has made these yet, and the
   * rotation below needs logs/ to exist. */
  mkdir(RUNTIME_ROOT, 0755);
  mkdir(RUNTIME_ROOT "/logs", 0755);
#endif
  /* Order matters: rotate the previous session's log before this session
   * starts overwriting it, then convert any crash record that log belongs to. */
  /* Order matters and each step is independent of the next:
   *   1. preserve the previous session's log BEFORE it is truncated
   *   2. turn any raw crash record into a readable report
   *   3. report an unclean end that neither crash path caught
   *   4. only then claim this session
   */
  RotateSessionLog();
  ConvertPending();
  ReportUncleanPrevious();
  MarkSessionActive(build_id);
#if defined(__SWITCH__) && defined(ALEKS_CRASH_SELFTEST)
  /* Development validation only: proves the report path works on the real
   * card.  A release build must not write a crash report on every boot, so
   * this is compile-gated OFF and the real crash writers are untouched.
   * Build with -DALEKS_CRASH_SELFTEST to bring it back. */
  AleksCrash_WriteTestReport();
#endif
}
