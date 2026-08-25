#pragma once
#include "types.h"
#include <SDL_keycode.h>

enum {
  kKeys_Null,
  kKeys_Controls,
  kKeys_Controls_Last = kKeys_Controls + 11,
  kKeys_Load,
  kKeys_Load_Last = kKeys_Load + 19,
  kKeys_Save,
  kKeys_Save_Last = kKeys_Save + 19,
  kKeys_Replay,
  kKeys_Replay_Last = kKeys_Replay + 19,
  kKeys_LoadRef,
  kKeys_LoadRef_Last = kKeys_LoadRef + 19,
  kKeys_ReplayRef,
  kKeys_ReplayRef_Last = kKeys_ReplayRef + 19,
  kKeys_CheatLife,
  kKeys_CheatKeys,
  kKeys_CheatEquipment,
  kKeys_CheatWalkThroughWalls,
  kKeys_ClearKeyLog,
  kKeys_StopReplay,
  kKeys_Fullscreen,
  kKeys_Reset,
  kKeys_Pause,
  kKeys_PauseDimmed,
  kKeys_Turbo,
  kKeys_ReplayTurbo,
  kKeys_WindowBigger,
  kKeys_WindowSmaller,
  kKeys_DisplayPerf,
  kKeys_ToggleRenderer,
  kKeys_VolumeUp,
  kKeys_VolumeDown,
  kKeys_Total,
};

enum {
  kOutputMethod_SDL,
  kOutputMethod_SDLSoftware,
  kOutputMethod_OpenGL,
  kOutputMethod_OpenGL_ES,
};

/*
 * GAMEPLAY ASPECT -- the one authority for how wide a frame the engine draws.
 *
 * Everything downstream (PPU extraLeftRight, g_snes_width, the source rect,
 * both cameras, the spotlight tables, the layout, the crash journal) derives
 * from AleksAspect_ExtraSide(), so "how much side space does this aspect
 * mean" is answered in exactly one place instead of being spelled 71 in
 * several.
 *
 *   4:3        0 extra   256 wide  -- untouched original geometry
 *   WIDE      71 extra   398 wide  -- maximum extra world; 16:9 computed on
 *                                     SQUARE pixels, so after the 7:6 PAR
 *                                     correction it is WIDER than 16:9 and
 *                                     keeps a small letterbox.  Unchanged.
 *   TRUE 16:9 EXPANDED  55 extra  366x240 -- 16:9 with the PAR *and* the
 *                                     engine's 240-line mode:
 *                                     366*7 : 240*6 = 1.77917 vs 1.77778.
 *                                     More world horizontally AND vertically.
 *   TRUE 16:9 43 extra   342 wide  -- 16:9 computed WITH the PAR:
 *                                     (256+2e)*7 / (224*6) = 16/9
 *                                     => 256+2e = 341.3 => e = 42.7 -> 43.
 *                                     342*7:224*6 = 1.7813 against 1.7778,
 *                                     under one pixel of bar at 720p.
 */
enum {
  kAleksAspect_4x3 = 0,
  kAleksAspect_Wide = 1,
  kAleksAspect_True169 = 2,
  kAleksAspect_True169Expanded = 3,
  kAleksAspect_Count,
};
int AleksAspect_ExtraSide(int aspect);       /* side space per side, clamped */
bool AleksAspect_Height240(int aspect);      /* 240 rendered lines instead of 224 */
int AleksAspect_FromExtraSide(int extra);    /* the inverse, for revert paths */
const char *AleksAspect_IniValue(int aspect);
const char *AleksAspect_Label(int aspect);
int AleksAspect_FromIni(const char *value);  /* -1 when unrecognised */
/* The aspects the settings row offers.  WIDE parses and runs but is not in
 * here, so it cannot be selected -- see the note in config.c. */
extern const uint8 kAleksAspectCycle[3];
int AleksAspect_NextInCycle(int from, int dir);

/*
 * The controls an ALEKS shortcut or a Quick Item may be bound to.
 *
 * ONE table, shared by the settings UI that offers the choices and the input
 * layer that resolves them -- they used to be two copies in two files, which
 * is a silent mismatch waiting to happen the moment one gains an entry.
 * Index 0 is OFF; note kGamepadBtn_A is also 0, so every user of this table
 * must treat index <= 0 as "unbound" before indexing.
 */
extern const uint8 kAleksShortcutButtons[];
extern const int kAleksShortcutButtonCount;

// ALEKS shortcut actions.  Index into Config.aleks_shortcut.
enum {
  kAleksShortcut_Companion,
  kAleksShortcut_Settings,
  kAleksShortcut_NextPage,
  kAleksShortcut_QuickSave,
  kAleksShortcut_QuickLoad,
  kAleksShortcut_DisplayMode,
  kAleksShortcut_Count,
};

typedef struct Config {
  int window_width;
  int window_height;
  bool enhanced_mode7;
  bool new_renderer;
  bool ignore_aspect_ratio;
  uint8 fullscreen;
  uint8 window_scale;
  bool enable_audio;
  bool linear_filtering;
  uint8 output_method;
  uint16 audio_freq;
  uint8 audio_channels;
  uint16 audio_samples;
  bool autosave;
  uint8 extended_aspect_ratio;
  bool extend_y;
  bool no_sprite_limits;
  bool display_perf_title;
  uint8 enable_msu;
  bool resume_msu;
  bool disable_frame_delay;
  uint8 aleks_display_mode;
  uint8 aleks_companion_page;
  bool aleks_companion_hud;
  bool aleks_pixel_perfect;
  uint8 aleks_gameplay_aspect;
  uint8 aleks_wide_camera;   // 0 = STANDARD, 1 = FIXED (Esteban)
  uint8 aleks_hud_mode;      // 0 = AUTO, 1 = ON, 2 = OFF
  uint8 aleks_companion_layout;  // 0 = CLASSIC, 1 = TALL
  uint8 aleks_screen_order;  // 0 = game first, 1 = companion first
  bool aleks_x_item_ring;    // second ring assigning the X item
  uint8 aleks_story_guide;   // StoryGuideDetail (0 = OFF)
  // ALEKS shortcuts, one physical control per action (0 = OFF).
  // Same model as the final TMC's ezlo_shortcut: a small index that the
  // input layer resolves, not a raw button id.
  uint8 aleks_shortcut[6];
  /* Save-state quick slot, stored as a 0-based PICKER index (the same index
   * the SAVE STATES cards use), never as a backend slot number -- backend
   * slot 0 is the autosave and 1 is reserved.  The UI shows this + 1. */
  uint8 aleks_quick_slot;
  uint8 aleks_ra_enable;     // RetroAchievements, off until asked for
  /* Hold the dialogue-advance button instead of tapping per page.  OFF keeps
   * vanilla timing exactly, which matters for timed cutscenes. */
  uint8 aleks_hold_to_advance;
  /* Quick Items: a 1..20 inventory-grid slot (0 = unassigned) and the
   * physical control that triggers it (index into the shortcut button list,
   * 0 = OFF).  The grid index is exactly what SS_EquipSlot takes. */
  uint8 aleks_quick_item_slot[2];
  uint8 aleks_quick_item_button[2];
  uint8 aleks_scaling_mode;
  uint8 aleks_dual_game_scale, aleks_dual_companion_scale, aleks_dual_gap;
  uint8 aleks_flip_game_scale, aleks_flip_companion_scale, aleks_flip_gap;
  bool aleks_touch_ui, aleks_tap_equip;
  uint8 msuvolume;
  uint32 features0;

  const char *link_graphics;
  char *memory_buffer;
  const char *shader;
  const char *msu_path;
  const char *language;
} Config;

enum {
  kMsuEnabled_Msu = 1,
  kMsuEnabled_MsuDeluxe = 2,
  kMsuEnabled_Opuz = 4,
};
enum {
  kGamepadBtn_Invalid = -1,
  kGamepadBtn_A,
  kGamepadBtn_B,
  kGamepadBtn_X,
  kGamepadBtn_Y,
  kGamepadBtn_Back,
  kGamepadBtn_Guide,
  kGamepadBtn_Start,
  kGamepadBtn_L3,
  kGamepadBtn_R3,
  kGamepadBtn_L1,
  kGamepadBtn_R1,
  kGamepadBtn_DpadUp,
  kGamepadBtn_DpadDown,
  kGamepadBtn_DpadLeft,
  kGamepadBtn_DpadRight,
  kGamepadBtn_L2,
  kGamepadBtn_R2,
  kGamepadBtn_Count,
};

extern Config g_config;

void ParseConfigFile(const char *filename);
bool Config_SaveAleksDisplaySettings(void);
void Config_SetDefaults(void);
bool Config_WriteDefaultIni(const char *path);
void GamepadMap_GetControls(uint8 *out12);
/* Factory binding for one command, used to repair an unbound one. */
uint8 GamepadMap_DefaultForCmd(int cmd);
void GamepadMap_SetControls(const uint8 *btns12);
void GamepadMap_ResetControls(void);
int FindCmdForSdlKey(SDL_Keycode code, SDL_Keymod mod);
int FindCmdForGamepadButton(int button, uint32 modifiers);
