#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>
#include <SDL.h>
#ifdef __SWITCH__
#include <switch.h>
#include "platform/switch/switch_first_run.h"
#endif
#ifdef _WIN32
#include "platform/win32/volume_control.h"
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "snes/ppu.h"

#include "types.h"
#include "variables.h"

#include "zelda_rtl.h"
#include "zelda_cpu_infra.h"

#include "config.h"
#include "assets.h"
#include "asset_extractor.h"
#include "second_screen.h"
#include "aleks_compositor.h"
#include "aleks_crashctx.h"
#include "aleks_ra.h"
#include "aleks_lang.h"
#include "load_gfx.h"
#include "util.h"
#include "audio.h"

static bool g_run_without_emu = 0;

// Forwards
static bool LoadRom(const char *filename);
static void LoadLinkGraphics();
static void RenderNumber(uint8 *dst, size_t pitch, int n, bool big);
static void HandleInput(int keyCode, int modCode, bool pressed);
static void HandleCommand(uint32 j, bool pressed);
static int RemapSdlButton(int button);
static void HandleGamepadInput(int button, bool pressed);
static void HandleGamepadAxisInput(int gamepad_id, int axis, int value);
static void OpenOneGamepad(int i);
static void HandleVolumeAdjustment(int volume_adjustment);
static void LoadAssets();
static void SwitchDirectory();
static void StartupLog_Init();
void StartupLog(const char *fmt, ...);
static void StartupLogCurrentDirectory();
static void SetBootStage(const char *stage);

enum {
  kDefaultFullscreen = 0,
  kMaxWindowScale = 10,
  kDefaultFreq = 44100,
  kDefaultChannels = 2,
  kDefaultSamples = 2048,
};

static const char kWindowTitle[] = "The Legend of Zelda: A Link to the Past";
#ifdef __SWITCH__
static uint32 g_win_flags = 0;
#else
static uint32 g_win_flags = SDL_WINDOW_RESIZABLE;
#endif
static SDL_Window *g_window;

static uint8 g_paused, g_turbo, g_replay_turbo = true, g_cursor = true;
static uint8 g_current_window_scale;
static uint8 g_gamepad_buttons;
static int g_input1_state;
static bool g_display_perf;
static int g_curr_fps;
static int g_ppu_render_flags = 0;
static int g_snes_width, g_snes_height;
static int g_sdl_audio_mixer_volume = SDL_MIX_MAXVOLUME;
static struct RendererFuncs g_renderer_funcs;
static uint32 g_gamepad_modifiers;
static uint16 g_gamepad_last_cmd[kGamepadBtn_Count];
static const char kSwitchRuntimeRoot[] = "sdmc:/switch/Zelda3";
static const char kSwitchStartupLogPath[] = "sdmc:/switch/Zelda3/startup.log";
static FILE *g_startup_log;
static const char *g_boot_stage = "[BOOT 00] process startup";
static bool g_audio_ready;

static void StartupLog_Init() {
#ifdef __SWITCH__
  /* Rotate last session's log and convert any crash record it belongs to,
   * BEFORE this session truncates startup.log. */
  AleksCrash_Init("Zelda3-ALEKS-NX DISPLAY-ROOTFIX-V1 " __DATE__ " " __TIME__);
  g_startup_log = fopen(kSwitchStartupLogPath, "wb");
#endif
}

void StartupLog(const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  if (g_startup_log) {
    vfprintf(g_startup_log, fmt, va);
    fputc('\n', g_startup_log);
    fflush(g_startup_log);
  }
  va_end(va);
}

#ifdef __SWITCH__
static void AssetExtractionLog(const char *message) {
  StartupLog("%s", message);
}
#endif

/*
 * Clean first boot.
 *
 * The hardware bug this fixes: with no zelda3.ini on the card the game booted
 * with every upstream config field at zero -- no audio, old renderer, no
 * window size -- and nothing ever created the file.  A fresh install is now a
 * supported path: make the writable directories, then write a zelda3.ini from
 * the compiled defaults (config.c owns those; this only decides WHEN).
 *
 * Every step here is best-effort.  A directory we cannot create, or an ini we
 * cannot write, must not stop the game: Config_SetDefaults has already given
 * us a correct configuration in memory.
 */
static bool g_config_was_created;

static void EnsureRuntimeDirectory(const char *name) {
#if defined(_WIN32)
  int r = _mkdir(name);
#else
  int r = mkdir(name, 0755);
#endif
  if (r != 0 && errno != EEXIST)
    StartupLog("WARNING: could not create %s (errno %d)", name, errno);
}

static void EnsureRuntimeLayout(void) {
  /* saves/ is required; the rest are conveniences whose absence is harmless. */
  EnsureRuntimeDirectory("saves");
  EnsureRuntimeDirectory("logs");
  EnsureRuntimeDirectory("screenshots");
  EnsureRuntimeDirectory("languages");
  EnsureRuntimeDirectory("msu");
}

static bool ConfigFileExists(void) {
  FILE *f = fopen("zelda3.ini", "rb");
  if (!f) return false;
  fclose(f);
  return true;
}

static void StartupLogCurrentDirectory() {
  char cwd[4096];
  if (getcwd(cwd, sizeof(cwd)))
    StartupLog("cwd=%s", cwd);
  else
    StartupLog("cwd=<unavailable>");
}

static void SetBootStage(const char *stage) {
  g_boot_stage = stage;
  StartupLog("%s", stage);
}

/*
 * Die() was an ordinary exit(1): no CPU exception, so __libnx_exception_handler
 * never ran and no crash report was written.  That is why the first hardware
 * crash left nothing behind.  It now goes through AleksFatalf, which writes the
 * same report from the already-published context and marks the termination
 * APPLICATION_FATAL.
 */
void NORETURN Die(const char *error) {
#if defined(NDEBUG) && defined(_WIN32)
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kWindowTitle, error, NULL);
#endif
  fprintf(stderr, "Error: %s\n", error);
  StartupLog("FATAL MESSAGE: %s", error);
  StartupLog("SDL_GetError: %s", SDL_GetError());
  StartupLog("current boot stage: %s", g_boot_stage);
  StartupLog("runtime root: %s", kSwitchRuntimeRoot);
  StartupLog("config path: zelda3.ini");
  StartupLog("assets path: zelda3_assets.dat");
  StartupLog("save path: saves/sram.dat");
  StartupLogCurrentDirectory();
  AleksFatalf("Die: %s", error);
  exit(1);   /* not reached: AleksFatalf terminates */
}

void ChangeWindowScale(int scale_step) {
  if ((SDL_GetWindowFlags(g_window) & (SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED | SDL_WINDOW_MAXIMIZED)) != 0)
    return;
  int screen = SDL_GetWindowDisplayIndex(g_window);
  if (screen < 0) screen = 0;
  int max_scale = kMaxWindowScale;
  SDL_Rect bounds;
  int bt = -1, bl, bb, br;
  // note this takes into effect Windows display scaling, i.e., resolution is divided by scale factor
  if (SDL_GetDisplayUsableBounds(screen, &bounds) == 0) {
    // this call may take a while before it is reported by Windows (or not at all in my testing)
    if (SDL_GetWindowBordersSize(g_window, &bt, &bl, &bb, &br) != 0) {
      // guess based on Windows 10/11 defaults
      bl = br = bb = 1;
      bt = 31;
    }
    // Allow a scale level slightly above the max that fits on screen
    int mw = (bounds.w - bl - br + g_snes_width / 4) / g_snes_width;
    int mh = (bounds.h - bt - bb + g_snes_height / 4) / g_snes_height;
    max_scale = IntMin(mw, mh);
  }
  int new_scale = IntMax(IntMin(g_current_window_scale + scale_step, max_scale), 1);
  g_current_window_scale = new_scale;
  int w = new_scale * g_snes_width;
  int h = new_scale * g_snes_height;

  //SDL_RenderSetLogicalSize(g_renderer, w, h);
  SDL_SetWindowSize(g_window, w, h);
  if (bt >= 0) {
    // Center the window on top of the mouse
    int mx, my;
    SDL_GetGlobalMouseState(&mx, &my);
    int wx = IntMax(IntMin(mx - w / 2, bounds.x + bounds.w - bl - br - w), bounds.x + bl);
    int wy = IntMax(IntMin(my - h / 2, bounds.y + bounds.h - bt - bb - h), bounds.y + bt);
    SDL_SetWindowPosition(g_window, wx, wy);
  } else {
    SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
  }
}

#ifndef __SWITCH__
#define RESIZE_BORDER 20
static SDL_HitTestResult HitTestCallback(SDL_Window *win, const SDL_Point *pt, void *data) {
  uint32 flags = SDL_GetWindowFlags(win);
  if ((flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0 || (flags & SDL_WINDOW_FULLSCREEN) != 0)
    return SDL_HITTEST_NORMAL;

  if ((SDL_GetModState() & KMOD_CTRL) != 0)
    return SDL_HITTEST_DRAGGABLE;

  int w, h;
  SDL_GetWindowSize(win, &w, &h);

  if (pt->y < RESIZE_BORDER) {
    return (pt->x < RESIZE_BORDER) ? SDL_HITTEST_RESIZE_TOPLEFT :
           (pt->x >= w - RESIZE_BORDER) ? SDL_HITTEST_RESIZE_TOPRIGHT : SDL_HITTEST_RESIZE_TOP;
  } else if (pt->y >= h - RESIZE_BORDER) {
    return (pt->x < RESIZE_BORDER) ? SDL_HITTEST_RESIZE_BOTTOMLEFT :
           (pt->x >= w - RESIZE_BORDER) ? SDL_HITTEST_RESIZE_BOTTOMRIGHT : SDL_HITTEST_RESIZE_BOTTOM;
  } else {
    if (pt->x < RESIZE_BORDER) {
      return SDL_HITTEST_RESIZE_LEFT;
    } else if (pt->x >= w - RESIZE_BORDER) {
      return SDL_HITTEST_RESIZE_RIGHT;
    }
  }
  return SDL_HITTEST_NORMAL;
}
#endif

/* Gameplay frame geometry follows the engine's aspect state.  Called at boot
 * and again whenever ZeldaSetWidescreen changes it at runtime. */
/* Forward: the SDL renderer objects are defined with the rest of the SDL
 * backend below; the aspect reconfiguration above needs them. */
static SDL_Renderer *g_renderer;
static SDL_Texture *g_texture;
/* What the game texture actually is, so nothing has to infer it. */
static int g_texture_w, g_texture_h;

static void ApplyGameplayGeometry(void) {
  g_snes_width = (g_config.extended_aspect_ratio * 2 + 256);
  g_snes_height = (g_config.extend_y ? 240 : 224);
  /* The render flag follows the same setting, here rather than only at boot,
   * because the aspect is now switchable at runtime and TRUE 16:9 EXPANDED is
   * the mode that asks for the engine's 240-line frame.  ZeldaDrawPpuFrame
   * reads this flag to decide how many scanlines to run. */
  if (g_config.extend_y)
    g_ppu_render_flags |= kPpuRenderFlags_Height240;
  else
    g_ppu_render_flags &= ~kPpuRenderFlags_Height240;
}

/*
 * Live aspect change, as a transaction.
 *
 * The old version changed g_snes_width first and then tried to resize the
 * texture; if SDL_CreateTexture failed it simply returned, leaving the engine
 * asking for a 398-wide frame from a 256-wide texture.  The next
 * SDL_LockTexture then failed and Die() killed the game -- that is the crash
 * seen when switching aspect.
 *
 * Now: the replacement is created and validated BEFORE anything is committed,
 * and if it cannot be created the engine's aspect is put back so the old
 * texture stays correct.  Never destroy-then-create.
 */
static bool SdlRenderer_ApplyAspect(void) {
  int tex_mult = (g_ppu_render_flags & kPpuRenderFlags_4x4Mode7) ? 4 : 1;
  int want_w, want_h;
  SDL_Texture *replacement;

  AleksCrash_Step("STEP 01 SAFEPOINT");
  AleksDisplayJournal("ASPECT", "STEP 01 SAFEPOINT");
  ApplyGameplayGeometry();
  /* Sized for the widest/tallest frame the PPU can ever produce, so an aspect
   * change never needs a new texture -- only a different source rect. */
  want_w = kPpuXPixels * tex_mult;
  want_h = 240 * tex_mult;

  if (!g_renderer) return false;
  if (want_w == g_texture_w && want_h == g_texture_h) {
    /* The normal case now: the texture is allocated for the widest frame the
     * PPU can produce, so changing aspect only changes the source rect. */
    StartupLog("TEXTURE RESIZE: NOT REQUIRED (%dx%d)", want_w, want_h);
    StartupLog("ASPECT APPLIED: source %dx%d texture %dx%d",
               g_snes_width, g_snes_height, g_texture_w, g_texture_h);
    AleksCrash_Step("STEP 08 APPLY COMPLETE");
    AleksDisplayJournal("ASPECT", "STEP 08 APPLY COMPLETE");
    return true;
  }

  AleksCrash_Step("STEP 02 CREATE TEXTURE");
  replacement = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING, want_w, want_h);
  if (!replacement) {
    /* Put the engine back where the surviving texture can still serve it. */
    StartupLog("DISPLAY STEP 02 FAILED: SDL_CreateTexture %dx%d: %s",
               want_w, want_h, SDL_GetError());
    AleksCrash_LogSdlOnce("SDL_CreateTexture(aspect)");
    ZeldaSetAspect(AleksAspect_FromExtraSide((g_texture_w / tex_mult - 256) / 2));
    ApplyGameplayGeometry();
    AleksCrash_Step("STEP 02 REVERTED");
    return false;
  }
  AleksCrash_Step("STEP 03 TEXTURE CREATED");

  {
    SDL_Texture *old = g_texture;
    g_texture = replacement;            /* swap */
    g_texture_w = want_w;
    g_texture_h = want_h;
    AleksCrash_Step("STEP 04 SWAP");
    if (old) SDL_DestroyTexture(old);   /* only now is the old one unused */
    AleksCrash_Step("STEP 05 DESTROY OLD");
  }

  AleksCrash_Step("STEP 06 LAYOUT INVALIDATE");
  AleksCrash_PublishTexture(g_texture_w, g_texture_h);
  StartupLog("ASPECT APPLIED: source %dx%d texture %dx%d",
             g_snes_width, g_snes_height, g_texture_w, g_texture_h);
  AleksCrash_Step("STEP 08 APPLY COMPLETE");
  AleksDisplayJournal("ASPECT", "STEP 08 APPLY COMPLETE");
  return true;
}

static void DrawPpuFrameWithPerf() {
  /* The engine flags the change on the frame it applies it; the frontend
   * catches up here, before anything locks the old texture. */
  if (ZeldaConsumeDisplayReconfig())
    SdlRenderer_ApplyAspect();

  if (!g_zenv.ppu || !g_renderer_funcs.BeginDraw || !g_renderer_funcs.EndDraw)
    Die("Renderer or PPU was not initialized before draw");
  int render_scale = PpuGetCurrentRenderScale(g_zenv.ppu, g_ppu_render_flags);
  if (render_scale <= 0 || g_snes_width <= 0 || g_snes_height <= 0)
    Die("Invalid first-frame render geometry");
  uint8 *pixel_buffer = 0;
  int pitch = 0;

  g_renderer_funcs.BeginDraw(g_snes_width * render_scale,
                             g_snes_height * render_scale,
                             &pixel_buffer, &pitch);
  if (g_display_perf || g_config.display_perf_title) {
    static float history[64], average;
    static int history_pos;
    uint64 before = SDL_GetPerformanceCounter();
    ZeldaDrawPpuFrame(pixel_buffer, pitch, g_ppu_render_flags);
    uint64 after = SDL_GetPerformanceCounter();
    float v = (double)SDL_GetPerformanceFrequency() / (after - before);
    average += v - history[history_pos];
    history[history_pos] = v;
    history_pos = (history_pos + 1) & 63;
    g_curr_fps = average * (1.0f / 64);
  } else {
    ZeldaDrawPpuFrame(pixel_buffer, pitch, g_ppu_render_flags);
  }
  if (g_display_perf)
    RenderNumber(pixel_buffer + pitch * render_scale, pitch, g_curr_fps, render_scale == 4);
  /* The companion's save-state picker grabs its thumbnail off this frame.
   * Costs nothing unless a save asked for one (donor behaviour). */
  SecondScreen_CaptureFrameHook(pixel_buffer, pitch,
                                g_snes_width * render_scale,
                                g_snes_height * render_scale);
  g_renderer_funcs.EndDraw();
}

static SDL_mutex *g_audio_mutex;
static uint8 *g_audiobuffer, *g_audiobuffer_cur, *g_audiobuffer_end;
static int g_frames_per_block;
static uint8 g_audio_channels;

/* Set by the audio thread, drained by the game thread.  The callback must not
 * touch the log file: startup.log is a plain FILE* and two threads writing it
 * produced interleaved half-lines on hardware. */
static volatile bool g_audio_callback_seen;

static void SDLCALL AudioCallback(void *userdata, Uint8 *stream, int len) {
  g_audio_callback_seen = true;
  if (!g_audio_ready) {
    SDL_memset(stream, 0, len);
    return;
  }
  if (SDL_LockMutex(g_audio_mutex)) Die("Mutex lock failed!");
  while (len != 0) {
    if (g_audiobuffer_end - g_audiobuffer_cur == 0) {
      ZeldaRenderAudio((int16*)g_audiobuffer, g_frames_per_block, g_audio_channels);
      g_audiobuffer_cur = g_audiobuffer;
      g_audiobuffer_end = g_audiobuffer + g_frames_per_block * g_audio_channels * sizeof(int16);
    }
    int n = IntMin(len, g_audiobuffer_end - g_audiobuffer_cur);
    if (g_sdl_audio_mixer_volume == SDL_MIX_MAXVOLUME) {
      memcpy(stream, g_audiobuffer_cur, n);
    } else {
      SDL_memset(stream, 0, n);
      SDL_MixAudioFormat(stream, g_audiobuffer_cur, AUDIO_S16, n, g_sdl_audio_mixer_volume);
    }
    g_audiobuffer_cur += n;
    stream += n;
    len -= n;
  }

  ZeldaDiscardUnusedAudioFrames();
  SDL_UnlockMutex(g_audio_mutex);
}

// State for sdl renderer
static SDL_Rect g_sdl_renderer_rect;

static bool SdlRenderer_Init(SDL_Window *window) {

  if (g_config.shader)
    fprintf(stderr, "Warning: Shaders are supported only with the OpenGL backend\n");

  SDL_Renderer *renderer = SDL_CreateRenderer(g_window, -1,
                                              g_config.output_method == kOutputMethod_SDLSoftware ? SDL_RENDERER_SOFTWARE :
                                              SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (renderer == NULL) {
    printf("Failed to create renderer: %s\n", SDL_GetError());
    StartupLog("SDL_CreateRenderer failed: %s", SDL_GetError());
    return false;
  }
  SDL_RendererInfo renderer_info;
  SDL_GetRendererInfo(renderer, &renderer_info);
  if (kDebugFlag) {
    printf("Supported texture formats:");
    for (int i = 0; i < renderer_info.num_texture_formats; i++)
      printf(" %s", SDL_GetPixelFormatName(renderer_info.texture_formats[i]));
    printf("\n");
  }
  g_renderer = renderer;
  /* NO SDL_RenderSetLogicalSize.  It used to be set here, which put SDL in a
   * 256x224 coordinate space while the compositor computed destination rects
   * in output pixels -- two coordinate systems on one renderer, and the reason
   * NORMAL never matched the label on the tin.  aleks_layout.c owns every
   * rectangle now, in output pixels, for every mode. */
  if (g_config.linear_filtering)
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");

  {
    int tex_mult = (g_ppu_render_flags & kPpuRenderFlags_4x4Mode7) ? 4 : 1;
    g_texture_w = kPpuXPixels * tex_mult;      /* widest the PPU can render */
    g_texture_h = 240 * tex_mult;              /* tallest, incl. extend_y */
    g_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING,
                                  g_texture_w, g_texture_h);
    if (g_texture == NULL) {
      printf("Failed to create texture: %s\n", SDL_GetError());
      StartupLog("SDL_CreateTexture failed: %s", SDL_GetError());
      return false;
    }
    AleksCrash_PublishTexture(g_texture_w, g_texture_h);
    StartupLog("GAME TEXTURE: %dx%d", g_texture_w, g_texture_h);
  }
  /* Always brought up, not only when the ini already selects DUAL/FLIP: the
   * mode can be toggled at runtime with ZR+L3, and the companion surface is
   * allocated lazily on its first draw, so this costs nothing in NORMAL. */
  if (!AleksCompositor_Init(g_window, renderer)) {
    StartupLog("ALEKS companion compositor unavailable — NORMAL only");
    g_config.aleks_display_mode = 0;
  } else {
    {
      SDL_DisplayMode cur, desk;
      int ren_w = 0, ren_h = 0, win_w = 0, win_h = 0;
      SDL_GetWindowSize(g_window, &win_w, &win_h);
      SDL_GetRendererOutputSize(renderer, &ren_w, &ren_h);
      StartupLog("DISPLAY PROBE: displays=%d", SDL_GetNumVideoDisplays());
      if (SDL_GetCurrentDisplayMode(0, &cur) == 0)
        StartupLog("DISPLAY PROBE: current mode=%dx%d @%dHz fmt=%s",
                   cur.w, cur.h, cur.refresh_rate,
                   SDL_GetPixelFormatName(cur.format));
      else
        StartupLog("DISPLAY PROBE: current mode FAILED: %s", SDL_GetError());
      if (SDL_GetDesktopDisplayMode(0, &desk) == 0)
        StartupLog("DISPLAY PROBE: desktop mode=%dx%d", desk.w, desk.h);
      else
        StartupLog("DISPLAY PROBE: desktop mode FAILED: %s", SDL_GetError());
      StartupLog("DISPLAY PROBE: window=%dx%d renderer=%dx%d flags=0x%08x",
                 win_w, win_h, ren_w, ren_h,
                 (unsigned)SDL_GetWindowFlags(g_window));
      StartupLog("DISPLAY PROBE: video driver=%s", SDL_GetCurrentVideoDriver());
    }
    AleksDisplay_LogBoot(&StartupLog);
    StartupLog("ALEKS companion compositor ready (mode: %s)",
               g_config.aleks_display_mode == ALEKS_DISPLAY_FLIP ? "Flip" :
               g_config.aleks_display_mode == ALEKS_DISPLAY_DUAL ? "Dual" : "Normal");
  }
  return true;
}

static void SdlRenderer_Destroy() {
  AleksCompositor_Shutdown();
  SDL_DestroyTexture(g_texture);
  SDL_DestroyRenderer(g_renderer);
}

static void SdlRenderer_BeginDraw(int width, int height, uint8 **pixels, int *pitch) {
  /* A rect wider than the texture is the one failure that used to kill the
   * process.  Clamp instead: a frame drawn into the space that exists is
   * survivable, an abort is not. */
  /* INVARIANT: the lock rect must fit the texture.  The texture is sized for
   * the widest frame the PPU can render, so this cannot happen any more -- and
   * if it ever does, it is a geometry bug worth a report rather than a clamp
   * that hides it. */
  if (width > g_texture_w || height > g_texture_h) {
    AleksFatalf("POST-ASPECT INVARIANT FAILED: lock %dx%d > texture %dx%d "
                "(source %dx%d)", width, height, g_texture_w, g_texture_h,
                g_snes_width, g_snes_height);
  }
  g_sdl_renderer_rect.w = width;
  g_sdl_renderer_rect.h = height;
  if (SDL_LockTexture(g_texture, &g_sdl_renderer_rect, (void **)pixels, pitch) != 0) {
    AleksCrash_LogSdlOnce("SDL_LockTexture");
    Die("SDL_LockTexture failed");
  }
  if (!*pixels || *pitch <= 0)
    Die("SDL_LockTexture returned an invalid pixel buffer");
}

static void SdlRenderer_EndDraw() {

//  uint64 before = SDL_GetPerformanceCounter();
  SDL_UnlockTexture(g_texture);
//  uint64 after = SDL_GetPerformanceCounter();
//  float v = (double)(after - before) / SDL_GetPerformanceFrequency();
//  printf("%f ms\n", v * 1000);
  /* One present, one place that decides where the picture goes -- NORMAL
   * included.  The old fallback blit here was a second presentation path with
   * its own idea of the coordinate system. */
  AleksCompositor_PrepareRenderer(g_renderer, &g_sdl_renderer_rect);
  if (!AleksCompositor_Present(g_renderer, g_texture, &g_sdl_renderer_rect)) {
    AleksCrash_LogSdlOnce("AleksCompositor_Present");
    SDL_RenderPresent(g_renderer);
  }
}

static const struct RendererFuncs kSdlRendererFuncs  = {
  &SdlRenderer_Init,
  &SdlRenderer_Destroy,
  &SdlRenderer_BeginDraw,
  &SdlRenderer_EndDraw,
};

void OpenGLRenderer_Create(struct RendererFuncs *funcs, bool use_opengl_es);

#undef main
/*
 * SWITCH LIFECYCLE / SRAM SAFETY (ALEKS: GBAtemp report "pressing HOME quit
 * the game without saving").
 *
 * HOME is never seen by this program.  The OS owns it: SDL delivers no
 * SDL_CONTROLLER_BUTTON_GUIDE press for it, kAleksShortcutButtons (config.c)
 * deliberately does not list Guide, and the CONTROLS binder explicitly
 * refuses to capture it (see the capture path in HandleGamepadInput).  So
 * there is no ALEKS or Zelda action HOME can trigger, and nothing here treats
 * it as "quit".
 *
 * What HOME does is suspend the applet.  The process is only torn down when
 * the OS asks for it -- the player launching another title, or an applet-mode
 * NRO being evicted -- and SDL's Switch backend turns that request into
 * SDL_QUIT.  The real gap was on our side: SRAM was written ONLY by the
 * game's own save points (select_file.c, messaging.c) and by nothing else, so
 * an OS-initiated shutdown persisted nothing at all.
 *
 * This is the smallest close of that gap.  It is NOT an autosave: it does not
 * run during play, it does not run on a timer, it changes no defaults, and it
 * writes exactly the 8 KB the SNES game itself owns.  Vanilla semantics are
 * preserved -- progress since the player's last in-game save is still lost,
 * because SRAM does not contain it.
 */
/* Cleared when the app comes back to the foreground, so a suspend/resume/
 * suspend cycle flushes each time rather than only once. */
static bool g_sram_flushed_this_stop;

static void FlushSramForShutdown(const char *reason) {
  if (g_sram_flushed_this_stop || g_run_without_emu || !g_zenv.sram) return;
  /* Never overwrite a good sram.dat with an all-zero buffer: if the emulator
   * never got far enough to read one in, there is nothing worth persisting
   * and ZeldaWriteSram would also rotate the .bak away. */
  for (int i = 0; i < 8192; i++) {
    if (g_zenv.sram[i] != 0) {
      g_sram_flushed_this_stop = true;
      StartupLog("SRAM FLUSH reason=%s", reason);
      ZeldaWriteSram();
      StartupLog("SRAM FLUSH COMPLETE");
      return;
    }
  }
  StartupLog("SRAM FLUSH SKIPPED (empty sram) reason=%s", reason);
}

int main(int argc, char** argv) {
  StartupLog_Init();
  SetBootStage("[BOOT 01] main entered");
  argc--, argv++;
  const char *config_file = NULL;
  if (argc >= 2 && strcmp(argv[0], "--config") == 0) {
    config_file = argv[1];
    argc -= 2, argv += 2;
  } else {
    SetBootStage("[BOOT 02] filesystem/root setup begin");
    SwitchDirectory();
    SetBootStage("[BOOT 03] filesystem/root setup done");
  }
#ifdef __SWITCH__
  Result romfs_result = romfsInit();
  if (R_FAILED(romfs_result))
    Die("Unable to initialize bundled extraction resources");
#endif
  EnsureRuntimeLayout();
  if (!ConfigFileExists()) {
    /* Defaults first: the writer serialises whatever they are, so the file on
     * the card and the compiled fallback can never disagree. */
    Config_SetDefaults();
    g_config_was_created = Config_WriteDefaultIni("zelda3.ini");
    StartupLog("CONFIG FOUND: no");
    StartupLog("CONFIG CREATED: %s", g_config_was_created ? "yes" :
               "FAILED (running on compiled defaults)");
  } else {
    StartupLog("CONFIG FOUND: yes");
    StartupLog("CONFIG CREATED: no");
  }
  SetBootStage("[BOOT 04] config load begin");
  ParseConfigFile(config_file);
  SetBootStage("[BOOT 05] config load done");
  SetBootStage("[BOOT 06] asset load begin");
#ifdef __SWITCH__
  if (!AssetExtractor_AssetsFileIsValid("zelda3_assets.dat")) {
    char extraction_error[256] = {0};
    char selected_rom[256] = {0};
    StartupLog("[EXTRACT 01] asset missing or invalid");
    StartupLog("[EXTRACT 02] ROM scan");
    AssetExtractor_SetLogCallback(AssetExtractionLog);
    if (!SwitchFirstRun_EnsureAssets(selected_rom, sizeof(selected_rom), extraction_error, sizeof(extraction_error))) {
      StartupLog("Extraction failed: %s", extraction_error);
      romfsExit();
      return 1;
    }
    AssetExtractor_SetLogCallback(NULL);
  }
#endif
  LoadAssets();
  SetBootStage("[BOOT 09] assets loaded");
  StartupLog("SOUND BANK 0 SIZE: %u", (unsigned)kSoundBank_intro_SIZE);
  StartupLog("SOUND BANK 1 SIZE: %u", (unsigned)kSoundBank_indoor_SIZE);
  StartupLog("SOUND BANK 2 SIZE: %u", (unsigned)kSoundBank_ending_SIZE);
  StartupLog("[EXTRACT 10] boot game");
  LoadLinkGraphics();

  SetBootStage("[BOOT 10] Zelda engine init begin");
  ZeldaInitialize();
  SetBootStage("[BOOT 11] Zelda engine init done");
  SecondScreen_Init();
  SecondScreenState second_screen_state;
  SecondScreen_GetState(&second_screen_state);
  StartupLog("SECOND SCREEN CORE: module=%04x area=%d dungeon=%04x indoors=%d world=%s link=%d,%d item_slot=%d rupees=%d bombs=%d arrows=%d",
             second_screen_state.module, second_screen_state.area, second_screen_state.dungeon,
             second_screen_state.indoors, second_screen_state.dark_world ? "dark" : "light",
             second_screen_state.link_x, second_screen_state.link_y,
             second_screen_state.equipped_slot, second_screen_state.rupees,
             second_screen_state.bombs, second_screen_state.arrows);
  g_zenv.ppu->extraLeftRight = UintMin(g_config.extended_aspect_ratio, kPpuExtraLeftRight);
  ApplyGameplayGeometry();
  /* Esteban fixed camera: apply the stored choice before the first frame. */
  ZeldaSetWidescreenEdgeMode(g_config.aleks_wide_camera ? 1 : 0);
  StartupLog("WIDE CAMERA: %s", g_config.aleks_wide_camera ? "FIXED" : "STANDARD");
  StartupLog("AUTOSAVE: %s", g_config.autosave ? "on" : "off");
  StartupLog("DISPLAY MODE: %s", g_config.aleks_display_mode == 2 ? "Flip" :
             g_config.aleks_display_mode == 1 ? "Dual" : "Normal");
  StartupLog("DISPLAY MODE: %s", g_config.extended_aspect_ratio ? "16:9 extended" : "4:3");
  StartupLog("EXTENDED WIDTH: %d", g_snes_width);


  // Delay actually setting those features in ram until any snapshots finish playing.
  g_wanted_zelda_features = g_config.features0;

  g_ppu_render_flags = g_config.new_renderer * kPpuRenderFlags_NewRenderer |
                       g_config.enhanced_mode7 * kPpuRenderFlags_4x4Mode7 |
                       g_config.extend_y * kPpuRenderFlags_Height240 |
                       g_config.no_sprite_limits * kPpuRenderFlags_NoSpriteLimits;
  ZeldaEnableMsu(g_config.enable_msu);
  /* Optional translated dialogue, registered into the asset arrays before the
   * engine picks a language.  A missing or broken pack simply leaves the
   * built-in English in place. */
  AleksLang_Init();
  if (AleksLang_CurrentIndex() == 0)
    ZeldaSetLanguage(NULL);           /* built-in English, incl. the fallback */
  else
    ZeldaSetLanguage(g_config.language);

  if (g_config.fullscreen == 1)
    g_win_flags ^= SDL_WINDOW_FULLSCREEN_DESKTOP;
  else if (g_config.fullscreen == 2)
    g_win_flags ^= SDL_WINDOW_FULLSCREEN;

  // Window scale (1=100%, 2=200%, 3=300%, etc.)
  g_current_window_scale = (g_config.window_scale == 0) ? 2 : IntMin(g_config.window_scale, kMaxWindowScale);

  // audio_freq: Use common sampling rates (see user config file. values higher than 48000 are not supported.)
  if (g_config.audio_freq < 11025 || g_config.audio_freq > 48000)
    g_config.audio_freq = kDefaultFreq;

  // Currently, the SPC/DSP implementation only supports up to stereo.
  if (g_config.audio_channels < 1 || g_config.audio_channels > 2)
    g_config.audio_channels = kDefaultChannels;

  // audio_samples: power of 2
  if (g_config.audio_samples <= 0 || ((g_config.audio_samples & (g_config.audio_samples - 1)) != 0))
    g_config.audio_samples = kDefaultSamples;

  // set up SDL
  SetBootStage("[BOOT 12] SDL_Init begin");
  if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
    printf("Failed to init SDL: %s\n", SDL_GetError());
    StartupLog("SDL_Init failed: %s", SDL_GetError());
    return 1;
  }
  SetBootStage("[BOOT 13] SDL_Init done");

  bool custom_size  = g_config.window_width != 0 && g_config.window_height != 0;
  int window_width  = custom_size ? g_config.window_width  : g_current_window_scale * g_snes_width;
  int window_height = custom_size ? g_config.window_height : g_current_window_scale * g_snes_height;

  if (g_config.output_method == kOutputMethod_OpenGL ||
      g_config.output_method == kOutputMethod_OpenGL_ES) {
    g_win_flags |= SDL_WINDOW_OPENGL;
    OpenGLRenderer_Create(&g_renderer_funcs, (g_config.output_method == kOutputMethod_OpenGL_ES));
  } else {
    g_renderer_funcs = kSdlRendererFuncs;
  }

#ifdef __SWITCH__
  /*
   * Create the window FULLSCREEN at the native handheld resolution.
   *
   * This is the final ALEKS TMC lesson, and it is the real fix for the
   * 1280x1280 surface: switch-sdl2 only reports an honest renderer output size
   * when the window was created fullscreen at the display resolution.  Created
   * any other way it hands back 1280x1280 on a 16:9 panel, which is what made
   * everything wide and squashed.
   *
   * TMC's own note is explicit that this must happen AT CREATION -- resizing
   * afterwards with SDL_SetWindowSize does not refresh the renderer's output
   * size on this backend, so it cannot be corrected later.
   *
   * The surface correction in aleks_layout.c stays: with an honest surface it
   * computes to exactly 1:1 and costs nothing, and it keeps the picture right
   * if a dock transition or a future SDL ever reports something else again.
   */
  /*
   * Created AT the resolution SDL says the display currently is.
   *
   * This is the rule the hardware taught us, and the logs prove it: the build
   * that let the window be created any other way recorded
   *     output=1280x1280 (window+renderer agree)
   * -- the WINDOW itself was square, not merely the renderer answering with a
   * bound texture -- and that value went straight into the layout.  Creating
   * it fullscreen at the display's own mode is what makes the surface honest.
   *
   * The current display mode already follows the dock state (1280x720
   * handheld, 1920x1080 docked), so asking SDL is both correct and future
   * proof; the operation mode is the fallback when SDL cannot answer, and a
   * non-16:9 answer is refused outright because that is the failure this
   * exists to prevent.
   */
  {
    SDL_DisplayMode dm = {0};   /* logged even when the query fails */
    bool docked = appletGetOperationMode() == AppletOperationMode_Console;
    window_width  = docked ? 1920 : 1280;
    window_height = docked ? 1080 : 720;
    if (SDL_GetCurrentDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0 &&
        (long)dm.w * 9 == (long)dm.h * 16) {
      window_width = dm.w;
      window_height = dm.h;
    }
    g_win_flags |= SDL_WINDOW_FULLSCREEN;
    StartupLog("SWITCH OUTPUT: mode=%s physical=%dx%d (sdl current mode %dx%d)",
               docked ? "DOCKED" : "HANDHELD", window_width, window_height,
               dm.w, dm.h);
  }
#endif
  SetBootStage("[BOOT 14] window creation begin");
  SDL_Window* window = SDL_CreateWindow(kWindowTitle, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, window_width, window_height, g_win_flags);
  if(window == NULL) {
    printf("Failed to create window: %s\n", SDL_GetError());
    StartupLog("SDL_CreateWindow failed: %s", SDL_GetError());
    return 1;
  }
  g_window = window;
#ifndef __SWITCH__
  SDL_SetWindowHitTest(window, HitTestCallback, NULL);
#endif
  SetBootStage("[BOOT 15] window creation done");

  SetBootStage("[BOOT 16] renderer init begin");
  if (!g_renderer_funcs.Initialize(window))
    return 1;
  SetBootStage("[BOOT 17] renderer init done");

  SDL_AudioDeviceID device = 0;
  SDL_AudioSpec want = { 0 }, have;
  g_audio_mutex = SDL_CreateMutex();
  if (!g_audio_mutex) Die("No mutex");

  StartupLog("SDL AUDIO INIT: EnableAudio=%d freq=%u channels=%u samples=%u",
             g_config.enable_audio ? 1 : 0, g_config.audio_freq,
             g_config.audio_channels, g_config.audio_samples);
  if (g_config.enable_audio) {
    SetBootStage("[BOOT 18] audio init begin");
    want.freq = g_config.audio_freq;
    want.format = AUDIO_S16;
    want.channels = g_config.audio_channels;
    want.samples = g_config.audio_samples;
    want.callback = &AudioCallback;
    device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (device == 0) {
      /* Audio is not worth losing the game over: say exactly why and play on.
       * (Before this pass a failure here returned 1 and killed the launch.) */
      printf("Failed to open audio device: %s\n", SDL_GetError());
      StartupLog("AUDIO DEVICE: FAILED - %s", SDL_GetError());
      StartupLog("AUDIO CALLBACK: never (device did not open)");
      g_config.enable_audio = false;
    } else {
      SDL_PauseAudioDevice(device, 1);
      g_audio_channels = have.channels;
      g_frames_per_block = (534 * have.freq) / 32000;
      g_audiobuffer = malloc(g_frames_per_block * have.channels * sizeof(int16));
      if (!g_audiobuffer) Die("Audio buffer allocation failed");
      g_audio_ready = true;
      StartupLog("AUDIO DEVICE: id=%u obtained freq=%d channels=%d samples=%d format=0x%04x",
                 (unsigned)device, have.freq, have.channels, have.samples,
                 (unsigned)have.format);
      SetBootStage("[BOOT 19] audio init done");
    }
  } else {
    StartupLog("AUDIO DEVICE: skipped (EnableAudio = 0)");
    StartupLog("AUDIO CALLBACK: never (audio disabled in config)");
  }

  if (argc >= 1 && !g_run_without_emu)
    LoadRom(argv[0]);

#if defined(_WIN32)
  _mkdir("saves");
#else
  mkdir("saves", 0755);
#endif

  SetBootStage("[BOOT 20] save init begin");
  ZeldaReadSram();
  SetBootStage("[BOOT 21] save init done");

  /* RetroAchievements, last and optional.  Returns immediately when disabled;
   * when enabled it only creates the client -- the login is deferred until
   * after the first frame, so nothing here can delay or block the boot. */
  AleksRA_Init();

  for (int i = 0; i < SDL_NumJoysticks(); i++)
    OpenOneGamepad(i);

  bool running = true;
  SDL_Event event;
  uint32 lastTick = SDL_GetTicks();
  uint32 curTick = 0;
  uint32 frameCtr = 0;
  bool audiopaused = true;
  bool first_event_loop = true;
  bool first_frame = true;

  /*
   * AUTOSAVE, restore side.
   *
   * Hardware reported occasional crashes/closes around autosave, so this pass
   * makes it observable and conservative rather than assuming the donor logic
   * is safe here.  It is also OFF by default now (config.c).
   */
  if (g_config.autosave) {
    FILE *probe = fopen("saves/save0.sav", "rb");
    StartupLog("AUTOSAVE REQUEST reason=startup");
    if (probe) {
      fclose(probe);
      StartupLog("AUTOSAVE LOAD BEGIN");
      HandleCommand(kKeys_Load + 0, true);
      StartupLog("AUTOSAVE LOAD COMPLETE");
    } else {
      StartupLog("AUTOSAVE LOAD SKIPPED: no saves/save0.sav");
    }
  } else {
    StartupLog("AUTOSAVE: disabled");
  }

  while(running) {
    if (first_event_loop) {
      SetBootStage("[BOOT 26] first event loop iteration");
      first_event_loop = false;
    }
    while(SDL_PollEvent(&event)) {
      switch(event.type) {
      case SDL_CONTROLLERDEVICEADDED:
        OpenOneGamepad(event.cdevice.which);
        break;
      case SDL_CONTROLLERAXISMOTION:
        HandleGamepadAxisInput(event.caxis.which, event.caxis.axis, event.caxis.value);
        break;
      case SDL_CONTROLLERBUTTONDOWN:
      case SDL_CONTROLLERBUTTONUP: {
        int b = RemapSdlButton(event.cbutton.button);
        if (b >= 0)
          HandleGamepadInput(b, event.type == SDL_CONTROLLERBUTTONDOWN);
        break;
      }
      case SDL_MOUSEWHEEL:
        if (SDL_GetModState() & KMOD_CTRL && event.wheel.y != 0)
          ChangeWindowScale(event.wheel.y > 0 ? 1 : -1);
        break;
      case SDL_MOUSEBUTTONDOWN:
        if (event.button.button == SDL_BUTTON_LEFT && event.button.state == SDL_PRESSED && event.button.clicks == 2) {
          if ((g_win_flags & SDL_WINDOW_FULLSCREEN_DESKTOP) == 0 && (g_win_flags & SDL_WINDOW_FULLSCREEN) == 0 && SDL_GetModState() & KMOD_SHIFT) {
            g_win_flags ^= SDL_WINDOW_BORDERLESS;
            SDL_SetWindowBordered(g_window, (g_win_flags & SDL_WINDOW_BORDERLESS) == 0);
          }
        }
        break;
      case SDL_MOUSEBUTTONUP:
        if (event.button.button == SDL_BUTTON_LEFT && event.button.which != SDL_TOUCH_MOUSEID)
          AleksCompositor_HandleTouch(event.button.x, event.button.y);
        break;
      /* Finger coordinates are normalised, so they need the PHYSICAL size --
       * from the one resolver, not a second SDL query that a bound render
       * target could answer with a texture's dimensions. */
      case SDL_FINGERDOWN: {
        int tw = 0, th = 0;
        if (AleksDisplay_GetPhysicalOutputSize(&tw, &th))
          AleksCompositor_TouchDown((int)(event.tfinger.x * tw), (int)(event.tfinger.y * th));
        break;
      }
      case SDL_FINGERUP: {
        int tw = 0, th = 0;
        if (AleksDisplay_GetPhysicalOutputSize(&tw, &th))
          AleksCompositor_HandleTouch((int)(event.tfinger.x * tw), (int)(event.tfinger.y * th));
        break;
      }
      case SDL_KEYDOWN:
        HandleInput(event.key.keysym.sym, event.key.keysym.mod, true);
        break;
      case SDL_KEYUP:
        HandleInput(event.key.keysym.sym, event.key.keysym.mod, false);
        break;
      case SDL_QUIT:
        /* The OS asked us to go away (HOME -> another title, applet-mode
         * eviction, or hbmenu close).  Persist before unwinding: the cleanup
         * below can still be cut short if the request is impatient. */
        StartupLog("LIFECYCLE: SDL_QUIT");
        FlushSramForShutdown("sdl-quit");
        running = false;
        break;
      /* Focus transitions.  On Switch these bracket the HOME menu.  Nothing
       * is redesigned here -- audio already follows g_paused, the renderer and
       * the RA session are untouched -- the app just makes sure the player's
       * SRAM is on the card before it can be killed while suspended, and
       * leaves a line in the log so a hardware report can tell a suspend from
       * a termination. */
      case SDL_APP_WILLENTERBACKGROUND:
        StartupLog("LIFECYCLE: entering background");
        FlushSramForShutdown("background");
        break;
      case SDL_APP_DIDENTERFOREGROUND:
        StartupLog("LIFECYCLE: returned to foreground");
        g_sram_flushed_this_stop = false;
        break;
      case SDL_APP_TERMINATING:
        StartupLog("LIFECYCLE: terminating");
        FlushSramForShutdown("terminating");
        running = false;
        break;
      }
    }

    if (g_paused != audiopaused) {
      audiopaused = g_paused;
      if (device)
        SDL_PauseAudioDevice(device, audiopaused);
    }

    if (g_paused) {
      SDL_Delay(16);
      continue;
    }

    // Clear gamepad inputs when joypad directional inputs to avoid wonkiness
    int inputs = g_input1_state;
    if (g_input1_state & 0xf0)
      g_gamepad_buttons = 0;
    inputs |= g_gamepad_buttons;

    if (first_frame)
      SetBootStage("[BOOT 22] first frame begin");
    // Samyost companion actions are queued by the Switch adapter and must be
    // applied on this same game thread before the engine advances a frame.
    if (g_audio_callback_seen) {
      static bool logged;
      if (!logged) { logged = true; StartupLog("AUDIO CALLBACK: ACTIVE"); }
    }
    SecondScreen_RunFrameHook();
    AleksCrash_PublishRuntime(g_config.aleks_gameplay_aspect,
                              g_config.aleks_wide_camera,
                              g_config.autosave ? 1 : 0, 0,
                              g_config.aleks_companion_page);
    SDL_LockMutex(g_audio_mutex);
    bool is_replay = ZeldaRunFrame(inputs);
    SDL_UnlockMutex(g_audio_mutex);

    /* RetroAchievements evaluates against the frame that just ran, outside
     * the audio lock and after the engine has settled.  A no-op when RA is
     * off, unconfigured or offline. */
    AleksRA_DoFrame();

    frameCtr++;

    if ((g_turbo ^ (is_replay & g_replay_turbo)) && (frameCtr & (g_turbo ? 0xf : 0x7f)) != 0) {
      continue;
    }

    if (first_frame)
      SetBootStage("[BOOT 23] first frame PPU draw begin");
    DrawPpuFrameWithPerf();
    if (first_frame) {
      SetBootStage("[BOOT 24] first frame PPU draw done");
      SetBootStage("[BOOT 25] first frame present done");
      SetBootStage("[BOOT 27] stable game loop reached");
      first_frame = false;
      /* Only now, with a frame already on screen: a token re-login talks to
       * the network synchronously, and it must never sit on the boot path. */
      AleksRA_AutoLoginAfterFirstPresent();
    }

    if (g_config.display_perf_title) {
      char title[60];
      snprintf(title, sizeof(title), "%s | FPS: %d", kWindowTitle, g_curr_fps);
      SDL_SetWindowTitle(g_window, title);
    }

    // if vsync isn't working, delay manually
    curTick = SDL_GetTicks();

    if (!g_config.disable_frame_delay) {
      static const uint8 delays[3] = { 17, 17, 16 }; // 60 fps
      lastTick += delays[frameCtr % 3];

      if (lastTick > curTick) {
        uint32 delta = lastTick - curTick;
        if (delta > 500) {
          lastTick = curTick - 500;
          delta = 500;
        }
//        printf("Sleeping %d\n", delta);
        SDL_Delay(delta);
      } else if (curTick - lastTick > 500) {
        lastTick = curTick;
      }
    }
  }
  /*
   * AUTOSAVE, save side -- ordering matters here.
   *
   * SaveLoadSlot takes the APU lock, and the audio callback runs on SDL's own
   * thread reading the same APU state.  Writing the autosave while that
   * callback is still live is the most likely explanation for the intermittent
   * close reported on hardware, so the device is paused FIRST and the write
   * happens with nothing else touching the engine.
   *
   * The thumbnail path is deliberately not involved: it asks the renderer for
   * the next drawn frame, and during shutdown there will not be one.
   */
  /* Ordinary shutdown.  Same write as the event-loop paths, and the static
   * latch in there means it happens once per run however we got here. */
  FlushSramForShutdown("shutdown");

  if (g_config.autosave) {
    StartupLog("AUTOSAVE REQUEST reason=quit");
    if (g_audio_ready) {
      SDL_PauseAudioDevice(device, 1);
      SDL_LockAudioDevice(device);
      SDL_UnlockAudioDevice(device);   /* let an in-flight callback finish */
      StartupLog("AUTOSAVE LOCK ACQUIRED (audio paused)");
    }
    StartupLog("AUTOSAVE WRITE SLOT=0");
    HandleCommand(kKeys_Save + 0, true);
    StartupLog("AUTOSAVE COMPLETE");
  }

  // clean sdl
  if (g_config.enable_audio) {
    g_audio_ready = false;
    SDL_PauseAudioDevice(device, 1);
    SDL_CloseAudioDevice(device);
  }

  SDL_DestroyMutex(g_audio_mutex);
  free(g_audiobuffer);

  g_renderer_funcs.Destroy();

  /* Before SDL and the network go away: joins the badge worker and destroys
   * the RA client. */
  AleksRA_Shutdown();

  SDL_DestroyWindow(window);
  SDL_Quit();
  /* Reached the end normally: the next launch must not report this as
   * unclean. */
  AleksCrash_MarkCleanShutdown();
#ifdef __SWITCH__
  romfsExit();
#endif
  //SaveConfigFile();
  return 0;
}

static void RenderDigit(uint8 *dst, size_t pitch, int digit, uint32 color, bool big) {
  static const uint8 kFont[] = {
    0x1c, 0x36, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x36, 0x1c,
    0x18, 0x1c, 0x1e, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7e,
    0x3e, 0x63, 0x60, 0x30, 0x18, 0x0c, 0x06, 0x03, 0x63, 0x7f,
    0x3e, 0x63, 0x60, 0x60, 0x3c, 0x60, 0x60, 0x60, 0x63, 0x3e,
    0x30, 0x38, 0x3c, 0x36, 0x33, 0x7f, 0x30, 0x30, 0x30, 0x78,
    0x7f, 0x03, 0x03, 0x03, 0x3f, 0x60, 0x60, 0x60, 0x63, 0x3e,
    0x1c, 0x06, 0x03, 0x03, 0x3f, 0x63, 0x63, 0x63, 0x63, 0x3e,
    0x7f, 0x63, 0x60, 0x60, 0x30, 0x18, 0x0c, 0x0c, 0x0c, 0x0c,
    0x3e, 0x63, 0x63, 0x63, 0x3e, 0x63, 0x63, 0x63, 0x63, 0x3e,
    0x3e, 0x63, 0x63, 0x63, 0x7e, 0x60, 0x60, 0x60, 0x30, 0x1e,
  };
  const uint8 *p = kFont + digit * 10;
  if (!big) {
    for (int y = 0; y < 10; y++, dst += pitch) {
      int v = *p++;
      for (int x = 0; v; x++, v >>= 1) {
        if (v & 1)
          ((uint32 *)dst)[x] = color;
      }
    }
  } else {
    for (int y = 0; y < 10; y++, dst += pitch * 2) {
      int v = *p++;
      for (int x = 0; v; x++, v >>= 1) {
        if (v & 1) {
          ((uint32 *)dst)[x * 2 + 1] = ((uint32 *)dst)[x * 2] = color;
          ((uint32 *)(dst+pitch))[x * 2 + 1] = ((uint32 *)(dst + pitch))[x * 2] = color;
        }
      }
    }
  }
}

static void RenderNumber(uint8 *dst, size_t pitch, int n, bool big) {
  char buf[32], *s;
  int i;
  sprintf(buf, "%d", n);
  for (s = buf, i = 2 * 4; *s; s++, i += 8 * 4)
    RenderDigit(dst + ((pitch + i + 4) << big), pitch, *s - '0', 0x404040, big);
  for (s = buf, i = 2 * 4; *s; s++, i += 8 * 4)
    RenderDigit(dst + (i << big), pitch, *s - '0', 0xffffff, big);
}

static void HandleCommand_Locked(uint32 j, bool pressed);

static void HandleCommand(uint32 j, bool pressed) {
  if (j <= kKeys_Controls_Last) {
    static const uint8 kKbdRemap[] = { 0, 4, 5, 6, 7, 2, 3, 8, 0, 9, 1, 10, 11 };
    if (pressed)
      g_input1_state |= 1 << kKbdRemap[j];
    else
      g_input1_state &= ~(1 << kKbdRemap[j]);
    return;
  }

  if (j == kKeys_Turbo) {
    g_turbo = pressed;
    return;
  }

  // Everything that might access audio state
  // (like SaveLoad and Reset) must have the lock.
  SDL_LockMutex(g_audio_mutex);
  HandleCommand_Locked(j, pressed);
  SDL_UnlockMutex(g_audio_mutex);
}

void ZeldaApuLock() {
  SDL_LockMutex(g_audio_mutex);
}

void ZeldaApuUnlock() {
  SDL_UnlockMutex(g_audio_mutex);
}


static void HandleCommand_Locked(uint32 j, bool pressed) {
  if (!pressed)
    return;
  if (j <= kKeys_Load_Last) {
    SaveLoadSlot(kSaveLoad_Load, j - kKeys_Load);
  } else if (j <= kKeys_Save_Last) {
    SaveLoadSlot(kSaveLoad_Save, j - kKeys_Save);
  } else if (j <= kKeys_Replay_Last) {
    SaveLoadSlot(kSaveLoad_Replay, j - kKeys_Replay);
  } else if (j <= kKeys_LoadRef_Last) {
    SaveLoadSlot(kSaveLoad_Load, 256 + j - kKeys_LoadRef);
  } else if (j <= kKeys_ReplayRef_Last) {
    SaveLoadSlot(kSaveLoad_Replay, 256 + j - kKeys_ReplayRef);
  } else {
    switch (j) {
    case kKeys_CheatLife: PatchCommand('w'); break;
    case kKeys_CheatEquipment: PatchCommand('W'); break;
    case kKeys_CheatKeys: PatchCommand('o'); break;
    case kKeys_CheatWalkThroughWalls: PatchCommand('E'); break;
    case kKeys_ClearKeyLog: PatchCommand('k'); break;
    case kKeys_StopReplay: PatchCommand('l'); break;
    case kKeys_Fullscreen:
      g_win_flags ^= SDL_WINDOW_FULLSCREEN_DESKTOP;
      SDL_SetWindowFullscreen(g_window, g_win_flags & SDL_WINDOW_FULLSCREEN_DESKTOP);
      g_cursor = !g_cursor;
      SDL_ShowCursor(g_cursor);
      break;
    case kKeys_Reset:
      ZeldaReset(true);
      break;
    case kKeys_Pause: g_paused = !g_paused; break;
    case kKeys_PauseDimmed:
      g_paused = !g_paused;
      // SDL_RenderPresent may not be called more than once per frame.
      // Seems to work on Windows still. Temporary measure until it's fixed.
#ifdef _WIN32
      if (g_paused) {
        SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 159);
        SDL_RenderFillRect(g_renderer, NULL);
        SDL_RenderPresent(g_renderer);
      }
#endif
      break;
    case kKeys_ReplayTurbo: g_replay_turbo = !g_replay_turbo; break;
    case kKeys_WindowBigger: ChangeWindowScale(1); break;
    case kKeys_WindowSmaller: ChangeWindowScale(-1); break;
    case kKeys_DisplayPerf: g_display_perf ^= 1; break;
    case kKeys_ToggleRenderer: g_ppu_render_flags ^= kPpuRenderFlags_NewRenderer; break;
    case kKeys_VolumeUp:
    case kKeys_VolumeDown: HandleVolumeAdjustment(j == kKeys_VolumeUp ? 1 : -1); break;
    default: assert(0);
    }
  }
}

static void HandleInput(int keyCode, int keyMod, bool pressed) {
  int j = FindCmdForSdlKey(keyCode, keyMod);
  if (j != 0)
    HandleCommand(j, pressed);
}

static void OpenOneGamepad(int i) {
  if (SDL_IsGameController(i)) {
    SDL_GameController *controller = SDL_GameControllerOpen(i);
    if (!controller)
      fprintf(stderr, "Could not open gamepad %d: %s\n", i, SDL_GetError());
  }
}

static int RemapSdlButton(int button) {
  switch (button) {
  case SDL_CONTROLLER_BUTTON_A: return kGamepadBtn_A;
  case SDL_CONTROLLER_BUTTON_B: return kGamepadBtn_B;
  case SDL_CONTROLLER_BUTTON_X: return kGamepadBtn_X;
  case SDL_CONTROLLER_BUTTON_Y: return kGamepadBtn_Y;
  case SDL_CONTROLLER_BUTTON_BACK: return kGamepadBtn_Back;
  case SDL_CONTROLLER_BUTTON_GUIDE: return kGamepadBtn_Guide;
  case SDL_CONTROLLER_BUTTON_START: return kGamepadBtn_Start;
  case SDL_CONTROLLER_BUTTON_LEFTSTICK: return kGamepadBtn_L3;
  case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return kGamepadBtn_R3;
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return kGamepadBtn_L1;
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return kGamepadBtn_R1;
  case SDL_CONTROLLER_BUTTON_DPAD_UP: return kGamepadBtn_DpadUp;
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return kGamepadBtn_DpadDown;
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return kGamepadBtn_DpadLeft;
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return kGamepadBtn_DpadRight;
  default: return -1;
  }
}

/*
 * ALEKS shortcuts.  The final TMC model (port_runtime_config.cpp's
 * ezlo_shortcut): config stores a small index per action, and the input layer
 * resolves it to a physical control at press time.  The companion's SHORTCUTS
 * menu refuses to assign a control that a Zelda command already owns, so a
 * shortcut here can never be stealing a gameplay press.
 *
 * Returns true when the press was consumed as a shortcut.
 */
/* The shared table (config.h) -- the settings UI offers exactly what this
 * resolves, because it is now literally the same array. */
#define kShortcutButtons kAleksShortcutButtons
#define kShortcutButtonCount kAleksShortcutButtonCount

/* A chord's components must not go on to fire their own single-button
 * actions once the chord has been recognized and acted on. */
static void ConsumeChord(int a, int b) {
  AleksCompositor_ConsumeUntilRelease(a);
  AleksCompositor_ConsumeUntilRelease(b);
}

/* One place that changes the display mode, so continuity is never forgotten
 * at one of the three call sites. */
static void SetAleksDisplayMode(int to) {
  int from = g_config.aleks_display_mode;
  if (to == from) return;
  g_config.aleks_display_mode = (uint8)to;
  AleksCompositor_NotifyDisplayModeChanged(from, to);
}

/*
 * Quick Items.  A configured slot is an inventory-grid index (1..20), exactly
 * what SS_EquipSlot takes -- there is no separate item id to translate.  The
 * backend validates ownership and refuses politely, so no inventory knowledge
 * lives here.
 */
static bool HandleQuickItem(int button) {
  for (int i = 0; i < 2; i++) {
    int choice = g_config.aleks_quick_item_button[i];
    int slot = g_config.aleks_quick_item_slot[i];
    if (choice <= 0 || choice >= kShortcutButtonCount) continue;
    if (kShortcutButtons[choice] != button) continue;
    if (slot >= 1 && slot <= 20) {
      /* USE, not equip.  The Y selection is deliberately left alone -- that is
       * the whole point of a quick item, and the ITEMS page remains the way to
       * change what Y holds. */
      bool ok = SS_RequestUseItemSlot(slot);
      StartupLog("ALEKS quick item %d slot %d: %s", i + 1, slot,
                 ok ? "queued for use" : "not owned");
    }
    return true;   /* the button is claimed either way; never falls to Zelda */
  }
  return false;
}

static bool HandleAleksShortcut(int button, bool pressed) {
  if (!pressed)
    return false;
  for (int slot = 0; slot < kAleksShortcut_Count; slot++) {
    int choice = g_config.aleks_shortcut[slot];
    if (choice <= 0 || choice >= kShortcutButtonCount)
      continue;
    if (kShortcutButtons[choice] != button)
      continue;
    switch (slot) {
    case kAleksShortcut_Companion:  AleksCompositor_ToggleOverlay(); break;
    case kAleksShortcut_Settings:   AleksCompositor_ToggleSettings(); break;
    case kAleksShortcut_NextPage:   AleksCompositor_CyclePage(); break;
    /* Quick Save asks first, and opens the companion to do the asking.
     * Quick Load is immediate.  Both use the configured quick slot. */
    case kAleksShortcut_QuickSave:  AleksCompositor_ArmQuickSaveConfirm(); break;
    case kAleksShortcut_QuickLoad:  AleksCompositor_QuickLoad(); break;
    case kAleksShortcut_DisplayMode:
      SetAleksDisplayMode((g_config.aleks_display_mode + 1) % 3);
      break;
    default: return false;
    }
    AleksCompositor_ConsumeUntilRelease(button);
    StartupLog("ALEKS shortcut %d fired (button %d)", slot, button);
    return true;
  }
  return HandleQuickItem(button);
}

static void HandleGamepadInput(int button, bool pressed) {
  static bool dual_toggle_latched, page_latched, settings_latched, overlay_latched;
  if (!!(g_gamepad_modifiers & (1 << button)) == pressed)
    return;
  g_gamepad_modifiers ^= 1 << button;

  /*
   * BUTTON CAPTURE ("PRESS A BUTTON"), and why it never worked.
   *
   * The companion has had the whole consumer side of this for as long as the
   * remap screen has existed -- SS_ArmButtonCapture, the armed row, the eight
   * second timeout -- but NOTHING EVER WROTE g_ss_capture_button.  The
   * producer was simply missing, so an armed row waited the full timeout and
   * gave up every time.  That is the "did not work reliably on hardware" the
   * cycling rows were introduced to work around.
   *
   * This is that producer.  It runs before every other layer, because while a
   * row is armed the next press IS the answer and must not also toggle a
   * menu, fire a shortcut or reach Link.  The press is swallowed here and its
   * release is gated, so the button the player just assigned does not act on
   * the way back up.
   */
  if (g_ss_capture_button == -2) {
    if (pressed && button != kGamepadBtn_Guide) {   /* HOME belongs to the OS */
      g_ss_capture_button = button;
      AleksCompositor_ConsumeUntilRelease(button);
      StartupLog("CONTROLS: captured button %d", button);
    }
    return;
  }

  /*
   * INPUT PRIORITY, in this order and nothing else:
   *   1. companion / modal UI
   *   2. recognized hardware chord
   *   3. single-button ALEKS shortcut / Quick Item
   *   4. Zelda gameplay
   * Each layer returns on consumption.  The companion goes first so a modal
   * genuinely owns everything; chords go ahead of single-button actions so
   * ZR+L3 cannot also fire ZR's Quick Item.
   */

  /*
   * A button consumed by a menu stays consumed until it is let go, so the
   * press that closed the menu never reaches Zelda and a chord's held
   * components never fire their own actions afterwards.
   *
   * Only PRESSES are blocked.  The release must still flow through: gameplay
   * commands are level-based (HandleCommand sets a bit on press and clears it
   * on release), so swallowing the release of a button that had an active
   * command would leave Link running forever.  The chord latches below are
   * cleared on release too, and skipping them would make every chord work
   * only once.
   */
  if (AleksCompositor_IsConsumedUntilRelease(button)) {
    if (pressed) return;
    AleksCompositor_ReleaseButton(button);
    /* fall through: clear latches and let any active command end normally */
  }

  if (pressed && AleksCompositor_SettingsInput(button))
    return;

  const uint32 dual_combo = (1u << kGamepadBtn_R2) | (1u << kGamepadBtn_L3);
  if ((g_gamepad_modifiers & dual_combo) == dual_combo) {
    if (!dual_toggle_latched) {
      dual_toggle_latched = true;
      {
        bool to_dual = g_config.aleks_display_mode != ALEKS_DISPLAY_DUAL;
        AleksCrash_DisplayRequest(to_dual ? "Normal" : "Dual",
                                  to_dual ? "Dual" : "Normal");
        SetAleksDisplayMode(to_dual ? ALEKS_DISPLAY_DUAL : ALEKS_DISPLAY_NORMAL);
      }
      ConsumeChord(kGamepadBtn_R2, kGamepadBtn_L3);
      StartupLog("DISPLAY REQUEST ZR+L3 -> %s", g_config.aleks_display_mode ? "Dual" : "Normal");
    }
    return;
  }
  if ((g_gamepad_modifiers & dual_combo) != dual_combo)
    dual_toggle_latched = false;
  const uint32 overlay_combo = (1u << kGamepadBtn_L2) | (1u << kGamepadBtn_L3);
  if ((g_gamepad_modifiers & overlay_combo) == overlay_combo) {
    if (!overlay_latched) {
      overlay_latched = true;
      AleksCompositor_ToggleOverlay();
      ConsumeChord(kGamepadBtn_L2, kGamepadBtn_L3);
      StartupLog("ALEKS companion overlay: %s",
                 AleksCompositor_IsOverlayOpen() ? "opened" : "closed");
    }
    return;
  }
  if ((g_gamepad_modifiers & overlay_combo) != overlay_combo)
    overlay_latched = false;
  const uint32 settings_combo = (1u << kGamepadBtn_L2) | (1u << kGamepadBtn_R3);
  bool companion_visible = g_config.aleks_display_mode != ALEKS_DISPLAY_NORMAL ||
                           AleksCompositor_IsOverlayOpen();
  /*
   * ZL+R3 IS GLOBAL.  It used to carry "&& companion_visible", which made the
   * one shortcut for Settings unreachable from the very place a player wants
   * it -- plain NORMAL gameplay with nothing on screen.  The compositor now
   * brings the companion up itself when it has to, so the gate is gone.
   * Still edge-triggered by settings_latched and still release-gated by
   * ConsumeChord, so neither ZL nor R3 leaks into gameplay afterwards.
   */
  if ((g_gamepad_modifiers & settings_combo) == settings_combo) {
    if (!settings_latched) {
      settings_latched = true;
      AleksCompositor_OpenSettings();
      ConsumeChord(kGamepadBtn_L2, kGamepadBtn_R3);
      StartupLog("ALEKS settings: opened via ZL+R3");
    }
    return;
  }
  if ((g_gamepad_modifiers & settings_combo) != settings_combo)
    settings_latched = false;
  const uint32 page_combo = (1u << kGamepadBtn_L2) | (1u << kGamepadBtn_R2);
  if ((g_gamepad_modifiers & page_combo) == page_combo && companion_visible) {
    if (!page_latched) {
      page_latched = true;
      AleksCompositor_CyclePage();
      ConsumeChord(kGamepadBtn_L2, kGamepadBtn_R2);
      StartupLog("ALEKS companion page: %d", g_config.aleks_companion_page);
    }
    return;
  }
  if ((g_gamepad_modifiers & page_combo) != page_combo)
    page_latched = false;
  /* After the companion and the chords, before gameplay: a shortcut only ever
   * uses a control no Zelda command is bound to. */
  if (HandleAleksShortcut(button, pressed))
    return;
  if (pressed)
    g_gamepad_last_cmd[button] = FindCmdForGamepadButton(button, g_gamepad_modifiers);
  if (g_gamepad_last_cmd[button] != 0)
    HandleCommand(g_gamepad_last_cmd[button], pressed);
}

static void HandleVolumeAdjustment(int volume_adjustment) {
#if SYSTEM_VOLUME_MIXER_AVAILABLE
  int current_volume = GetApplicationVolume();
  int new_volume = IntMin(IntMax(0, current_volume + volume_adjustment * 5), 100);
  SetApplicationVolume(new_volume);
  printf("[System Volume]=%i\n", new_volume);
#else
  g_sdl_audio_mixer_volume = IntMin(IntMax(0, g_sdl_audio_mixer_volume + volume_adjustment * (SDL_MIX_MAXVOLUME >> 4)), SDL_MIX_MAXVOLUME);
  printf("[SDL mixer volume]=%i\n", g_sdl_audio_mixer_volume);
#endif
}

// Approximates atan2(y, x) normalized to the [0,4) range
// with a maximum error of 0.1620 degrees
// normalized_atan(x) ~ (b x + x^2) / (1 + 2 b x + x^2)
static float ApproximateAtan2(float y, float x) {
  uint32 sign_mask = 0x80000000;
  float b = 0.596227f;
  // Extract the sign bits
  uint32 ux_s = sign_mask & *(uint32 *)&x;
  uint32 uy_s = sign_mask & *(uint32 *)&y;
  // Determine the quadrant offset
  float q = (float)((~ux_s & uy_s) >> 29 | ux_s >> 30);
  // Calculate the arctangent in the first quadrant
  float bxy_a = b * x * y;
  if (bxy_a < 0.0f) bxy_a = -bxy_a;  // avoid fabs
  float num = bxy_a + y * y;
  float atan_1q = num / (x * x + bxy_a + num + 0.000001f);
  // Translate it to the proper quadrant
  uint32_t uatan_2q = (ux_s ^ uy_s) | *(uint32 *)&atan_1q;
  return q + *(float *)&uatan_2q;
}

static void HandleGamepadAxisInput(int gamepad_id, int axis, int value) {
  static int last_gamepad_id, last_x, last_y;
  if (axis == SDL_CONTROLLER_AXIS_LEFTX || axis == SDL_CONTROLLER_AXIS_LEFTY) {
    // ignore other gamepads unless they have a big input
    if (last_gamepad_id != gamepad_id) {
      if (value > -16000 && value < 16000)
        return;
      last_gamepad_id = gamepad_id;
      last_x = last_y = 0;
    }
    *(axis == SDL_CONTROLLER_AXIS_LEFTX ? &last_x : &last_y) = value;
    int buttons = 0;
    if (last_x * last_x + last_y * last_y >= 10000 * 10000) {
      // in the non deadzone part, divide the circle into eight 45 degree
      // segments rotated by 22.5 degrees that control which direction to move.
      // todo: do this without floats?
      static const uint8 kSegmentToButtons[8] = {
        1 << 4,           // 0 = up
        1 << 4 | 1 << 7,  // 1 = up, right
        1 << 7,           // 2 = right
        1 << 7 | 1 << 5,  // 3 = right, down
        1 << 5,           // 4 = down
        1 << 5 | 1 << 6,  // 5 = down, left
        1 << 6,           // 6 = left
        1 << 6 | 1 << 4,  // 7 = left, up
      };
      uint8 angle = (uint8)(int)(ApproximateAtan2(last_y, last_x) * 64.0f + 0.5f);
      buttons = kSegmentToButtons[(uint8)(angle + 16 + 64) >> 5];
    }
    /*
     * THE COMPANION GETS FIRST REFUSAL, exactly like the button ladder.
     *
     * The stick only ever became gameplay direction bits, so no companion
     * page ever saw it -- the v1.0.0 report that the joystick does nothing in
     * the companion screen.  The eight-way result is reduced to one edge per
     * direction and offered to the companion; when the companion owns the pad
     * it also swallows the movement, so Link cannot walk while the player is
     * driving a menu with the same stick.
     */
    {
      static int prev_buttons;
      int newly = buttons & ~prev_buttons;
      int dx = (newly & (1 << 7)) ? 1 : (newly & (1 << 6)) ? -1 : 0;
      int dy = (newly & (1 << 5)) ? 1 : (newly & (1 << 4)) ? -1 : 0;
      prev_buttons = buttons;
      if (AleksCompositor_StickNav(dx, dy)) {
        g_gamepad_buttons = 0;
        return;
      }
    }
    g_gamepad_buttons = buttons;
  } else if ((axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) {
    if (value < 12000 || value >= 16000)  // hysteresis
      HandleGamepadInput(axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? kGamepadBtn_L2 : kGamepadBtn_R2, value >= 12000);
  }
}

static bool LoadRom(const char *filename) {
  size_t length = 0;
  uint8 *file = ReadWholeFile(filename, &length);
  if(!file) Die("Failed to read file");
  bool result = EmuInitialize(file, length);
  free(file);
  return result;
}

static bool ParseLinkGraphics(uint8 *file, size_t length) {
  if (length < 27 || memcmp(file, "ZSPR", 4) != 0)
    return false;
  uint32 pixel_offs = DWORD(file[9]);
  uint32 pixel_length = WORD(file[13]);
  uint32 palette_offs = DWORD(file[15]);
  uint32 palette_length = WORD(file[19]);
  if ((uint64)pixel_offs + pixel_length > length ||
      (uint64)palette_offs + palette_length > length ||
      pixel_length != 0x7000)
    return false;
  if (kPalette_ArmorAndGloves_SIZE != 150 || kLinkGraphics_SIZE != 0x7000)
    Die("ParseLinkGraphics: Invalid asset sizes");
  memcpy(kLinkGraphics, file + pixel_offs, 0x7000);
  if (palette_length >= 120)
    memcpy(kPalette_ArmorAndGloves, file + palette_offs, 120);
  if (palette_length >= 124)
    memcpy(kGlovesColor, file + palette_offs + 120, 4);
  return true;
}

static void LoadLinkGraphics() {
  if (g_config.link_graphics) {
    fprintf(stderr, "Loading Link Graphics: %s\n", g_config.link_graphics);
    size_t length = 0;
    uint8 *file = ReadWholeFile(g_config.link_graphics, &length);
    if (file == NULL || !ParseLinkGraphics(file, length))
      Die("Unable to load file");
    free(file);
  }
}


const uint8 *g_asset_ptrs[kNumberOfAssets];
uint32 g_asset_sizes[kNumberOfAssets];

static void LoadAssets() {
  size_t length = 0;
  uint8 *data = ReadWholeFile("zelda3_assets.dat", &length);
  if (data)
    StartupLog("[BOOT 07] asset file opened: zelda3_assets.dat (%zu bytes)", length);
  if (!data) {
    size_t bps_length, bps_src_length;
    uint8 *bps, *bps_src;
    bps = ReadWholeFile("zelda3_assets.bps", &bps_length);
    if (!bps)
      Die("Failed to read zelda3_assets.dat. Please see the README for information about how you get this file.");
    bps_src = ReadWholeFile("zelda3.sfc", &bps_src_length);
    if (!bps_src)
      Die("Missing file: zelda3.sfc");
    data = ApplyBps(bps_src, bps_src_length, bps, bps_length, &length);
    if (!data)
      Die("Unable to apply zelda3_assets.bps. Please make sure you got the right version of 'zelda3.sfc'");
  }

  static const char kAssetsSig[] = { kAssets_Sig };

  if (length < 16 + 32 + 32 + 8 + kNumberOfAssets * 4) {
    StartupLog("Asset validation failed: truncated header (%zu bytes)", length);
    Die("Invalid assets file");
  }
  if (memcmp(data, kAssetsSig, 48) != 0) {
    StartupLog("Asset validation failed: signature mismatch");
    Die("Invalid assets file");
  }
  if (*(uint32*)(data + 80) != kNumberOfAssets) {
    StartupLog("Asset validation failed: table count %u, expected %u",
               *(uint32*)(data + 80), kNumberOfAssets);
    Die("Invalid assets file");
  }

  StartupLog("[BOOT 08] asset validation done");

  uint32 offset = 88 + kNumberOfAssets * 4 + *(uint32 *)(data + 84);

  for (size_t i = 0; i < kNumberOfAssets; i++) {
    uint32 size = *(uint32 *)(data + 88 + i * 4);
    offset = (offset + 3) & ~3;
    if ((uint64)offset + size > length) {
      StartupLog("Asset validation failed: entry %zu exceeds file", i);
      Die("Assets file corruption");
    }
    g_asset_sizes[i] = size;
    g_asset_ptrs[i] = data + offset;
    offset += size;
  }

  if (g_config.features0 & kFeatures0_DimFlashes) { // patch dungeon floor palettes
    kPalette_DungBgMain[0x484] = 0x70;
    kPalette_DungBgMain[0x485] = 0x95;
    kPalette_DungBgMain[0x486] = 0x57;
  }
}

// Go some steps up and find zelda3.ini
static void SwitchDirectory() {
#ifdef __SWITCH__
  if (chdir(kSwitchRuntimeRoot) == 0) {
    StartupLog("Switch runtime root selected: %s", kSwitchRuntimeRoot);
    StartupLogCurrentDirectory();
    return;
  }
  StartupLog("Unable to chdir to canonical Switch runtime root: %s", kSwitchRuntimeRoot);
#endif
  char buf[4096];
  if (!getcwd(buf, sizeof(buf) - 32))
    return;
  size_t pos = strlen(buf);

  for (int step = 0; pos != 0 && step < 3; step++) {
    memcpy(buf + pos, "/zelda3.ini", 12);
    FILE *f = fopen(buf, "rb");
    if (f) {
      fclose(f);
      buf[pos] = 0;
      if (step != 0) {
        printf("Found zelda3.ini in %s\n", buf);
        int err = chdir(buf);
        if (err != 0)
          StartupLog("Unable to chdir to discovered config directory: %s", buf);
      }
      StartupLogCurrentDirectory();
      return;
    }
    pos--;
    while (pos != 0 && buf[pos] != '/' && buf[pos] != '\\')
      pos--;
  }
}

MemBlk FindInAssetArray(int asset, int idx) {
  return FindIndexInMemblk((MemBlk) { g_asset_ptrs[asset], g_asset_sizes[asset] }, idx);
}
