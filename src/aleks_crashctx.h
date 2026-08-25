#ifndef ALEKS_CRASHCTX_H_
#define ALEKS_CRASHCTX_H_

/*
 * aleks_crashctx.h -- one display/runtime record, two consumers.
 *
 * Adapted from the final ALEKS TMC worktree (port/port_crashctx.{c,h} and
 * port/port_crashreport.c).  The mechanism is TMC's; only the contents are
 * Zelda3's, because what we need to know after a crash here is what the
 * DISPLAY was doing.
 *
 * PUBLISH / CAPTURE SPLIT -- the reason this file exists.  The record is
 * refreshed from normal execution while everything is healthy.  The crash
 * handler never walks the renderer, the layout or the engine: by then SDL may
 * be unusable and the heap may be torn.  It copies this already-assembled
 * record and nothing else.  That is why the record is a fixed-size POD with no
 * pointers -- it has to be readable when nothing else is.
 *
 * BREADCRUMBS are edge-driven, never per frame, so a report can say what the
 * last few transitions were rather than only the final frozen state.
 *
 * TWO STAGES.  Stage A is __libnx_exception_handler (libnx calls it; there is
 * nothing to install): memcpy the record, add the registers, write it raw with
 * open/write/fsync/close.  No malloc, no stdio, no SDL.  Stage B runs at the
 * next healthy boot and turns that raw record into a readable report.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ALEKS_CRASH_MAGIC   0x5A334358u /* 'Z3CX' */
#define ALEKS_CRASH_VERSION 1u
#define ALEKS_CRASH_CRUMBS  24
#define ALEKS_CRASH_TAG     24

typedef struct AleksCrashCrumb {
  uint32_t ms;
  char tag[ALEKS_CRASH_TAG];
  int32_t a;
  int32_t b;
} AleksCrashCrumb;

typedef struct AleksCrashContext {
  /* header, validated by stage B */
  uint32_t magic, version, size, crc;

  char build[32];
  /* Where this NRO was loaded, so a raw PC/LR can be turned into a source
   * line: addr2line offset = ADDR - module_base. */
  uint64_t module_base;
  uint32_t uptime_ms;
  uint32_t frame;
  uint8_t from_exception;

  /* display -- the whole point of this record */
  int32_t mode;              /* 0 NORMAL, 1 DUAL, 2 FLIP */
  int32_t aspect_wide;
  int32_t wide_camera;
  int32_t src_w, src_h;      /* engine frame handed to the renderer */
  int32_t tex_w, tex_h;      /* the SDL texture actually allocated */
  int32_t out_w, out_h;      /* RESOLVED physical output (AleksDisplay_...) */
  int32_t logical_w, logical_h;
  char output_source[24];    /* window / renderer / fallback -- how out_* was decided */
  int32_t game_dst[4];
  int32_t comp_dst[4];
  int32_t rotation;

  /* what was being asked for when it went wrong */
  char last_display_request[ALEKS_CRASH_TAG];
  char last_aspect_request[ALEKS_CRASH_TAG];
  char last_step[ALEKS_CRASH_TAG];

  /* runtime */
  uint8_t autosave_enabled;
  uint8_t save_state_op;     /* 0 idle, 1 save, 2 load */
  uint8_t companion_page;
  char sdl_error[96];

  AleksCrashCrumb crumbs[ALEKS_CRASH_CRUMBS];
  uint8_t crumb_head, crumb_wrapped;
} AleksCrashContext;

/* Boot: guarantee logs/, rotate the session log, convert any record a previous
 * crash left behind, and report an unclean previous shutdown.  Safe to call
 * before SDL exists. */
void AleksCrash_Init(const char *build_id);

/* Normal shutdown reached: clears the session-active flag.  If this is never
 * called, the next launch writes logs/unclean-previous.log. */
void AleksCrash_MarkCleanShutdown(void);

/*
 * CONTROLLED fatal exit -- the other half of the crash story.
 *
 * __libnx_exception_handler only runs for a real CPU exception.  A deliberate
 * Die()/exit(1) is an ordinary process exit, so it produced no report at all.
 * Anything that decides it cannot continue goes through here instead: it
 * writes the same readable report from the already-published context, marks
 * the termination type, and only then exits.
 *
 * Does no rendering, no engine traversal, no allocation beyond a small fixed
 * buffer.  Reentrancy-guarded.
 */
void AleksFatalf(const char *reason_fmt, ...);

/*
 * The tiny always-current transition journal: logs/display-last.log, rewritten
 * and fsynced at every step of a dangerous transition.  Transitions are rare
 * and the file is a few hundred bytes, so durability beats performance here.
 * If the process dies halfway, this file names the last completed step.
 */
void AleksDisplayJournal(const char *what, const char *step);

/* Development validation: exercise the report writer without terminating, so
 * the path/format is proven on the real filesystem rather than assumed.
 * Writes logs/crash-selftest.log.  Not reachable from any user input. */
bool AleksCrash_WriteTestReport(void);

/* Called from the present path while the renderer is healthy. */
void AleksCrash_PublishDisplay(int mode, int src_w, int src_h,
                               int out_w, int out_h,
                               int gx, int gy, int gw, int gh,
                               int cx, int cy, int cw, int ch,
                               int rotation);

/* The frontend's own numbers, which the layout does not see. */
void AleksCrash_PublishTexture(int tex_w, int tex_h);

/* The resolved physical output and how it was decided. */
void AleksCrash_PublishOutput(int w, int h, const char *source);

/* First frames only: what the picture actually works out to on the panel. */
void AleksCrash_LogGeometry(int mode, int src_w, int src_h,
                            int surf_w, int surf_h, int disp_w, int disp_h,
                            int gx, int gy, int gw, int gh,
                            int cw, int ch, int rotation,
                            float kx, float ky);

/* One line, first occurrence only, when the renderer and the window disagree
 * about the size of the screen. */
void AleksCrash_LogOutputMismatch(int ren_w, int ren_h, int win_w, int win_h,
                                  int used_w, int used_h, const char *reason);
void AleksCrash_PublishRuntime(int aspect_wide, int wide_camera,
                               int autosave, int save_state_op, int page);

/* Edge-driven only.  Never call these per frame. */
/* main.c's startup/runtime log (startup.log).  Declared here because it is
 * the log every module already writes its one-line events to. */
void StartupLog(const char *fmt, ...);

void AleksCrash_Breadcrumb(const char *tag, int a, int b);
void AleksCrash_Step(const char *step);
void AleksCrash_DisplayRequest(const char *from, const char *to);
void AleksCrash_AspectRequest(const char *from, const char *to);

/* First occurrence only, so a repeating SDL failure cannot spam the log. */
void AleksCrash_LogSdlOnce(const char *operation);

/* The last published record.  Never NULL, no pointers inside. */
const AleksCrashContext *AleksCrash_Get(void);
void AleksCrash_Seal(AleksCrashContext *ctx, uint8_t from_exception);

#ifdef __cplusplus
}
#endif

#endif /* ALEKS_CRASHCTX_H_ */
