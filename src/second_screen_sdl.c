// SDL second screen: Linux/desktop frontend, compiled only in the make
// build (the Android build uses second_screen_sdl_stub.c instead).
// SDL version of the second screen (MinimapView.java) for dual-screen linux
// handhelds: map with follow-cam, dungeon automap, touch items/gear/settings,
// all drawn from art generated out of zelda3_assets.dat (see second_screen.c).
//
// Enabled with ZELDA3_SECOND_SCREEN=1. The window opens fullscreen on the
// second display when there is one; ZELDA3_SECOND_SCREEN_DISPLAY=n picks a
// display and ZELDA3_SECOND_SCREEN_TITLE overrides the window title for
// systems that route windows by title. Software renderer so it can't fight
// the game's GL context.
#include <SDL.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __3DS__
#include <3ds.h>
#include "platform_3ds.h"
#else
enum Platform3DSDisplayMode {
  kPlatform3DSDisplayOriginal,
  kPlatform3DSDisplayUltraWideMod,
  kPlatform3DSDisplayStretch,
};

enum Platform3DSWideEdgeMode {
  kPlatform3DSWideEdgeStandard,
  kPlatform3DSWideEdgeFixedCamera,
};

enum Platform3DSCStickMode {
  kPlatform3DSCStickTurbo,
  kPlatform3DSCStickWalk,
  kPlatform3DSCStickDisabled,
};
#endif

#include "types.h"                 // uint8/uint16 for the tables header
#include "second_screen_tables.h"  // kIconCount/kIconCols/kGlyphCount/kGlyphCols
#include "ss_sheets.h"             // generated cell indices for icons/glyphs/letters
#include "ss_textures.h"           // baked theme background tiles (menu/parchment/stone)

#ifdef __SWITCH__
// On Switch this file keeps its own 320x240 logical surface -- the same one
// the 3DS build draws for the bottom screen -- but renders it into a texture
// on the game's single renderer instead of owning a second window.  Where
// the finished surface lands is aleks_layout.c / aleks_compositor.c, both
// adapted from the final ALEKS TMC Switch worktree.
#include "config.h"
#include "features.h"
#include "aleks_layout.h"
#include "story_guide.h"
#include "aleks_crashctx.h"
#include "aleks_ra.h"
#include "aleks_lang.h"
#endif

#ifndef ZELDA3_3DS_VERSION
#define ZELDA3_3DS_VERSION "dev"
#endif

// API provided by second_screen.c
int  SS_GetLinkX(void);
int  SS_GetLinkY(void);
int  SS_GetArea(void);
int  SS_GetModule(void);
bool SS_IsIndoors(void);
void SS_ReadSram(uint8_t *out, int n);
int  SS_GetEquippedSlot(void);
int  SS_GetEquippedSlotX(void);
void SS_AssignSlotX(int slot);
int  SS_GetDungeon(void);
bool SS_GetIndoorExit(int *out);
void SS_ReadDungFlags(uint8_t *out, int n);
bool SS_RenderIconSheet(uint32_t *px);
bool SS_RenderGlyphSheet(uint32_t *px);
bool SS_RenderLetterSheet(uint32_t *px);
bool SS_RenderWorldMap(uint32_t *px, bool dark);
bool SS_RenderLinkFace(uint32_t *px, int chunk);
int  SS_GetDungeonLayout(int palace, uint8_t *out, int cap);
bool SS_RenderDungeonFloor(int palace, int floorIdx, uint32_t *px);
bool SS_RenderMapIcons(int palace, uint32_t *px);
bool SS_EquipSlot(int slot);
bool SS_CanEquipSlot(int slot);
/* aleks_compositor.c: keep an interactive companion session visible when the
 * display mode changes out from under it. */
void AleksCompositor_NotifyDisplayModeChanged(int old_mode, int new_mode);
void SS_SetWidescreen(bool on);
bool SS_IsWidescreen(void);
void SS_SetAspect(int aspect);
int  SS_GetAspect(void);
void SS_Set3DSDisplayMode(int mode);
void SS_Set3DSWideEdgeMode(int mode);
void SS_SetHudHidden(bool hide);
bool SS_IsHudHidden(void);
void SS_RequestMemoryDump(const char *dump_dir);
void SS_RequestRestart(void);
void SS_ArmButtonCapture(bool arm);
int  SS_GetCapturedButton(void);
void SS_GetGamepadControls(int *out12);
void SS_SetGamepadControls(const int *in12);
unsigned SS_GetFeatures(void);
void SS_SetFeature(unsigned mask, bool on);
void SS_SetWideEdgeMode(int mode);
int  SS_GetWideEdgeMode(void);
void SS_RequestSaveState(int slot);
void SS_RequestLoadState(int slot);
bool SS_TakeThumbnail(uint32_t *out);
void SS_SetAutosave(bool on);
int  SS_GetLinkFacing(void);
bool SS_RenderLinkFacingMarker(uint32_t *px, int facing);

// palette (MinimapView)
#define COL(r,g,b) (0xff000000u | ((r) << 16) | ((g) << 8) | (b))
enum {
  COL_GOLD        = COL(232, 194, 96),
  COL_GOLD_DARK   = COL(122, 88, 30),
  COL_OUTLINE     = COL(30, 22, 10),
  COL_BOX         = COL(12, 12, 12),
  COL_BOX_BORDER  = COL(96, 200, 120),
  COL_BOX_BORDER2 = COL(224, 176, 60),
  COL_STONE_EDGE_L= COL(134, 142, 158),
  COL_STONE_EDGE_D= COL(44, 50, 62),
  COL_STONE_INSET = COL(38, 44, 56),
  COL_PLAQUE      = COL(88, 96, 112),
  COL_PLAQUE_SEL  = COL(58, 108, 196),
  COL_BG_MENU     = COL(24, 28, 22),   // stand-ins for the tiled theme textures
  COL_BG_STONE    = COL(52, 58, 70),
  COL_BG_PARCH    = COL(214, 188, 138),
};

/* TAB_SAVE is a top-level page like MAP/ITEMS/GEAR, but it draws with the
 * settings row machinery (menu SW_STATES) -- see sw_current_menu(). */
enum { TAB_MAP, TAB_ITEMS, TAB_GEAR, TAB_SETTINGS, TAB_GUIDE, TAB_SAVE, TAB_COUNT };
enum { MODE_GAME, MODE_TITLE, MODE_CINEMA };

typedef struct { float x, y, w, h; } RectFS;

// constants ported from MinimapView
static const char *const kItemNames[20] = {
  "bow", "boomerang", "hookshot", "bombs", "mushroom",
  "firerod", "icerod", "bombos", "ether", "quake",
  "torch", "hammer", "flute", "bugnet", "book",
  "bottle", "somaria", "byrna", "cape", "mirror",
};
static const int kPendantMarks[3][3] = {   // {bit, x, y}
  {4, 3928, 1600},   // Courage - Eastern Palace
  {2, 296, 3248},    // Power   - Desert Palace
  {1, 2160, 320},    // Wisdom  - Tower of Hera
};
static const int kCrystalMarks[7][3] = {
  {2, 3960, 1600},   // Palace of Darkness
  {16, 1888, 3776},  // Swamp Palace
  {64, 208, 320},    // Skull Woods
  {32, 384, 1888},   // Thieves' Town
  {4, 3168, 3660},   // Ice Palace
  {1, 320, 3376},    // Misery Mire
  {8, 3800, 256},    // Turtle Rock
};
static const char *const kDungeonNames[14] = {
  "SEWERS", "HYRULE CASTLE", "EASTERN PALACE", "DESERT PALACE", "CASTLE TOWER",
  "SWAMP PALACE", "DARK PALACE", "MISERY MIRE", "SKULL WOODS", "ICE PALACE",
  "TOWER OF HERA", "THIEVES TOWN", "TURTLE ROCK", "GANONS TOWER",
};
static const int kDungeonBoss[14]    = {15, 15, 200, 51, 32, 6, 90, 144, 41, 222, 7, 172, 164, 13};
static const int kDungeonBossPos[14] = {   // x<<8|y of the skull inside its room (kDungMap_Tab37)
  -1, -1, 0x808, 8, 0, 8, 0x808, 8, 0x808, 0x800, 0x404, 0x808, 8, 8,
};
static const int kDotPalette[4] = {0, 1, 2, 1};  // marker blink cycle (kDungMap_Tab38)

// joypad command names + gamepad button names, in the game's orders
static const char *const kPadCmdNames[12] = {
  "UP", "DOWN", "LEFT", "RIGHT", "SELECT", "START", "A", "B", "X", "Y", "L", "R",
};
static const char *const kPadButtonLabel[17] = {
  "A", "B", "X", "Y", "BACK", "GUIDE", "START", "L3", "R3",
  "L1", "R1", "D UP", "D DOWN", "D LEFT", "D RIGHT", "L2", "R2",
};
static const char *const kPadButtonIni[17] = {
  "A", "B", "X", "Y", "Back", "Guide", "Start", "L3", "R3",
  "L1", "R1", "DpadUp", "DpadDown", "DpadLeft", "DpadRight", "L2", "R2",
};

// state
static SDL_Window   *ss_win;
static SDL_Renderer *ss_r;
static uint32_t      ss_winid;
static int           W, H;
static float         u = 1.0f;

static SDL_Texture *tex_map[2], *tex_icons, *tex_glyphs, *tex_letters, *tex_face;
static SDL_Texture *tex_floor, *tex_mapicons;
static SDL_Texture *tex_bg_menu, *tex_bg_parch, *tex_bg_stone;
static bool art_ready;

typedef struct {
  const char *name;
  int boss, floors, basements;
  uint8_t layout[16][25];
} Dungeon;
static Dungeon dungeons[14];

static int  tab = TAB_MAP;
static bool whole_map;
static int  tap_flash_slot = -1;
static uint32_t tap_flash_until;
static int  view_floor_offset;
static uint32_t view_floor_touched_at;

static bool has_last_outdoor;
static int  last_out_x, last_out_y, last_out_area;

static uint8_t sram[256];
static uint8_t dung_flags[0x500];

static void rebuild_renderer(int w2, int h2);
// set on SIZE_CHANGED, handled at the top of the next Update
static bool ss_needs_rebuild;

// touch rects recomputed every draw, used by the tap handler
static RectFS map_area_r, tab_items_r, tab_gear_r, tab_map_r, tab_settings_r, y_ring_r;
#ifdef __SWITCH__
static RectFS tab_guide_r, tab_save_r, x_ring_r;
/* 0 = none, 1 = Y ring armed, 2 = X ring armed (donor gesture). */
static int armed_ring;
#endif
static RectFS settings_row_r[6], remap_row_r[6], remap_back_r;
static RectFS remap_page_r;
static RectFS screen_row_r[4], screen_back_r;
#ifdef __SWITCH__
/* ------------------------------------------------------------------ *
 * Switch settings menu state.
 *
 * The 3DS settings screen is three flat panels of platform controls that do
 * not exist here, so the rows differ -- but the presentation does not: every
 * row below is drawn with the donor's own menu_box / draw_settings_row /
 * draw_text / chevron primitives.
 *
 * ONE row table per menu feeds both the painter and the tap handler, so the
 * controller and the touchscreen can never drift into two behaviours (the
 * same rule the final TMC panel follows).
 * ------------------------------------------------------------------ */
enum {
  SW_ROOT, SW_SCREEN, SW_DUAL, SW_FLIP, SW_GAMEPLAY,
  SW_RETRO,
  SW_QOL, SW_ADVANCED, SW_CONTROLS, SW_SHORTCUTS,
  SW_AUDIO, SW_SYSTEM, SW_STATES,
};

#define SW_MAX_ROWS 20
#define SW_MAX_VISIBLE 6

typedef struct SwRow {
  const char *label;
  const char *value;   /* NULL = no value column */
  bool submenu;        /* draw a chevron instead of a value */
} SwRow;

static uint8 sw_stack[6] = { SW_ROOT };
static int sw_depth;                 /* index into sw_stack */
static int sw_sel = -1;              /* cursor row; -1 until the pad is used */
static int sw_scroll;                /* first visible row */
static int sw_row_count, sw_visible;
static RectFS sw_row_r[SW_MAX_VISIBLE], sw_back_r, sw_up_r, sw_down_r;
static SwRow sw_rows[SW_MAX_ROWS];
static char sw_value_buf[SW_MAX_ROWS][20];

/* ---- map pins ----------------------------------------------------------
 * Ported from the donor's Android companion (MinimapView.java: MAX_PINS,
 * togglePin, loadPins/savePins, PIN_ART).  Same 20-pin cap, same per-world
 * split, same "world,x,y per line" file, same pennant art -- only the drawing
 * primitives and the storage path change, because this is SDL on sdmc:.
 *
 * The 3DS/SDL donor UI never got pins (they exist only in its Android view),
 * so this is a port across renderers rather than a copy.
 * ------------------------------------------------------------------ */
#define SW_MAX_PINS 20
static uint8 pin_dark[SW_MAX_PINS];
static int pin_x[SW_MAX_PINS], pin_y[SW_MAX_PINS];
static int pin_count;
static bool pins_loaded;

/* Thumbnail geometry, matching second_screen.c's capture (kSsThumb*). */
#define kSsThumbW 128
#define kSsThumbH 112

/* draw_overworld runs before these are defined; the definitions stay next to
 * the rest of the pin code rather than being hoisted up here. */
static void pins_load(void);
static void draw_pin(float x, float y, float s);

/* Live overworld mapping, published by draw_overworld so the tap handler can
 * turn a screen point into world coordinates using the SAME numbers that were
 * drawn -- never a second copy of the projection. */
static bool map_live;
static bool map_dark;
static float map_ox, map_oy, map_scale;
static RectFS map_view_r, map_zoom_r;

static void ini_set_int(const char *key, int value);
/* The 12 joypad bindings, as gamepad button ids (-1 = unbound).  Defined
 * with the rest of the donor's state further down. */
static int pad_controls[12];

/* ---- CONTROLS -----------------------------------------------------------
 * Button capture ("PRESS A BUTTON") did not work reliably on hardware, so the
 * rows are edited deterministically instead: LEFT/RIGHT step through the
 * allowed physical controls and apply immediately.
 *
 * LABELS ARE PHYSICAL, NOT SDL.  SDL names buttons positionally after the Xbox
 * layout, so SDL "A" is the BOTTOM button -- which on a Switch pad is
 * physically B.  Showing the SDL name would tell the player the opposite of
 * what is printed on their controller, so kSwitchButtonLabel maps every
 * kGamepadBtn_* to the Nintendo label.
 */
static const char *const kSwitchButtonLabel[kGamepadBtn_Count] = {
  [kGamepadBtn_A]         = "B",        /* SDL A  = bottom = Switch B */
  [kGamepadBtn_B]         = "A",        /* SDL B  = right  = Switch A */
  [kGamepadBtn_X]         = "Y",        /* SDL X  = left   = Switch Y */
  [kGamepadBtn_Y]         = "X",        /* SDL Y  = top    = Switch X */
  [kGamepadBtn_Back]      = "MINUS",
  [kGamepadBtn_Guide]     = "HOME",
  [kGamepadBtn_Start]     = "PLUS",
  /* "L STICK"/"R STICK" read as the same word at a glance in this font and
   * were reported as duplicated entries; the stick presses are printed L3/R3
   * on nothing, but they are what every Switch player calls them. */
  [kGamepadBtn_L3]        = "L3",
  [kGamepadBtn_R3]        = "R3",
  [kGamepadBtn_L1]        = "L",
  [kGamepadBtn_R1]        = "R",
  [kGamepadBtn_DpadUp]    = "D UP",
  [kGamepadBtn_DpadDown]  = "D DOWN",
  [kGamepadBtn_DpadLeft]  = "D LEFT",
  [kGamepadBtn_DpadRight] = "D RIGHT",
  [kGamepadBtn_L2]        = "ZL",
  [kGamepadBtn_R2]        = "ZR",
};

/* The controls a Zelda command may be bound to.  HOME is omitted: the system
 * owns it.  ZL/ZR are included because this build already routes the analog
 * triggers through kGamepadBtn_L2/R2 (main.c, SDL_CONTROLLERAXISMOTION). */
static const uint8 kBindableButtons[] = {
  kGamepadBtn_B, kGamepadBtn_A, kGamepadBtn_Y, kGamepadBtn_X,
  kGamepadBtn_L1, kGamepadBtn_R1, kGamepadBtn_L2, kGamepadBtn_R2,
  kGamepadBtn_L3, kGamepadBtn_R3, kGamepadBtn_Start, kGamepadBtn_Back,
  kGamepadBtn_DpadUp, kGamepadBtn_DpadDown,
  kGamepadBtn_DpadLeft, kGamepadBtn_DpadRight,
};
#define SW_BINDABLE_COUNT ((int)(sizeof(kBindableButtons) / sizeof(kBindableButtons[0])))

static const char *sw_button_label(int b) {
  if (b < 0 || b >= kGamepadBtn_Count || !kSwitchButtonLabel[b])
    return "----";
  return kSwitchButtonLabel[b];
}

static int sw_bindable_index(int button) {
  for (int i = 0; i < SW_BINDABLE_COUNT; i++)
    if (kBindableButtons[i] == button) return i;
  return -1;
}

/* A queued binding change is waiting to be written back to the ini. */
static bool sw_controls_dirty;

/*
 * Rebind one command, swapping rather than rejecting on a conflict.
 *
 * The mapper resolves one command per button, so leaving two commands on the
 * same control would silently strand one of them.  Handing the displaced
 * command the button this one just gave up keeps every command bound and is
 * what a player expects when they move A onto B's button.
 */
/*
 * Repair anything that is not a real binding.
 *
 * Called before and after every rebind.  This is the invariant that hardware
 * proved was missing: a command with no button is unusable, it survives into
 * the ini as an empty field, and the next launch reads it back and spreads it
 * further.  Nothing downstream is allowed to see -1.
 */
static void sw_sanitize_bindings(void) {
  for (int i = 0; i < 12; i++) {
    if (pad_controls[i] < 0 || pad_controls[i] >= kGamepadBtn_Count) {
      pad_controls[i] = GamepadMap_DefaultForCmd(i);
      StartupLog("CONTROLS: command %d was unbound, restored to default", i);
    }
  }
}

/*
 * Rebind one command, swapping rather than rejecting on a conflict.
 *
 * The mapper resolves one command per button, so leaving two commands on the
 * same control would silently strand one of them.  Handing the displaced
 * command the button this one gave up keeps every command bound -- and that
 * only holds if the button being given up is a real one, which is what the
 * sanitize call guarantees.  The old version handed over -1 unchecked, and
 * that is how a controller ended up with three dead commands.
 */
static void sw_set_binding(int cmd, int button) {
  int previous;
  if (cmd < 0 || cmd >= 12) return;
  if (button < 0 || button >= kGamepadBtn_Count) return;

  sw_sanitize_bindings();
  previous = pad_controls[cmd];
  if (previous == button) return;            /* nothing to do */

  for (int i = 0; i < 12; i++) {
    if (i != cmd && pad_controls[i] == button)
      pad_controls[i] = previous;            /* a real button, never -1 */
  }
  pad_controls[cmd] = button;
  sw_sanitize_bindings();                    /* belt and braces */
  SS_SetGamepadControls(pad_controls);
  sw_controls_dirty = true;
}

static void sw_cycle_binding(int cmd, int dir) {
  int at = sw_bindable_index(pad_controls[cmd]);
  if (at < 0) at = 0;
  at = (at + (dir < 0 ? SW_BINDABLE_COUNT - 1 : 1)) % SW_BINDABLE_COUNT;
  sw_set_binding(cmd, kBindableButtons[at]);
}

/* ---- ALEKS shortcuts ----------------------------------------------------
 * The final TMC model, adapted: one small index per action which the input
 * layer resolves to a physical control, cycled with LEFT/RIGHT and persisted
 * on change (port_runtime_config.cpp: ezlo_shortcut / CycleEzloShortcut).
 *
 * Controls already bound to a Zelda command are skipped while cycling, so a
 * shortcut can never fight gameplay input -- the conflict is made impossible
 * rather than merely reported.
 */
#define kShortcutButtons kAleksShortcutButtons      /* the shared table */
#define SW_SHORTCUT_CHOICES kAleksShortcutButtonCount

static const char *const kShortcutActionName[kAleksShortcut_Count] = {
  "OPEN COMPANION", "OPEN SETTINGS", "NEXT PAGE",
  "QUICK SAVE", "QUICK LOAD", "DISPLAY MODE",
};

static bool sw_button_used_by_gameplay(int button) {
  for (int i = 0; i < 12; i++)
    if (pad_controls[i] == button) return true;
  return false;
}

static const char *sw_shortcut_label(int slot) {
  int choice = g_config.aleks_shortcut[slot];
  if (choice <= 0 || choice >= SW_SHORTCUT_CHOICES) return "OFF";
  return sw_button_label(kShortcutButtons[choice]);
}

/*
 * Is this shortcut-button choice already spoken for?
 *
 * ONE definition, consulted by the shortcut rows and the Quick Item rows
 * alike -- otherwise adding Quick Items would have quietly reintroduced the
 * very conflict the exclusion-while-cycling rule exists to prevent, since the
 * old loop only knew about aleks_shortcut[].
 *
 * `except_shortcut` / `except_quick_item` are the row being edited (-1 for
 * none), which must not count as a conflict with itself.
 */
static bool sw_choice_taken(int choice, int except_shortcut, int except_quick_item) {
  if (choice <= 0) return false;                        /* OFF is free */
  if (sw_button_used_by_gameplay(kShortcutButtons[choice])) return true;
  for (int i = 0; i < kAleksShortcut_Count; i++)
    if (i != except_shortcut && g_config.aleks_shortcut[i] == choice) return true;
  for (int i = 0; i < 2; i++)
    if (i != except_quick_item && g_config.aleks_quick_item_button[i] == choice) return true;
  return false;
}

static void sw_cycle_shortcut(int slot, int dir) {
  int choice = g_config.aleks_shortcut[slot];
  for (int step = 0; step < SW_SHORTCUT_CHOICES; step++) {
    choice = (choice + (dir < 0 ? SW_SHORTCUT_CHOICES - 1 : 1)) % SW_SHORTCUT_CHOICES;
    if (choice == 0) break;                       /* OFF is always offered */
    if (!sw_choice_taken(choice, slot, -1)) break;
  }
  g_config.aleks_shortcut[slot] = (uint8)choice;
  {
    char key[32];
    snprintf(key, sizeof(key), "AleksShortcut%d", slot);
    ini_set_int(key, choice);
  }
}

/* Quick Item rows: same exclusion rule, same button list. */
static void sw_cycle_quick_item_button(int idx, int dir) {
  int choice = g_config.aleks_quick_item_button[idx];
  for (int step = 0; step < SW_SHORTCUT_CHOICES; step++) {
    choice = (choice + (dir < 0 ? SW_SHORTCUT_CHOICES - 1 : 1)) % SW_SHORTCUT_CHOICES;
    if (choice == 0) break;
    if (!sw_choice_taken(choice, -1, idx)) break;
  }
  g_config.aleks_quick_item_button[idx] = (uint8)choice;
  {
    char key[32];
    snprintf(key, sizeof(key), "AleksQuickItemButton%d", idx);
    ini_set_int(key, choice);
  }
}

/* 0 = unassigned, else a 1..20 inventory-grid slot (kItemNames order). */
static void sw_cycle_quick_item_slot(int idx, int dir) {
  int slot = g_config.aleks_quick_item_slot[idx];
  slot += dir < 0 ? -1 : 1;
  if (slot < 0) slot = 20;
  if (slot > 20) slot = 0;
  g_config.aleks_quick_item_slot[idx] = (uint8)slot;
  {
    char key[32];
    snprintf(key, sizeof(key), "AleksQuickItemSlot%d", idx);
    ini_set_int(key, slot);
  }
}

static const char *sw_quick_item_label(int idx) {
  int slot = g_config.aleks_quick_item_slot[idx];
  if (slot < 1 || slot > 20) return "OFF";
  return kItemNames[slot - 1];
}

static const char *sw_quick_item_button_label(int idx) {
  int choice = g_config.aleks_quick_item_button[idx];
  if (choice <= 0 || choice >= SW_SHORTCUT_CHOICES) return "OFF";
  return sw_button_label(kShortcutButtons[choice]);
}

/* SAVE STATES: donor slot numbering -- backend slot 0 is the autosave and
 * slot 1 is reserved, so the picker starts at 2 (MinimapView.STATE_SLOT0).
 *
 * PICKER INDEX vs BACKEND SLOT.  Everything the player sees is a 0-based
 * picker index; the engine's SaveLoadSlot() names files saves/save<N>.sav by
 * BACKEND slot.  The two are not the same number and mixing them up is not
 * cosmetic: passing a picker index straight to the backend wrote save0.sav --
 * the autosave -- while the card still read its state from save2.sav, so the
 * slot showed EMPTY and the autosave was destroyed.  Every backend call goes
 * through sw_backend_slot(); nothing else may add the offset itself. */
#define SW_STATE_SLOT0 2
#define SW_STATE_SLOTS 4
static SDL_Texture *sw_state_thumb[SW_STATE_SLOTS];
static long sw_state_stamp[SW_STATE_SLOTS];

static int sw_backend_slot(int picker_index) {
  return SW_STATE_SLOT0 + picker_index;
}

/* ---- save-state confirmation modal -------------------------------------
 * One modal backend, shared by the SAVE STATES page and the Quick Save
 * shortcut.  SAVE is always confirmed; LOAD never is (see sw_state_load).
 * While this is up it owns all navigation input -- the page underneath must
 * not move and gameplay must not see the presses (aleks_compositor.c). */
static bool sw_confirm_active;
/* The modal serves more than save states now.  ONE modal with a kind, rather
 * than a second one that could fight it for input ownership. */
enum { kConfirm_Save = 0, kConfirm_LanguageRestart = 1 };
static int  sw_confirm_kind;
static int  sw_confirm_slot;           /* picker index */
static bool sw_confirm_overwrite;      /* wording only */
static bool sw_confirm_quick;          /* armed by the Quick Save shortcut */
static int  sw_confirm_choice;         /* 0 = NO (default), 1 = YES */
/* What to restore when a quick-save modal resolves, so the shortcut returns
 * the player exactly where they were. */
static int  sw_confirm_restore_tab = -1;
static bool sw_confirm_close_overlay;
/* Filled by the painter, hit-tested by the tap handler. */
static RectFS sw_confirm_no_r, sw_confirm_yes_r;
bool SecondScreenSDL_ConfirmCommit(void);
bool SecondScreenSDL_ConfirmCancel(void);
void SecondScreenSDL_SetTab(int page);
static int sw_thumb_want = -1;       /* a save waiting for its frame grab */
static int sw_state_flash = -1;
static uint32_t sw_state_flash_until;
/* What the flashing slot says.  An empty slot that was asked to LOAD must not
 * report DONE -- that read as "loaded" and was half of why an empty slot
 * looked broken (public issue #1). */
static const char *sw_state_flash_msg = "DONE";
#endif
static RectFS developer_row_r[2], developer_back_r;

// settings / remap state
static bool remap_mode;
static bool screen_mode;
static bool developer_mode;
static bool developer_overlay_mode;
static int  remap_first_row;
static int  remap_arm = -1;         // row currently waiting for a button press
static uint32_t remap_arm_at;
static bool hud_pref_applied;
static uint32_t dump_flash_until;
static RectFS plaque_r[16];
static int    plaque_floor[16], plaque_count;
static float  grid_x, grid_y, grid_cell;
static int    ss_diag_current_fps;
static int    ss_diag_average_fps;

// live values snapshot for the current frame
static int cur_room, cur_floor_now, cur_palace;

static int sram8(int off) { return sram[off]; }
static int sram16(int off) { return sram[off] | (sram[off + 1] << 8); }
static int dung_flag(int room) {
  int off = room * 2;
  if (off + 1 >= (int)sizeof(dung_flags)) return 0;
  return dung_flags[off] | (dung_flags[off + 1] << 8);
}
static int bottle_level(void) {
  int sel = sram8(0x4F);
  if (sel <= 0) return 0;
  int v = sram8(0x5C + sel - 1);
  return v > 7 ? 7 : v;
}
static bool slot_owned(int i) {
  return (i == 15 ? sram8(0x4F) : sram8(0x40 + i)) > 0;
}
static int mode_for_module(int m) {
  if (m <= 0x05) return MODE_TITLE;
  if (m == 0x12 || m == 0x14 || m == 0x17 || (m >= 0x18 && m <= 0x1A)) return MODE_CINEMA;
  return MODE_GAME;
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static bool in_rect(const RectFS *r, float x, float y) {
  return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}
static float unit_for_size(int w, int h) {
  float unit = (w < h ? w : h) / 720.0f;
#if defined(__3DS__) || defined(__SWITCH__)
  if (unit < 0.5f)
    unit = 0.5f;
#endif
  return unit;
}

// draw primitives
static void set_color(uint32_t c) {
  SDL_SetRenderDrawColor(ss_r, (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff, (c >> 24) & 0xff);
}
static void fill_rect(float x, float y, float w, float h, uint32_t c) {
  SDL_FRect r = {x, y, w, h};
  set_color(c);
  SDL_RenderFillRectF(ss_r, &r);
}
static void draw_frame(float x, float y, float w, float h, float t, uint32_t c) {
  fill_rect(x, y, w, t, c);
  fill_rect(x, y + h - t, w, t, c);
  fill_rect(x, y, t, h, c);
  fill_rect(x + w - t, y, t, h, c);
}
// rounded-rect fill; nested insets give rounded borders
static void fill_round(float x, float y, float w, float h, float rad, uint32_t c) {
  if (rad > w / 2) rad = w / 2;
  if (rad > h / 2) rad = h / 2;
  set_color(c);
  SDL_FRect mid = {x, y + rad, w, h - 2 * rad};
  SDL_RenderFillRectF(ss_r, &mid);
  for (int i = 0; i < (int)rad; i++) {
    float dy = rad - i;
    float dx = rad - sqrtf(rad * rad - dy * dy);
    SDL_FRect t = {x + dx, y + i, w - 2 * dx, 1};
    SDL_FRect b = {x + dx, y + h - 1 - i, w - 2 * dx, 1};
    SDL_RenderFillRectF(ss_r, &t);
    SDL_RenderFillRectF(ss_r, &b);
  }
}
static void fill_circle(float cx, float cy, float r, uint32_t c) {
  set_color(c);
  for (int dy = (int)-r; dy <= (int)r; dy++) {
    float dx = sqrtf(r * r - dy * dy);
    SDL_FRect seg = {cx - dx, cy + dy, dx * 2, 1};
    SDL_RenderFillRectF(ss_r, &seg);
  }
}
static void stroke_circle(float cx, float cy, float r, float t, uint32_t c) {
  set_color(c);
  float ri = r - t;
  for (int dy = (int)-r; dy <= (int)r; dy++) {
    float dxo = sqrtf(r * r - dy * dy);
    float dxi = (float)fabs((double)dy) < ri ? sqrtf(ri * ri - dy * dy) : 0;
    SDL_FRect a = {cx - dxo, cy + dy, dxo - dxi, 1};
    SDL_FRect b = {cx + dxi, cy + dy, dxo - dxi, 1};
    SDL_RenderFillRectF(ss_r, &a);
    SDL_RenderFillRectF(ss_r, &b);
  }
}
static void draw_x_mark(float cx, float cy, float r, float t, uint32_t c) {
  set_color(c);
  for (float d = -r; d <= r; d += 1.0f) {
    SDL_FRect a = {cx + d - t / 2, cy + d - t / 2, t, t};
    SDL_FRect b = {cx + d - t / 2, cy - d - t / 2, t, t};
    SDL_RenderFillRectF(ss_r, &a);
    SDL_RenderFillRectF(ss_r, &b);
  }
}
static void tri_up(float cx, float top, float size, uint32_t c) {
  set_color(c);
  for (int i = 0; i <= (int)size; i++) {
    float half = size * (i / size) * 0.5773f * 2.0f;  // ~equilateral
    SDL_FRect seg = {cx - half / 2, top + i, half, 1};
    SDL_RenderFillRectF(ss_r, &seg);
  }
}

// blit one cell from a sheet texture
static void draw_cell(SDL_Texture *tex, int cell, int cellpx, int cols, float x, float y, float s) {
  if (cell < 0 || !tex) return;
  SDL_Rect src = {(cell % cols) * cellpx, (cell / cols) * cellpx, cellpx, cellpx};
  SDL_FRect dst = {x, y, cellpx * s, cellpx * s};
  SDL_RenderCopyF(ss_r, tex, &src, &dst);
}
static void draw_icon(int cell, float x, float y, float s)  { draw_cell(tex_icons, cell, 16, SS_ICON_COLS, x, y, s); }
static void draw_glyph(int cell, float x, float y, float s) { draw_cell(tex_glyphs, cell, 8, SS_GLYPH_COLS, x, y, s); }

static void draw_icon_inner(int cell, float x, float y, float size) {
  if (cell < 0 || !tex_icons) return;
  SDL_Rect src = {(cell % SS_ICON_COLS) * 16 + 1, (cell / SS_ICON_COLS) * 16 + 1, 14, 14};
  SDL_FRect dst = {x, y, size, size};
  SDL_RenderCopyF(ss_r, tex_icons, &src, &dst);
}

static float text_width(const char *s, float sc) {
  float w = 0;
  for (; *s; s++) w += (*s == ' ' ? 5 : 8) * sc;
  return w;
}
/* Draw at sc, shrinking only as far as the available width demands.  Every
 * Switch menu string goes through this, so a longer translation reflows
 * instead of running off its box. */
static void draw_text(const char *s, float x, float y, float sc);
static float text_width(const char *s, float sc);

static float fit_scale(const char *s, float sc, float max_w) {
  float w = text_width(s, sc);
  if (w > max_w && w > 0.0f)
    sc *= max_w / w;
  return sc;
}

static void draw_text_fit(const char *s, float x, float y, float sc, float max_w) {
  draw_text(s, x, y, fit_scale(s, sc, max_w));
}

static void draw_text_right_fit(const char *s, float right, float y, float sc,
                                float max_w) {
  float used = fit_scale(s, sc, max_w);
  draw_text(s, right - text_width(s, used), y, used);
}

static void draw_text(const char *s, float x, float y, float sc) {
  static const int kDigitGlyph[10] = {
    SS_GLYPH_DIGIT0, SS_GLYPH_DIGIT1, SS_GLYPH_DIGIT2, SS_GLYPH_DIGIT3, SS_GLYPH_DIGIT4,
    SS_GLYPH_DIGIT5, SS_GLYPH_DIGIT6, SS_GLYPH_DIGIT7, SS_GLYPH_DIGIT8, SS_GLYPH_DIGIT9,
  };
  float cx = x;
  for (; *s; s++) {
    /* The atlas is uppercase-only, so a lowercase string used to draw as
     * nothing at all -- which is why the Quick Item rows showed a blank value
     * for every real item: kItemNames is lowercase ("hookshot"), while "OFF"
     * happened to be uppercase and rendered fine.  Folding here fixes every
     * lowercase string in the UI at once instead of uppercasing one table. */
    char ch = *s;
    if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
    if (ch == ' ') { cx += 5 * sc; continue; }
    if (ch >= '0' && ch <= '9') draw_glyph(kDigitGlyph[ch - '0'], cx, y, sc);
    else if (ch >= 'A' && ch <= 'Z') draw_cell(tex_letters, kSS_LetterCell[ch - 'A'], 8, SS_LETTER_COLS, cx, y, sc);
    cx += 8 * sc;
  }
}

static const uint8_t *tiny_letter(char ch) {
  static const uint8_t letters[26][7] = {
    {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
    {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
    {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
    {14,17,16,23,17,17,14}, {17,17,17,31,17,17,17},
    {14,4,4,4,4,4,14}, {7,2,2,2,18,18,12},
    {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
    {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
    {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
    {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31},
  };
  static const uint8_t digits[10][7] = {
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
    {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
    {2,6,10,18,31,2,2}, {31,16,30,1,1,17,14},
    {6,8,16,30,17,17,14}, {31,1,2,4,8,8,8},
    {14,17,17,14,17,17,14}, {14,17,17,15,1,2,12},
  };
  static const uint8_t dot[7] = {0,0,0,0,0,12,12};
  static const uint8_t dash[7] = {0,0,0,31,0,0,0};
  if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';   /* same uppercase-only atlas */
  if (ch >= 'A' && ch <= 'Z')
    return letters[ch - 'A'];
  if (ch >= '0' && ch <= '9')
    return digits[ch - '0'];
  if (ch == '.')
    return dot;
  if (ch == '-')
    return dash;
  return NULL;
}

static float tiny_text_width(const char *s, float sc) {
  float w = 0;
  for (; *s; s++)
    w += (*s == ' ' ? 4 : 6) * sc;
  return w;
}

static void draw_tiny_text(const char *s, float x, float y, float sc, uint32_t c) {
  float cx = floorf(x + 0.5f);
  float cy = floorf(y + 0.5f);
  for (; *s; s++) {
    const uint8_t *rows = tiny_letter(*s);
    if (rows) {
      for (int yy = 0; yy < 7; yy++)
        for (int xx = 0; xx < 5; xx++)
          if (rows[yy] & (1 << (4 - xx)))
            fill_rect(cx + xx * sc, cy + yy * sc, sc, sc, c);
    }
    cx += (*s == ' ' ? 4 : 6) * sc;
  }
}

static void draw_block_text(const char *s, float x, float y, float sc,
                            uint32_t color) {
  draw_tiny_text(s, floorf(x + 0.5f), floorf(y + 0.5f), floorf(sc + 0.5f),
                 color);
}

static void draw_block_label_value(const char *label, const char *value,
                                   float x, float y, float sc,
                                   uint32_t label_color,
                                   uint32_t value_color) {
  draw_block_text(label, x, y, sc, label_color);
  draw_block_text(value, x + 190 * u, y, sc, value_color);
}
static void draw_number(int value, int digits, float x, float y, float s, bool yellow) {
  static const int kD[10]  = {SS_GLYPH_DIGIT0, SS_GLYPH_DIGIT1, SS_GLYPH_DIGIT2, SS_GLYPH_DIGIT3, SS_GLYPH_DIGIT4,
                              SS_GLYPH_DIGIT5, SS_GLYPH_DIGIT6, SS_GLYPH_DIGIT7, SS_GLYPH_DIGIT8, SS_GLYPH_DIGIT9};
  static const int kDy[10] = {SS_GLYPH_DIGIT0Y, SS_GLYPH_DIGIT1Y, SS_GLYPH_DIGIT2Y, SS_GLYPH_DIGIT3Y, SS_GLYPH_DIGIT4Y,
                              SS_GLYPH_DIGIT5Y, SS_GLYPH_DIGIT6Y, SS_GLYPH_DIGIT7Y, SS_GLYPH_DIGIT8Y, SS_GLYPH_DIGIT9Y};
  for (int i = digits - 1; i >= 0; i--) {
    draw_glyph((yellow ? kDy : kD)[value % 10], x + i * 8 * s, y, s);
    value /= 10;
  }
}

// ALttP menu-style box: black fill, colored double border, corner dots
static void menu_box(RectFS r, uint32_t border) {
  fill_round(r.x, r.y, r.w, r.h, 10 * u, COL_BOX);
  fill_round(r.x + 3 * u, r.y + 3 * u, r.w - 6 * u, r.h - 6 * u, 8 * u, border);
  fill_round(r.x + 7 * u, r.y + 7 * u, r.w - 14 * u, r.h - 14 * u, 6 * u, COL(200, 200, 200));
  fill_round(r.x + 9 * u, r.y + 9 * u, r.w - 18 * u, r.h - 18 * u, 6 * u, COL_BOX);
  float d = 3.5f * u;
  fill_circle(r.x + 8 * u, r.y + 8 * u, d, COL(255, 255, 255));
  fill_circle(r.x + r.w - 8 * u, r.y + 8 * u, d, COL(255, 255, 255));
  fill_circle(r.x + 8 * u, r.y + r.h - 8 * u, d, COL(255, 255, 255));
  fill_circle(r.x + r.w - 8 * u, r.y + r.h - 8 * u, d, COL(255, 255, 255));
}
static void slot_bg(float x, float y, float size) {
  fill_round(x, y, size, size, 10 * u, COL(70, 70, 70));
  fill_round(x + 2.5f * u, y + 2.5f * u, size - 5 * u, size - 5 * u, 8 * u, COL(30, 30, 30));
}

// textures from second_screen.c buffers
static SDL_Texture *make_tex(int w, int h, const void *px, bool blend) {
  SDL_Texture *t = SDL_CreateTexture(ss_r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
  if (!t) return NULL;
  SDL_UpdateTexture(t, NULL, px, w * 4);
  SDL_SetTextureScaleMode(t, SDL_ScaleModeNearest);
  if (blend) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
  return t;
}

// Tile a theme texture across r at 2x, clipped
static void draw_tiled(SDL_Texture *tex, int tw, int th, RectFS r, uint32_t fallback) {
  if (!tex) { fill_rect(r.x, r.y, r.w, r.h, fallback); return; }
  SDL_Rect clip = {(int)r.x, (int)r.y, (int)r.w, (int)r.h};
  SDL_RenderSetClipRect(ss_r, &clip);
  float sw = tw * 2.0f, sh = th * 2.0f;
  for (float y = r.y; y < r.y + r.h; y += sh)
    for (float x = r.x; x < r.x + r.w; x += sw) {
      SDL_FRect dst = {x, y, sw, sh};
      SDL_RenderCopyF(ss_r, tex, NULL, &dst);
    }
  SDL_RenderSetClipRect(ss_r, NULL);
}

static bool try_load_art(void) {
  static uint32_t buf[512 * 512];   // reused for every sheet; world map is the largest
  uint8_t lay[16 * 25];

  // theme tiles are baked into the binary
  if (!tex_bg_menu)  tex_bg_menu  = make_tex(kSSTexMenu_W, kSSTexMenu_H, kSSTexMenu, false);
  if (!tex_bg_parch) tex_bg_parch = make_tex(kSSTexParch_W, kSSTexParch_H, kSSTexParch, false);
  if (!tex_bg_stone) tex_bg_stone = make_tex(kSSTexStone_W, kSSTexStone_H, kSSTexStone, false);

  // cheap probe: the engine hasn't parsed zelda3_assets.dat yet
  if (SS_GetDungeonLayout(0, lay, sizeof(lay)) < 0) return false;
  if (!SS_RenderWorldMap(buf, false)) return false;
  tex_map[0] = make_tex(512, 512, buf, false);
  SS_RenderWorldMap(buf, true);
  tex_map[1] = make_tex(512, 512, buf, false);

  SS_RenderIconSheet(buf);
  tex_icons = make_tex(SS_ICON_COLS * 16, ((kIconCount + kIconCols - 1) / kIconCols) * 16, buf, true);
  SS_RenderGlyphSheet(buf);
  tex_glyphs = make_tex(SS_GLYPH_COLS * 8, ((kGlyphCount + kGlyphCols - 1) / kGlyphCols) * 8, buf, true);
  SS_RenderLetterSheet(buf);
  tex_letters = make_tex(16 * 8, 2 * 8, buf, true);
  SS_RenderLinkFace(buf, 0);
  tex_face = make_tex(16, 16, buf, true);

  tex_floor = SDL_CreateTexture(ss_r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 80, 80);
  SDL_SetTextureBlendMode(tex_floor, SDL_BLENDMODE_BLEND);
  tex_mapicons = SDL_CreateTexture(ss_r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 32, 8);
  SDL_SetTextureBlendMode(tex_mapicons, SDL_BLENDMODE_BLEND);

  for (int i = 0; i < 14; i++) {
    int r = SS_GetDungeonLayout(i, lay, sizeof(lay));
    if (r < 0) return false;
    Dungeon *d = &dungeons[i];
    d->name = kDungeonNames[i];
    d->boss = kDungeonBoss[i];
    d->floors = r & 0xFF;
    if (d->floors > 16) d->floors = 16;
    d->basements = (r >> 8) & 0xFF;
    for (int f = 0; f < d->floors; f++)
      memcpy(d->layout[f], lay + f * 25, 25);
  }
  return true;
}

// panels

static void draw_cinema(void) {
  fill_rect(0, 0, W, H, COL_BOX & 0xff000000u);  // black
  draw_frame(12 * u, 12 * u, W - 24 * u, H - 24 * u, 2 * u, COL_GOLD_DARK);
  float t = SDL_GetTicks() / 1000.0f;
  float pulse = sinf(t * 1.5f) * 0.5f + 0.5f;
  uint8_t g = (uint8_t)(150 + 100 * pulse);
  uint32_t c = COL(g, (uint8_t)(g * 0.83f), (uint8_t)(g * 0.41f));
  float s = (W < H ? W : H) * 0.06f;
  float cx = W / 2.0f, cy = H / 2.0f;
  tri_up(cx, cy - s, s, c);            // top
  tri_up(cx - s * 0.58f, cy, s, c);    // bottom-left
  tri_up(cx + s * 0.58f, cy, s, c);    // bottom-right
}

static void draw_overworld(RectFS r, int link_x, int link_y, int area) {
  // parchment sheet with gold frame
  draw_tiled(tex_bg_parch, kSSTexParch_W, kSSTexParch_H, r, COL_BG_PARCH);
  draw_frame(r.x + u, r.y + u, r.w - 2 * u, r.h - 2 * u, 3 * u, COL_GOLD_DARK);
  draw_frame(r.x + 4 * u, r.y + 4 * u, r.w - 8 * u, r.h - 8 * u, 2 * u, COL_GOLD);

  float pad = 8 * u;
  RectFS m = {r.x + pad, r.y + pad, r.w - 2 * pad, r.h - 2 * pad};

  bool dark = (area & 0x40) != 0;
#ifdef __SWITCH__
  pins_load();
#endif
  SDL_Texture *map = tex_map[dark ? 1 : 0];
  float px = 128.0f + (link_x / 4096.0f) * 256.0f;
  float py = 128.0f + (link_y / 4096.0f) * 256.0f;

  SDL_Rect clip = {(int)m.x, (int)m.y, (int)m.w, (int)m.h};
  SDL_RenderSetClipRect(ss_r, &clip);

  float scale, ox, oy;
  if (whole_map) {
    scale = (m.w < m.h ? m.w : m.h) / 512.0f;
    ox = m.x + m.w / 2 - 256.0f * scale;
    oy = m.y + m.h / 2 - 256.0f * scale;
  } else {
    scale = 2.6f * u;
    float cxm = clampf(px, m.w / scale / 2, 512.0f - m.w / scale / 2);
    float cym = clampf(py, m.h / scale / 2, 512.0f - m.h / scale / 2);
    ox = m.x + m.w / 2 - cxm * scale;
    oy = m.y + m.h / 2 - cym * scale;
  }
  SDL_FRect dst = {ox, oy, 512 * scale, 512 * scale};
  SDL_RenderCopyF(ss_r, map, NULL, &dst);

#ifdef __SWITCH__
  /* Publish the projection the tap handler has to invert.  Set after the map
   * is placed so a pin can never be resolved against a stale layout. */
  map_live = true;
  map_dark = dark;
  map_ox = ox; map_oy = oy; map_scale = scale;
  map_view_r = m;

  /* The player's own pins, this world only, under Link's marker. */
  for (int i = 0; i < pin_count; i++) {
    if ((pin_dark[i] != 0) != dark) continue;
    draw_pin(ox + (128.0f + pin_x[i] / 4096.0f * 256.0f) * scale,
             oy + (128.0f + pin_y[i] / 4096.0f * 256.0f) * scale,
             (whole_map ? 1.0f : 1.6f) * u);
  }
#endif

  // X marks for unclaimed pendants / crystals
  const int (*marks)[3] = dark ? kCrystalMarks : kPendantMarks;
  int nmarks = dark ? 7 : 3;
  int owned = sram8(dark ? 0x7A : 0x74);
  for (int i = 0; i < nmarks; i++) {
    if (owned & marks[i][0]) continue;
    float mx = ox + (128.0f + marks[i][1] / 4096.0f * 256.0f) * scale;
    float my = oy + (128.0f + marks[i][2] / 4096.0f * 256.0f) * scale;
    draw_x_mark(mx, my, 8 * u, 8 * u, COL_OUTLINE);
    draw_x_mark(mx, my, 8 * u, 4.5f * u, COL(224, 40, 32));
  }

  // Link's bobbing head
  float fx = ox + px * scale, fy = oy + py * scale;
  float bob = sinf(SDL_GetTicks() / 300.0f) * 2 * u;
  float fs = (whole_map ? 1.2f : 1.6f) * u * 2;   // face tex is 16px (Java pre-scaled to 32)
  SDL_FRect fdst = {fx - 16 * fs / 2, fy - 16 * fs / 2 + bob, 16 * fs, 16 * fs};
  SDL_RenderCopyF(ss_r, tex_face, NULL, &fdst);

#ifdef __SWITCH__
  /* Facing indicator.  A gold pennant-style wedge on the side Link faces,
   * drawn around the authentic face rather than rotating it: the face is real
   * game art and stays upright, and the direction cannot be wrong.
   *
   * (Per-direction Link sprites are not available as a static sheet -- see the
   * note on SS_RenderLinkFaceArmor in second_screen.c.)  Indoors the map is
   * frozen at the doorway, so the marker shows no direction at all rather
   * than implying an indoor facing. */
  if (!SS_IsIndoors()) {
    int facing = SS_GetLinkFacing();
    float cx = fx, cy = fy + bob;
    float d = 16 * fs * 0.5f + 3 * u;   /* just outside the face */
    float w = 5.0f * u;
    static const float kOff[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
    float ax = cx + kOff[facing][0] * d, ay = cy + kOff[facing][1] * d;
    for (int pass = 0; pass < 2; pass++) {
      uint32_t col = pass ? COL_GOLD : COL_OUTLINE;
      float t = pass ? w : w + 1.5f * u;
      for (int i = 0; i < (int)t; i++) {
        float span = (t - i) * 2.0f;
        if (facing <= 1) {
          float yy = ay + kOff[facing][1] * i;
          fill_rect(ax - span / 2, yy, span, 1.0f, col);
        } else {
          float xx = ax + kOff[facing][0] * i;
          fill_rect(xx, ay - span / 2, 1.0f, span, col);
        }
      }
    }
  }
#endif
  SDL_RenderSetClipRect(ss_r, NULL);

  // zoom toggle button
  float bs2 = 56 * u, bx = r.x + 14 * u, by = r.y + 14 * u;
  fill_round(bx, by, bs2, bs2, 8 * u, COL_BOX);
  fill_round(bx + 3 * u, by + 3 * u, bs2 - 6 * u, bs2 - 6 * u, 6 * u, COL_BOX_BORDER2);
  fill_round(bx + 6 * u, by + 6 * u, bs2 - 12 * u, bs2 - 12 * u, 5 * u, COL_BOX);
  float cxb = bx + bs2 / 2, cyb = by + bs2 / 2, arm = 14 * u, th = 5 * u;
  fill_rect(cxb - arm, cyb - th / 2, arm * 2, th, COL(255, 255, 255));
  if (whole_map) fill_rect(cxb - th / 2, cyb - arm, th, arm * 2, COL(255, 255, 255));
#ifdef __SWITCH__
  /* A long press here is the zoom, not a pin (donor rule). */
  map_zoom_r = (RectFS){bx, by, bs2, bs2};
#endif
}

static void draw_dungeon(RectFS r, int link_x, int link_y, int room, int dungeon_info) {
#ifdef __SWITCH__
  map_live = false;
#endif
  int palace = dungeon_info & 0xFF;
  int floor = (int8_t)(dungeon_info >> 8);
  Dungeon *d = (palace >= 0 && palace < 14) ? &dungeons[palace] : NULL;

  float bs = 3 * u;
  const char *name = d ? d->name : "DUNGEON";
  float tw = text_width(name, bs);
  float bx = r.x + r.w / 2 - tw / 2, by = r.y + 20 * u;
  fill_round(bx - 20 * u, by - 9 * u, tw + 40 * u, 8 * bs + 18 * u, 8 * u, COL_STONE_EDGE_L);
  fill_round(bx - 18 * u, by - 7 * u, tw + 36 * u, 8 * bs + 14 * u, 7 * u, COL_STONE_INSET);
  draw_text(name, bx, by, bs);
  if (!d) return;

  if (view_floor_touched_at && SDL_GetTicks() - view_floor_touched_at > 6000) {
    view_floor_offset = 0;
    view_floor_touched_at = 0;
  }
  int li = floor + view_floor_offset + d->basements;
  if (li < 0) li = 0;
  if (li > d->floors - 1) li = d->floors - 1;
  int view_floor = li - d->basements;

  // floor plaques down the left side
  plaque_count = 0;
  float ph = 50 * u, pw = 100 * u, pgap = 8 * u;
  float px0 = r.x + 24 * u, py0 = r.y + 78 * u;
  for (int f = d->floors - 1; f >= 0; f--) {
    int fl = f - d->basements;
    if (plaque_count >= 16) break;
    RectFS *pr = &plaque_r[plaque_count];
    *pr = (RectFS){px0, py0, pw, ph};
    plaque_floor[plaque_count] = fl;
    plaque_count++;
    bool sel = (fl == view_floor);
    fill_round(pr->x, pr->y, pr->w, pr->h, 6 * u, sel ? COL(160, 200, 255) : COL_STONE_EDGE_L);
    fill_round(pr->x + 2 * u, pr->y + 2 * u, pr->w - 4 * u, pr->h - 4 * u, 5 * u,
               sel ? COL_PLAQUE_SEL : COL_PLAQUE);
    char label[16];
    if (fl >= 0) snprintf(label, sizeof(label), "%dF", fl + 1);
    else snprintf(label, sizeof(label), "B%d", -fl);
    draw_text(label, pr->x + pr->w / 2 - text_width(label, 2 * u) / 2 + 8 * u,
              pr->y + pr->h / 2 - 8 * u, 2 * u);
    if (fl == floor) {
      SDL_FRect fdst = {pr->x + 4 * u, pr->y + pr->h / 2 - 13 * u, 16 * 1.7f * u, 16 * 1.7f * u};
      SDL_RenderCopyF(ss_r, tex_face, NULL, &fdst);
    }
    py0 += ph + pgap;
  }

  // the floor map square
  float inset = 20 * u;
  float mx0 = px0 + pw + 28 * u, my0 = r.y + 74 * u;
  float avail_w = r.x + r.w - inset - mx0, avail_h = r.y + r.h - inset - my0;
  float msize = avail_w < avail_h ? avail_w : avail_h;
  mx0 += (avail_w - msize) / 2;
  my0 += (avail_h - msize) / 2;
  fill_round(mx0, my0, msize, msize, 10 * u, COL_STONE_EDGE_D);
  fill_round(mx0 + 3 * u, my0 + 3 * u, msize - 6 * u, msize - 6 * u, 8 * u, COL_STONE_INSET);

  const uint8_t *lay = d->layout[li];
  float cell = (msize - 24 * u) / 5.0f;
  float gx = mx0 + 12 * u, gy = my0 + 12 * u;

  // the floor's rooms with the game's own map tiles
  static uint32_t floor_buf[80 * 80];
  if (!SS_RenderDungeonFloor(palace, li, floor_buf)) return;
  SDL_UpdateTexture(tex_floor, NULL, floor_buf, 80 * 4);
  SDL_FRect fdst = {gx, gy, 5 * cell, 5 * cell};
  SDL_RenderCopyF(ss_r, tex_floor, NULL, &fdst);

  // overlay sprites: blinking here-dot + boss skull
  static uint32_t icon_buf[32 * 8];
  bool icons = SS_RenderMapIcons(palace, icon_buf);
  if (icons) SDL_UpdateTexture(tex_mapicons, NULL, icon_buf, 32 * 4);
  bool has_compass = (sram16(0x64) & (0x8000 >> palace)) != 0;
  uint32_t frame = SDL_GetTicks() / 17;
  float ms = cell / 16.0f;

  for (int i = 0; i < 25; i++) {
    int v = lay[i];
    if (v == 0x0F) continue;
    int col = i % 5, row = i / 5;
    float x = gx + col * cell, y = gy + row * cell;
    bool is_cur = (v == (room & 0xFF)) && view_floor == floor;

    if (icons && has_compass && palace >= 2 && v == d->boss &&
        (dung_flag(v) & 0x800) == 0 && (frame & 0xF) < 10) {
      int pos = kDungeonBossPos[palace];
      if (pos >= 0) {
        float sx = x + (pos >> 8) * ms, sy = y + (pos & 0xFF) * ms;
        SDL_Rect src = {24, 0, 8, 8};
        SDL_FRect dd = {sx, sy, 8 * ms, 8 * ms};
        SDL_RenderCopyF(ss_r, tex_mapicons, &src, &dd);
      }
    }
    if (is_cur) {
      draw_frame(x + 1.5f * u, y + 1.5f * u, cell - 3 * u, cell - 3 * u, 3 * u, COL_GOLD);
      if (icons) {
        int p = kDotPalette[(frame >> 2) & 3];
        float sx = x + (((link_x & 0x1E0) >> 5) - 3) * ms;
        float sy = y + (((link_y & 0x1E0) >> 5) - 3) * ms;
        SDL_Rect src = {p * 8, 0, 8, 8};
        SDL_FRect dd = {sx, sy, 8 * ms, 8 * ms};
        SDL_RenderCopyF(ss_r, tex_mapicons, &src, &dd);
      }
    }
  }
}

static void draw_items(RectFS r) {
  menu_box(r, COL_BOX_BORDER);
  draw_text("ITEMS", r.x + r.w / 2 - text_width("ITEMS", 3 * u) / 2, r.y + 18 * u, 3 * u);

  float cw1 = (r.w - 70 * u) / 5, cw2 = (r.h - 100 * u) / 4;
  float cellW = cw1 < cw2 ? cw1 : cw2;
  grid_cell = cellW;
  grid_x = r.x + r.w / 2 - cellW * 2.5f;
  grid_y = r.y + 40 * u + (r.h - 40 * u - 4 * cellW) / 2;

  int equipped = SS_GetEquippedSlot();
  for (int i = 0; i < 20; i++) {
    int col = i % 5, row = i / 5;
    float x = grid_x + col * cellW, y = grid_y + row * cellW;
    if (i + 1 == equipped) {
      fill_round(x + 4 * u, y + 4 * u, cellW - 8 * u, cellW - 8 * u, 10 * u, COL_GOLD);
      fill_round(x + 8 * u, y + 8 * u, cellW - 16 * u, cellW - 16 * u, 7 * u, COL(46, 40, 16));
    }
    if (i == tap_flash_slot && SDL_GetTicks() < tap_flash_until)
      fill_round(x + 4 * u, y + 4 * u, cellW - 8 * u, cellW - 8 * u, 10 * u, COL(90, 82, 56));
    int lv = (i == 15) ? bottle_level() : sram8(0x40 + i);
    if (lv <= 0) continue;
    if (lv > kSS_ItemMaxLevel[i]) lv = kSS_ItemMaxLevel[i];
    float is = (cellW - 24 * u) / 16.0f;
    is = clampf(is, 3 * u, 6 * u);
    float icon_size = 16 * is;
    draw_icon_inner(kSS_ItemCell[i][lv], x + (cellW - icon_size) / 2,
                    y + (cellW - icon_size) / 2, icon_size);
  }
  (void)kItemNames;
}

static void draw_gear(RectFS r) {
  menu_box(r, COL_BOX_BORDER2);
  draw_tiny_text("GEAR", r.x + r.w / 2 - tiny_text_width("GEAR", 2.0f) / 2,
                 r.y + 12.0f, 2.0f, COL(250, 250, 250));

  int sword = sram8(0x59), shield = sram8(0x5A);
  int gear_cells[7];
  gear_cells[0] = (sword > 0 && sword != 0xFF) ? SS_ICON_SWORD_1 + (sword > 4 ? 3 : sword - 1) : -1;
  gear_cells[1] = (shield > 0 && shield != 0xFF) ? SS_ICON_SHIELD_1 + (shield > 3 ? 2 : shield - 1) : -1;
  gear_cells[2] = SS_ICON_ARMOR_0 + (sram8(0x5B) > 2 ? 2 : sram8(0x5B));
  gear_cells[3] = sram8(0x54) > 0 ? SS_ICON_GLOVES_1 + (sram8(0x54) > 2 ? 1 : sram8(0x54) - 1) : -1;
  gear_cells[4] = sram8(0x55) > 0 ? SS_ICON_BOOTS_1 : -1;
  gear_cells[5] = sram8(0x56) > 0 ? SS_ICON_FLIPPERS_1 : -1;
  gear_cells[6] = sram8(0x57) > 0 ? SS_ICON_MOONPEARL_1 : -1;

  float cell = 30.0f;
  float gap = 7.0f;
  float row_w = 7 * cell + 6 * gap;
  if (row_w > r.w - 22) {
    gap = 5.0f;
    cell = (r.w - 22 - 6 * gap) / 7.0f;
  }
  float x0 = r.x + (r.w - (7 * cell + 6 * gap)) / 2;
  float y0 = r.y + 30.0f;
  float icon_s = clampf((cell - 5.0f) / 16.0f, 1.25f, 1.75f);

  for (int i = 0; i < 7; i++) {
    float x = x0 + i * (cell + gap);
    float y = y0;
    slot_bg(x, y, cell);
    if (gear_cells[i] >= 0)
      draw_icon(gear_cells[i], x + (cell - 16 * icon_s) / 2,
                y + (cell - 16 * icon_s) / 2, icon_s);
  }

  float y1 = y0 + cell + 24.0f;
  float bottle_cell = 25.0f;
  float bottle_gap = 7.0f;
  draw_tiny_text("BOTTLES", x0, y1 - 12.0f, 1.0f, COL(250, 250, 250));
  for (int i = 0; i < 4; i++) {
    float x = x0 + i * (bottle_cell + bottle_gap);
    slot_bg(x, y1, bottle_cell);
    int lv = sram8(0x5C + i);
    if (lv > 7) lv = 7;
    if (lv > 0) {
      float bs = clampf((bottle_cell - 5.0f) / 16.0f, 1.15f, 1.35f);
      draw_icon(SS_ICON_BOTTLE_1 + (lv - 1),
                x + (bottle_cell - 16 * bs) / 2,
                y1 + (bottle_cell - 16 * bs) / 2, bs);
    }
  }
  float pend_x = r.x + r.w - 108.0f;
  draw_tiny_text("PENDANTS", pend_x, y1 - 12.0f, 1.0f, COL(250, 250, 250));
  int pend = sram8(0x74);
  static const int pbit[3] = {4, 2, 1};
  static const uint32_t pcol[3] = {COL(64, 200, 88), COL(70, 110, 240), COL(230, 60, 60)};
  for (int i = 0; i < 3; i++) {
    float cxp = pend_x + i * 28.0f + 13.0f;
    float cyp = y1 + 13.0f;
    fill_circle(cxp, cyp, 8.0f, (pend & pbit[i]) ? pcol[i] : COL(34, 34, 34));
    stroke_circle(cxp, cyp, 8.0f, 2.0f, COL_GOLD_DARK);
  }

  float cyC = y1 + 49.0f;
  draw_tiny_text("CRYSTALS", x0, cyC - 12.0f, 1.0f, COL(250, 250, 250));
  float crystal_start = x0 + 78.0f;
  float crystal_step = 23.0f;
  int owned7 = sram8(0x7A) & 0x7F, n_owned = 0;
  while (owned7) { n_owned += owned7 & 1; owned7 >>= 1; }
  for (int i = 0; i < 7; i++) {
    float cxp = crystal_start + i * crystal_step;
    float cyp = cyC;
    fill_circle(cxp, cyp, 7.0f, i < n_owned ? COL(110, 160, 255) : COL(34, 34, 34));
    stroke_circle(cxp, cyp, 7.0f, 2.0f, COL_GOLD_DARK);
  }

  float yp = cyC + 25.0f;
  draw_glyph(SS_GLYPH_HEART_FULL, x0, yp - 3.0f, 1.7f);
  int pieces = sram8(0x6B) & 3;
  for (int i = 0; i < 4; i++) {
    float gx = x0 + 34.0f + i * 25.0f, gy = yp;
    fill_round(gx - 2.0f, gy - 2.0f, 20.0f, 20.0f, 5.0f, COL_GOLD_DARK);
    fill_round(gx, gy, 16.0f, 16.0f, 4.0f, i < pieces ? COL(235, 80, 80) : COL(40, 34, 30));
  }
}

#ifdef __SWITCH__
/* One ring: the badge letter, the frame, and the assigned item's icon. */
static void draw_item_ring(float cx, float cy, float rr, int slot, const char *badge) {
  fill_circle(cx, cy, rr, COL(12, 12, 12));
  stroke_circle(cx, cy, rr, 5.0f, COL_GOLD_DARK);
  stroke_circle(cx, cy, rr - 2.5f, 2.0f, COL_GOLD);
  if (slot >= 1 && slot <= 20) {
    int i = slot - 1;
    int lv = (i == 15) ? bottle_level() : sram8(0x40 + i);
    if (lv < 0) lv = 0;
    if (lv > kSS_ItemMaxLevel[i]) lv = kSS_ItemMaxLevel[i];
    if (lv > 0) {
      float item_size = rr * 1.35f;
      draw_icon_inner(kSS_ItemCell[i][lv], cx - item_size / 2, cy - item_size / 2,
                      item_size);
    }
  }
  draw_text(badge, cx + rr - 9.0f, cy - rr + 3.0f, 0.9f);
}
#endif

static void draw_sidebar(float x, float y, float w, float h, bool dungeon_mode) {
  float s = 3 * u;
  bool show_keys = dungeon_mode && sram8(0x6F) != 0xFF;
  float chip_h = (show_keys ? 40 : 30) * s + 20 * u;
  menu_box((RectFS){x, y, w, chip_h}, COL_BOX_BORDER);
  float ry = y + 12 * u, ix = x + 10 * u, ne = x + w - 10 * u;
  draw_glyph(SS_GLYPH_RUPEE, ix + 4 * s, ry, s);
  int rupees = sram16(0x62); if (rupees > 9999) rupees = 9999;
  draw_number(rupees, 4, ne - 32 * s, ry, s, false);
  static const int kBombCap[8]  = {10, 15, 20, 25, 30, 35, 40, 50};
  static const int kArrowCap[8] = {30, 35, 40, 45, 50, 55, 60, 70};
  bool bombs_max = sram8(0x43) >= kBombCap[sram8(0x70) & 7];
  bool arrows_max = sram8(0x77) >= kArrowCap[sram8(0x71) & 7];
  ry += 10 * s;
  draw_glyph(SS_GLYPH_BOMB0, ix, ry, s); draw_glyph(SS_GLYPH_BOMB1, ix + 8 * s, ry, s);
  draw_number(sram8(0x43), 2, ne - 16 * s, ry, s, bombs_max);
  ry += 10 * s;
  draw_glyph(SS_GLYPH_ARROW0, ix, ry, s); draw_glyph(SS_GLYPH_ARROW1, ix + 8 * s, ry, s);
  draw_number(sram8(0x77), 2, ne - 16 * s, ry, s, arrows_max);
  if (show_keys) {
    ry += 10 * s;
    draw_glyph(SS_GLYPH_KEY, ix + 4 * s, ry, s);
    draw_number(sram8(0x6F), 1, ne - 8 * s, ry, s, false);
  }

  // Magic and hearts use integer pixel sizes on 3DS; fractional scaling made
  // the HUD look uneven in real bottom-screen dumps.
  float bar_h = 10.0f;
  bool half_magic = sram8(0x7B) >= 1;
  float my = floorf(y + h - bar_h - 5.0f + 0.5f);
  int cap = sram8(0x6C) >> 3; if (cap > 20) cap = 20;
  int cur = sram8(0x6D);
  int heart_cols = cap <= 5 ? cap : 5;
  if (heart_cols <= 0) heart_cols = 1;
  int heart_rows = (cap + heart_cols - 1) / heart_cols;
  float heart_s = 2.0f;
  float heart_px = 8.0f * heart_s;
  float hs = 18.0f;
  float hy = floorf(my - heart_rows * hs - 9.0f - (half_magic ? 14.0f : 0.0f) + 0.5f);

  // equipped item ring: tap cycles to the next owned item
  float rcx = x + w / 2, rcy = ((y + chip_h) + hy) / 2;
#ifdef __SWITCH__
  /* X ITEM RING (ported from the donor's Android companion): a second ring
   * holding the X-assigned item.  Tapping a ring ARMS it, and the next tap on
   * the items grid assigns to that button -- the donor's gesture, which is why
   * the ring only arms while the ITEMS page is up. */
  if (g_config.aleks_x_item_ring) {
    float rr = 32.0f * 0.72f;
    float cy_y = rcy - rr - 4.0f, cy_x = rcy + rr + 4.0f;
    y_ring_r = (RectFS){rcx - rr, cy_y - rr, rr * 2, rr * 2};
    x_ring_r = (RectFS){rcx - rr, cy_x - rr, rr * 2, rr * 2};
    if (armed_ring) {
      /* the armed ring breathes while it waits for a grid tap */
      float t = (SDL_GetTicks() % 2400) / 2400.0f;
      float pulse = sinf(t * 6.28318f) * 0.5f + 0.5f;
      uint32_t c = COL((int)(120 + 112 * pulse), (int)(100 + 94 * pulse), (int)(48 + 48 * pulse));
      stroke_circle(rcx, armed_ring == 1 ? cy_y : cy_x, rr + 5.0f, 4.0f, c);
    }
    draw_item_ring(rcx, cy_y, rr, SS_GetEquippedSlot(), "Y");
    draw_item_ring(rcx, cy_x, rr, SS_GetEquippedSlotX(), "X");
  } else
#endif
  {
  float ring_r = 32.0f;
  y_ring_r = (RectFS){rcx - ring_r, rcy - ring_r, ring_r * 2, ring_r * 2};
#ifdef __SWITCH__
  x_ring_r = (RectFS){0, 0, 0, 0};
#endif
  fill_circle(rcx, rcy, ring_r, COL(12, 12, 12));
  stroke_circle(rcx, rcy, ring_r, 6 * u, COL_GOLD_DARK);
  stroke_circle(rcx, rcy, ring_r - 3 * u, 2.5f * u, COL_GOLD);
  int slot = SS_GetEquippedSlot();
  if (slot >= 1 && slot <= 20) {
    int i = slot - 1;
    int lv = (i == 15) ? bottle_level() : sram8(0x40 + i);
    if (lv < 0) lv = 0;
    if (lv > kSS_ItemMaxLevel[i]) lv = kSS_ItemMaxLevel[i];
    if (lv > 0) {
      float item_size = 38.0f;
      draw_icon_inner(kSS_ItemCell[i][lv], rcx - item_size / 2,
                      rcy - item_size / 2, item_size);
    }
  }
  draw_text("Y", rcx + ring_r - 11.0f, rcy - ring_r + 4.0f, 1.0f);
  }

  // hearts (live health)
  float hx0 = floorf(x + (w - (heart_cols - 1) * hs - heart_px) / 2 + 0.5f);
  for (int i = 0; i < cap; i++) {
    int g = i < (cur >> 3) ? SS_GLYPH_HEART_FULL
          : (i == (cur >> 3) && (cur & 7) >= 4 ? SS_GLYPH_HEART_HALF : SS_GLYPH_HEART_EMPTY);
    draw_glyph(g, hx0 + (i % heart_cols) * hs, hy + (i / heart_cols) * hs, heart_s);
  }

  // magic bar (with the HUD's 1/2 marker when the upgrade is owned)
  if (half_magic) {
    float gx = floorf(x + (w - 24.0f) / 2 + 0.5f);
    draw_glyph(SS_GLYPH_HALF0, gx, my - 13.0f, 1.0f);
    draw_glyph(SS_GLYPH_HALF1, gx + 8.0f, my - 13.0f, 1.0f);
    draw_glyph(SS_GLYPH_HALF2, gx + 16.0f, my - 13.0f, 1.0f);
  }
  int magic = sram8(0x6E); if (magic > 128) magic = 128;
  float bar_x = floorf(x + 8.0f + 0.5f);
  float bar_w = floorf(w - 16.0f + 0.5f);
  fill_rect(bar_x, my, bar_w, bar_h, COL_GOLD_DARK);
  fill_rect(bar_x + 2.0f, my + 2.0f, bar_w - 4.0f, bar_h - 4.0f, COL_BOX);
  float frac = magic / 128.0f;
  if (frac > 0)
    fill_rect(bar_x + 3.0f, my + 3.0f, floorf((bar_w - 6.0f) * frac + 0.5f),
              bar_h - 6.0f, COL(72, 208, 72));
}

/*
 * Rewrite one `key = value` line inside a section of zelda3.ini.
 *
 * Written to a temporary file and renamed over the original, so an
 * interrupted save leaves the previous config intact rather than a truncated
 * one -- `fopen("zelda3.ini","wb")` had already destroyed the old contents
 * before the first byte of the new was written.
 */
static void update_ini(const char *section, const char *key, const char *value) {
  FILE *f = fopen("zelda3.ini", "rb");
  if (!f) { StartupLog("CONFIG SAVE: failed (no zelda3.ini)"); return; }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0 || size > 1 << 20) { fclose(f); return; }
  char *buf = malloc(size + 1);
  if (!buf || fread(buf, 1, size, f) != (size_t)size) { free(buf); fclose(f); return; }
  fclose(f);
  buf[size] = 0;

  /* Room for the replacement line, which can be far longer than the one it
   * replaces -- the gamepad map is ~256 bytes on its own, and the old fixed
   * +256 slack was a heap overflow waiting for a long enough value. */
  size_t slack = strlen(section) + strlen(key) + strlen(value) + 64;
  char *out = malloc(size + slack);
  if (!out) { free(buf); return; }
  size_t olen = 0, klen = strlen(key);
  bool done = false;
  char cur[64] = "";
  char *line = buf;
  while (line) {
    char *nl = strchr(line, '\n');
    size_t len = nl ? (size_t)(nl - line) + 1 : strlen(line);
    const char *t = line;
    while (*t == ' ' || *t == '\t') t++;
    if (*t == '[') {
      size_t n = 0;
      while (t[n] && t[n] != '\n' && t[n] != '\r' && n < sizeof(cur) - 1) { cur[n] = t[n]; n++; }
      cur[n] = 0;
    } else if (!done && !SDL_strcasecmp(cur, section) &&
               !SDL_strncasecmp(t, key, klen)) {
      const char *after = t + klen;
      while (*after == ' ' || *after == '\t') after++;
      if (*after == '=') {
        olen += sprintf(out + olen, "%s = %s\n", key, value);
        done = true;
        line = nl ? nl + 1 : NULL;
        continue;
      }
    }
    memcpy(out + olen, line, len);
    olen += len;
    line = nl ? nl + 1 : NULL;
  }
  if (!done)
    olen += sprintf(out + olen, "%s\n%s = %s\n", section, key, value);

  /* tmp -> flush -> close -> rename.  The rename is the commit point; until
   * it succeeds the original file is still the whole, valid config. */
  {
    bool ok = false;
    f = fopen("zelda3.ini.tmp", "wb");
    if (f) {
      ok = fwrite(out, 1, olen, f) == olen;
      if (fflush(f) != 0) ok = false;
      if (fclose(f) != 0) ok = false;
    }
    if (ok) {
      remove("zelda3.ini.bak");
      /* fsdev has no atomic replace, so the old file steps aside first and is
       * only removed once the new one is in place under the real name. */
      if (rename("zelda3.ini", "zelda3.ini.bak") == 0) {
        if (rename("zelda3.ini.tmp", "zelda3.ini") == 0) {
          remove("zelda3.ini.bak");
        } else {
          rename("zelda3.ini.bak", "zelda3.ini");   /* put it back */
          ok = false;
        }
      } else {
        ok = false;
      }
    }
    if (!ok) {
      remove("zelda3.ini.tmp");
      StartupLog("CONFIG SAVE: failed %s %s", section, key);
    } else {
      StartupLog("CONFIG SAVE: %s %s = %s", section, key, value);
    }
  }
  free(out);
  free(buf);
}

static void write_ini_gamepad_controls(void) {
  char v[256];
  size_t n = 0;
  /* An empty field here is what persisted the damage across launches, so a
   * name is always written -- the command's own default if it somehow still
   * has no button by this point. */
  sw_sanitize_bindings();
  for (int i = 0; i < 12; i++) {
    int b = pad_controls[i];
    if (i) n += sprintf(v + n, ", ");
    if (b < 0 || b >= 17) b = GamepadMap_DefaultForCmd(i);
    n += sprintf(v + n, "%s", kPadButtonIni[b]);
  }
  update_ini("[GamepadMap]", "Controls", v);
}

static void leave_remap(void) {
  if (remap_arm >= 0) SS_ArmButtonCapture(false);
  remap_arm = -1;
  remap_mode = false;
  remap_first_row = 0;
}

static void leave_settings_submenu(void) {
  leave_remap();
  screen_mode = false;
  developer_mode = false;
  developer_overlay_mode = false;
#ifdef __SWITCH__
  /* Leaving SETTINGS resets its menu stack, so reopening it always lands on
   * the root rather than wherever the user was three screens deep. */
  sw_depth = 0;
  sw_sel = -1;
  sw_scroll = 0;
#endif
}

static void draw_cog(float cx, float cy, float r) {
  for (int i = 0; i < 8; i++) {
    float a = (float)M_PI / 4 * i;
    fill_circle(cx + cosf(a) * r, cy + sinf(a) * r, r * 0.3f, COL(255, 255, 255));
  }
  fill_circle(cx, cy, r * 0.85f, COL(255, 255, 255));
  fill_circle(cx, cy, r * 0.38f, COL_BOX);
}

static void draw_settings_row(RectFS *row, bool armed) {
  fill_round(row->x, row->y, row->w, row->h, 8 * u, armed ? COL_GOLD : COL_GOLD_DARK);
  fill_round(row->x + 3 * u, row->y + 3 * u, row->w - 6 * u, row->h - 6 * u, 6 * u,
             armed ? COL(58, 48, 12) : COL(28, 28, 28));
}

#ifndef __SWITCH__
// The 3DS/desktop sub-panels.  None of them has a Switch counterpart: the
// pad remap belongs to the Switch input adapter, and the screen/turbo/dump
// rows are 3DS platform controls.  Their Switch replacements are further
// down, drawn with the same primitives.
static void draw_remap_panel(RectFS r) {
  draw_text("REMAP BUTTONS", r.x + r.w / 2 - text_width("REMAP BUTTONS", 3 * u) / 2,
            r.y + 18 * u, 3 * u);
  remap_back_r = (RectFS){r.x + 14 * u, r.y + 12 * u, 76 * u, 32 * u};
  draw_settings_row(&remap_back_r, false);
  draw_text("BACK", remap_back_r.x + remap_back_r.w / 2 - text_width("BACK", 1.8f * u) / 2,
            remap_back_r.y + remap_back_r.h / 2 - 7 * u, 1.8f * u);

  // resolve a pending capture from the game thread
  if (remap_arm >= 0) {
    int b = SS_GetCapturedButton();
    if (b >= 0) {
      /* Through the same swap the cycling rows use.  A direct assignment left
       * the displaced command sharing the control, and the mapper resolves
       * one command per button -- so the other one silently stopped working. */
      sw_set_binding(remap_arm, b);
      write_ini_gamepad_controls();
      remap_arm = -1;
    } else if (b == -1 || SDL_GetTicks() - remap_arm_at > 8000) {
      SS_ArmButtonCapture(false);
      remap_arm = -1;
    }
  }

  if (remap_first_row < 0)
    remap_first_row = 0;
  if (remap_first_row > 6)
    remap_first_row = 6;

  remap_page_r = (RectFS){r.x + r.w - 86 * u, r.y + 12 * u, 72 * u, 32 * u};
  draw_settings_row(&remap_page_r, false);
  draw_text(remap_first_row == 0 ? "MORE" : "TOP",
            remap_page_r.x + remap_page_r.w / 2 -
              text_width(remap_first_row == 0 ? "MORE" : "TOP", 1.8f * u) / 2,
            remap_page_r.y + remap_page_r.h / 2 - 7 * u, 1.8f * u);

  float row_h = 44 * u, gap = 6 * u;
  float y0 = r.y + 56 * u;
  float row_w = r.w - 42 * u;
  for (int visible = 0; visible < 6; visible++) {
    int i = remap_first_row + visible;
    RectFS *row = &remap_row_r[visible];
    *row = (RectFS){r.x + 14 * u, y0 + visible * (row_h + gap), row_w, row_h};
    bool armed = remap_arm == i;
    draw_settings_row(row, armed);
    float ty = row->y + row->h / 2 - 9 * u;
    draw_text(kPadCmdNames[i], row->x + 12 * u, ty, 2.1f * u);
    const char *v = armed ? "PRESS KEY"
        : (pad_controls[i] >= 0 && pad_controls[i] < 17 ? kPadButtonLabel[pad_controls[i]] : "----");
    draw_text(v, row->x + row->w - 12 * u - text_width(v, 2.1f * u), ty, 2.1f * u);
  }
  float bar_x = r.x + r.w - 18 * u;
  float bar_y = y0;
  float bar_h = 6 * row_h + 5 * gap;
  fill_round(bar_x, bar_y, 4 * u, bar_h, 2 * u, COL(42, 42, 42));
  float thumb_h = bar_h * 0.5f;
  float thumb_y = bar_y + (remap_first_row == 0 ? 0 : bar_h - thumb_h);
  fill_round(bar_x, thumb_y, 4 * u, thumb_h, 2 * u, COL_GOLD);
}

static const char *display_mode_label(void) {
#ifdef __3DS__
  switch (Platform3DS_GetDisplayMode()) {
  case kPlatform3DSDisplayOriginal: return "ORIGINAL";
  case kPlatform3DSDisplayStretch: return "STRETCH";
  case kPlatform3DSDisplayUltraWideMod:
  default: return "WIDE";
  }
#else
  return SS_IsWidescreen() ? "WIDE" : "ORIGINAL";
#endif
}

static const char *wide_zoom_label(void) {
#ifdef __3DS__
  switch (Platform3DS_GetWideZoomIndex()) {
  case 1: return "1.2X";
  case 2: return "1.5X";
  case 3: return "2X";
  case 4: return "2.5X";
  case 0:
  default: return "1X";
  }
#else
  return "1X";
#endif
}

static void draw_screen_panel(RectFS r) {
  draw_text("SCREEN", r.x + r.w / 2 - text_width("SCREEN", 3 * u) / 2,
            r.y + 18 * u, 3 * u);
  screen_back_r = (RectFS){r.x + 20 * u, r.y + 12 * u, 90 * u, 38 * u};
  draw_settings_row(&screen_back_r, false);
  draw_text("BACK", screen_back_r.x + screen_back_r.w / 2 - text_width("BACK", 2.2f * u) / 2,
            screen_back_r.y + screen_back_r.h / 2 - 9 * u, 2.2f * u);

  const char *edge_value = "FIXED CAMERA";
#ifdef __3DS__
  if (Platform3DS_GetWideEdgeMode() == kPlatform3DSWideEdgeStandard)
    edge_value = "STANDARD";
#endif
  bool hud_hidden = SS_IsHudHidden();
  bool wide = false;
#ifdef __3DS__
  wide = Platform3DS_GetDisplayMode() == kPlatform3DSDisplayUltraWideMod;
#else
  wide = SS_IsWidescreen();
#endif
  static const char *const labels[4] = {
    "DISPLAY MODE", "EDGE MODE", "ZOOM", "TOP HUD",
  };
  const char *values[4] = {
    display_mode_label(), edge_value, wide_zoom_label(),
    hud_hidden ? "OFF" : "ON",
  };
  int rows = wide ? 4 : 3;
  screen_row_r[2] = (RectFS){0};
  float row_h = 58 * u, gap = 14 * u;
  float y0 = r.y + 82 * u;
  for (int visible = 0; visible < rows; visible++) {
    int item = !wide && visible == 2 ? 3 : visible;
    RectFS *row = &screen_row_r[item];
    *row = (RectFS){
      r.x + 28 * u, y0 + visible * (row_h + gap),
      r.w - 56 * u, row_h,
    };
    draw_settings_row(row, false);
    float ty = row->y + row->h / 2 - 8 * u;
    draw_text(labels[item], row->x + 16 * u, ty, 2 * u);
    draw_text(values[item],
              row->x + row->w - 16 * u - text_width(values[item], 2 * u),
              ty, 2 * u);
  }
}

static void draw_developer_panel(RectFS r) {
  draw_text("DEVELOPER", r.x + r.w / 2 - text_width("DEVELOPER", 3 * u) / 2,
            r.y + 18 * u, 3 * u);
  developer_back_r = (RectFS){r.x + 20 * u, r.y + 12 * u, 90 * u, 38 * u};
  draw_settings_row(&developer_back_r, false);
  draw_text("BACK", developer_back_r.x + developer_back_r.w / 2 - text_width("BACK", 2.2f * u) / 2,
            developer_back_r.y + developer_back_r.h / 2 - 9 * u, 2.2f * u);

  const char *labels[2] = {"MEM DUMP", "OVERLAY"};
  const char *values[2] = {
    SDL_GetTicks() < dump_flash_until ? "DONE" : "WRITE",
    "OPEN",
  };
  float row_h = 58 * u, gap = 14 * u;
  float y0 = r.y + 82 * u;
  for (int i = 0; i < 2; i++) {
    RectFS *row = &developer_row_r[i];
    *row = (RectFS){r.x + 28 * u, y0 + i * (row_h + gap), r.w - 56 * u, row_h};
    draw_settings_row(row, false);
    float ty = row->y + row->h / 2 - 8 * u;
    draw_text(labels[i], row->x + 16 * u, ty, 2 * u);
    draw_text(values[i], row->x + row->w - 16 * u - text_width(values[i], 2 * u),
              ty, 2 * u);
  }
}

static void draw_developer_overlay_panel(RectFS r) {
  draw_text("OVERLAY", r.x + r.w / 2 - text_width("OVERLAY", 3 * u) / 2,
            r.y + 18 * u, 3 * u);
  developer_back_r = (RectFS){r.x + 20 * u, r.y + 12 * u, 90 * u, 38 * u};
  draw_settings_row(&developer_back_r, false);
  draw_text("BACK", developer_back_r.x + developer_back_r.w / 2 - text_width("BACK", 2.2f * u) / 2,
            developer_back_r.y + developer_back_r.h / 2 - 9 * u, 2.2f * u);

  char version[32], model[32], fps_now[32], fps_avg[32], core[32],
       display[32], location[48], module[32];
#ifdef __3DS__
  snprintf(model, sizeof(model), "%s",
           Platform3DS_IsNew3DS() ? "NEW 3DS" : "OLD 3DS");
  snprintf(core, sizeof(core), "%s",
           Platform3DS_CanUseCore1PpuWorker() ? "ON" : "OFF");
#else
  snprintf(model, sizeof(model), "DESKTOP");
  snprintf(core, sizeof(core), "N A");
#endif
  snprintf(version, sizeof(version), "%s", ZELDA3_3DS_VERSION);
  for (char *p = version; *p; p++)
    if (*p == '.' || *p == '-')
      *p = ' ';
  snprintf(fps_now, sizeof(fps_now), "%d", ss_diag_current_fps);
  snprintf(fps_avg, sizeof(fps_avg), "%d", ss_diag_average_fps);
  snprintf(display, sizeof(display), "%s", display_mode_label());
  if (SS_IsIndoors()) {
    int dungeon_info = SS_GetDungeon();
    int palace = dungeon_info & 0xff;
    if (palace >= 0 && palace < 14)
      snprintf(location, sizeof(location), "%s", kDungeonNames[palace]);
    else
      snprintf(location, sizeof(location), "HOUSE %02X", SS_GetArea() & 0xff);
  } else {
    snprintf(location, sizeof(location), "OVERWORLD %02X", SS_GetArea() & 0xff);
  }
  snprintf(module, sizeof(module), "%02X", SS_GetModule() & 0xff);

  float sc = 3.0f * u;
  float row_h = 30 * u;
  float x = r.x + 26 * u;
  float y = r.y + 62 * u;
  struct {
    const char *label;
    const char *value;
    uint32_t color;
  } rows[] = {
    {"VERSION", version, COL(255, 255, 255)},
    {"MODEL", model, COL_GOLD},
    {"FPS NOW", fps_now, COL(120, 255, 140)},
    {"FPS AVG", fps_avg, COL(120, 220, 255)},
    {"CORE1", core, COL(230, 230, 230)},
    {"SCREEN", display, COL(230, 230, 230)},
    {"ROOM", location, COL(230, 230, 230)},
    {"MODULE", module, COL(230, 230, 230)},
  };
  for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
    fill_round(x - 8 * u, y - 4 * u, r.w - 40 * u, row_h, 5 * u,
               i == 2 || i == 3 ? COL(34, 28, 12) : COL(24, 24, 24));
    draw_block_label_value(rows[i].label, rows[i].value, x, y, sc,
                           COL(170, 170, 170), rows[i].color);
    y += row_h + 3 * u;
  }
}


#endif  // !__SWITCH__

#ifdef __SWITCH__
/* ------------------------------------------------------------------ *
 * Switch settings.
 *
 * Presentation is the donor's, unchanged: menu_box, draw_settings_row,
 * draw_text, the chevron.  What differs is the CONTENT, because what there is
 * to configure differs -- the 3DS had a fixed pair of physical screens and a
 * C-stick, this has NORMAL / DUAL / FLIP with configurable sizes and gap,
 * which is the final ALEKS TMC settings model.
 *
 * Every row lives in ONE table (sw_build), read by both the painter and the
 * tap handler, so a controller press and a touch always run the same action.
 * Values are written to the authoritative home -- g_config for layout, SS_*
 * for engine state -- and persisted through the donor's in-place update_ini.
 * ------------------------------------------------------------------ */

static const char *sw_onoff(bool on) { return on ? "ON" : "OFF"; }

static const char *switch_display_label(void) {
  switch (g_config.aleks_display_mode) {
  case 1:  return "DUAL";
  case 2:  return "FLIP";
  default: return "NORMAL";
  }
}

static void ini_set_int(const char *key, int value) {
  char v[16];
  snprintf(v, sizeof(v), "%d", value);
  update_ini("[General]", key, v);
}

static void save_display_mode(void) {
  update_ini("[General]", "AleksDisplayMode",
             g_config.aleks_display_mode == 1 ? "Dual" :
             g_config.aleks_display_mode == 2 ? "Flip" : "Normal");
}

/* One step through a percentage preset list, wrapping. */
static uint8 cycle_pct(uint8 cur, int lo, int hi, int step) {
  int v = cur ? cur : 100;
  v += step;
  if (v > hi) v = lo;
  if (v < lo) v = lo;
  return (uint8)v;
}

/* MSU: the engine plays it, we only report whether the user has a pack.
 * -1 not probed yet, 0 none found, 1 present. */
static int sw_msu_state = -1;
static int sw_msu_pack(void) {
  if (sw_msu_state < 0) {
    FILE *f = fopen("msu/alttp_msu-1.pcm", "rb");
    if (!f) f = fopen("msu/alttp_msu-2.pcm", "rb");
    sw_msu_state = f ? 1 : 0;
    if (f) fclose(f);
  }
  return sw_msu_state;
}

/* ---- save-state slots ------------------------------------------------- */
static void sw_state_path(char *out, size_t n, int slot, const char *ext) {
  snprintf(out, n, "saves/save%d.%s", SW_STATE_SLOT0 + slot, ext);
}

static void sw_state_refresh(int slot) {
  char path[64];
  FILE *f;
  sw_state_path(path, sizeof(path), slot, "sav");
  f = fopen(path, "rb");
  sw_state_stamp[slot] = 0;
  if (f) {
    fseek(f, 0, SEEK_END);
    sw_state_stamp[slot] = ftell(f);   /* size stands in for "slot used" */
    fclose(f);
  }
  if (sw_state_thumb[slot]) {
    SDL_DestroyTexture(sw_state_thumb[slot]);
    sw_state_thumb[slot] = NULL;
  }
  /* No .sav means no slot, whatever a leftover .thumb from a removed or
   * failed save still says on disk. */
  sw_state_path(path, sizeof(path), slot, "thumb");
  f = sw_state_stamp[slot] ? fopen(path, "rb") : NULL;
  if (f) {
    static uint32_t px[kSsThumbW * kSsThumbH];
    if (fread(px, 1, sizeof(px), f) == sizeof(px))
      sw_state_thumb[slot] = make_tex(kSsThumbW, kSsThumbH, px, false);
    fclose(f);
  }
}

static void sw_state_refresh_all(void) {
  for (int i = 0; i < SW_STATE_SLOTS; i++)
    sw_state_refresh(i);
}

/* The renderer hands the grabbed frame to second_screen.c; we collect it here
 * on a later UI frame and write it beside the .sav. */
static void sw_state_poll_thumb(void) {
  static uint32_t px[kSsThumbW * kSsThumbH];
  if (sw_thumb_want < 0 || !SS_TakeThumbnail(px))
    return;
  char path[64];
  sw_state_path(path, sizeof(path), sw_thumb_want, "thumb");
  FILE *f = fopen(path, "wb");
  if (f) { fwrite(px, 1, sizeof(px), f); fclose(f); }
  sw_state_refresh(sw_thumb_want);
  sw_thumb_want = -1;
}

/* ---- map pins: storage ------------------------------------------------ */
/* map_pins.txt sits next to zelda3.ini, one "world,x,y" per line -- the
 * donor's own format, so a file written by the Android build loads here. */
static void pins_load(void) {
  if (pins_loaded) return;
  pins_loaded = true;
  FILE *f = fopen("map_pins.txt", "r");
  if (!f) return;
  char line[64];
  while (pin_count < SW_MAX_PINS && fgets(line, sizeof(line), f)) {
    int d, x, y;
    if (sscanf(line, "%d,%d,%d", &d, &x, &y) != 3) continue;   /* garbled: skip */
    if (d < 0 || d > 1 || x < 0 || x > 4095 || y < 0 || y > 4095) continue;
    pin_dark[pin_count] = (uint8)d;
    pin_x[pin_count] = x;
    pin_y[pin_count] = y;
    pin_count++;
  }
  fclose(f);
}

static void pins_save(void) {
  FILE *f = fopen("map_pins.txt", "w");
  if (!f) return;
  for (int i = 0; i < pin_count; i++)
    fprintf(f, "%d,%d,%d\n", pin_dark[i], pin_x[i], pin_y[i]);
  fclose(f);
}

/* Remove the pin under (wx, wy) in the current world, else drop a new one.
 * Returns true when something changed. */
static bool pins_toggle_world(int wx, int wy, int hit_world_units) {
  int dark = map_dark ? 1 : 0;
  for (int i = 0; i < pin_count; i++) {
    if (pin_dark[i] != dark) continue;
    int dx = pin_x[i] - wx, dy = pin_y[i] - wy;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dx > hit_world_units || dy > hit_world_units) continue;
    for (int k = i; k < pin_count - 1; k++) {
      pin_dark[k] = pin_dark[k + 1];
      pin_x[k] = pin_x[k + 1];
      pin_y[k] = pin_y[k + 1];
    }
    pin_count--;
    pins_save();
    return true;
  }
  if (pin_count >= SW_MAX_PINS) return false;
  if (wx < 0 || wx > 4095 || wy < 0 || wy > 4095) return false;
  pin_dark[pin_count] = (uint8)dark;
  pin_x[pin_count] = wx;
  pin_y[pin_count] = wy;
  pin_count++;
  pins_save();
  return true;
}

/* Controller affordance: drop or lift a pin where Link is standing.  Touch
 * gets the precise long-press; the pad gets the useful common case. */
static bool pins_toggle_at_link(void) {
  if (!map_live) return false;
  return pins_toggle_world(SS_GetLinkX(), SS_GetLinkY(), 96);
}

/* The donor's pennant, drawn as square pixels so it sits beside the HUD art
 * rather than looking like a vector overlay.  Foot of the pole marks the spot. */
static void draw_pin(float x, float y, float s) {
  static const char *const kPinArt[12] = {
    "########", "########", "#######.", "######..",
    "#####...", "###.....", "##......", "##......",
    "##......", "##......", "##......", "##......",
  };
  float px = x - 1 * s, py = y - 12 * s;
  for (int pass = 0; pass < 2; pass++) {
    uint32_t col = pass ? COL_GOLD : COL_OUTLINE;
    float spread = pass ? 0.0f : s;
    for (int oy = -1; oy <= 1; oy++) {
      for (int ox = -1; ox <= 1; ox++) {
        if (pass == 0 && ox == 0 && oy == 0) continue;
        if (pass == 1 && (ox || oy)) continue;
        for (int gy = 0; gy < 12; gy++)
          for (int gx = 0; gx < 8; gx++)
            if (kPinArt[gy][gx] == '#')
              fill_rect(px + ox * spread + gx * s, py + oy * spread + gy * s, s, s, col);
      }
    }
  }
}

/* Every quality-of-life flag the engine already implements (features.h).
 * These toggle the native feature bits -- no ALEKS gameplay code exists for
 * any of them, MoreActiveBombs included. */
static const unsigned kQolMask[12] = {
  kFeatures0_SwitchLR,
  kFeatures0_TurnWhileDashing,
  kFeatures0_MirrorToDarkworld,
  kFeatures0_CollectItemsWithSword,
  kFeatures0_BreakPotsWithSword,
  kFeatures0_DisableLowHealthBeep,
  kFeatures0_SkipIntroOnKeypress,
  kFeatures0_ShowMaxItemsInYellow,
  kFeatures0_MoreActiveBombs,
  kFeatures0_CarryMoreRupees,
  kFeatures0_MiscBugFixes,
  kFeatures0_CancelBirdTravel,
};
/*
 * The [Features] ini key for each QoL row, in kQolMask order.
 *
 * THE PERSISTENCE BUG: these rows called SS_SetFeature and stopped there, so
 * a QoL option was a runtime-only change that the next launch knew nothing
 * about -- the player had to re-enable everything every time.  Every name
 * here is the exact string config.c's [Features] parser matches, so what is
 * written is what comes back.
 */
static const char *const kQolIniKey[12] = {
  "ItemSwitchLR", "TurnWhileDashing", "MirrorToDarkworld",
  "CollectItemsWithSword", "BreakPotsWithSword", "DisableLowHealthBeep",
  "SkipIntroOnKeypress", "ShowMaxItemsInYellow", "MoreActiveBombs",
  "CarryMoreRupees", "MiscBugFixes", "CancelBirdTravel",
};
static const char *const kQolLabel[12] = {
  "ITEM SWITCH LR", "TURN WHILE DASHING", "MIRROR TO DARK WORLD",
  "COLLECT WITH SWORD", "BREAK POTS: LV2 SWORD", "NO LOW HEALTH BEEP",
  "SKIP INTRO", "MAX ITEMS IN YELLOW", "MORE ACTIVE BOMBS",
  "CARRY MORE RUPEES", "MISC BUG FIXES", "CANCEL BIRD TRAVEL",
};

/* ---- row tables ------------------------------------------------------- */
/* Builds the current menu into sw_rows and returns the count.  This is the
 * single definition of what each row means. */
static int sw_build(int menu) {
  int n = 0;
  unsigned feats = SS_GetFeatures();
  #define ROW(l, v)  do { sw_rows[n].label = (l); sw_rows[n].value = (v); \
                          sw_rows[n].submenu = false; n++; } while (0)
  #define SUB(l)     do { sw_rows[n].label = (l); sw_rows[n].value = NULL;  \
                          sw_rows[n].submenu = true;  n++; } while (0)
  #define FMT(...)   (snprintf(sw_value_buf[n], sizeof(sw_value_buf[n]), __VA_ARGS__), \
                      sw_value_buf[n])

  switch (menu) {
  case SW_ROOT:
    SUB("GAMEPLAY");
    SUB("SCREEN");
    SUB("CONTROLS");
    SUB("AUDIO");
    SUB("RETROACHIEVEMENTS");
    SUB("SYSTEM");
    break;

  case SW_RETRO:
    ROW("ENABLE", sw_onoff(AleksRA_IsEnabled()));
    /* Status and game are read straight from the client, so the page can
     * never claim a state the client is not actually in. */
    ROW("STATUS", AleksRA_StatusLine());
    ROW("GAME", AleksRA_GameLine());
    ROW(AleksRA_IsLoggedIn() ? "LOG OUT" : "LOG IN",
        AleksRA_IsLoggedIn() ? (AleksRA_UserName() ? AleksRA_UserName() : "") : "");
    /* Hardcore is reported, never offered -- see aleks_ra.c. */
    ROW("HARDCORE", AleksRA_IsHardcore() ? "ON" : "NOT SUPPORTED");
    break;

  case SW_SCREEN:
    ROW("DISPLAY MODE", switch_display_label());
    /* Aspect is the engine's, not the presentation's: this asks Zelda3 to
     * render 4:3 or true widescreen, it never stretches a finished frame. */
    ROW("ASPECT", AleksAspect_Label(SS_GetAspect()));
    /* Presentation, not rendering: how the finished frame is SHAPED.  Sits
     * next to ASPECT because the two together decide what the player sees,
     * but they are independent -- see AleksLayout_FrameAspect. */
    ROW("PIXEL ASPECT", g_config.aleks_square_pixels ? "SQUARE 1:1" : "TV 7:6");
    /* Esteban fixed camera.  Only meaningful while WIDE is on. */
    ROW("WIDE CAMERA", SS_GetWideEdgeMode() == 1 ? "FIXED" : "STANDARD");
    /* No COMPANION LAYOUT row.  TALL has no implementation behind it: the
     * companion is a fixed 4:3 surface that must not be stretched, so a
     * "taller" companion needs the donor pages reflowed, which is its own
     * job.  The key is still parsed for compatibility. */
    ROW("SCREEN ORDER", g_config.aleks_screen_order ? "COMPANION FIRST" : "GAME FIRST");
    SUB("DUAL LAYOUT");
    SUB("FLIP LAYOUT");
    break;

  case SW_DUAL:
    ROW("GAME SIZE", FMT("%u", g_config.aleks_dual_game_scale));
    ROW("COMPANION SIZE", FMT("%u", g_config.aleks_dual_companion_scale));
    ROW("GAP", FMT("%u", g_config.aleks_dual_gap));
    break;

  case SW_FLIP:
    ROW("GAME SIZE", FMT("%u", g_config.aleks_flip_game_scale));
    ROW("COMPANION SIZE", FMT("%u", g_config.aleks_flip_companion_scale));
    ROW("GAP", FMT("%u", g_config.aleks_flip_gap));
    break;

  case SW_GAMEPLAY:
    ROW("TOP HUD", g_config.aleks_hud_mode == 1 ? "ON" :
                   g_config.aleks_hud_mode == 2 ? "OFF" : "AUTO");
    ROW("COMPANION HUD", sw_onoff(g_config.aleks_companion_hud));
    ROW("TAP TO EQUIP", sw_onoff(g_config.aleks_tap_equip));
    ROW("X ITEM RING", sw_onoff(g_config.aleks_x_item_ring));
    SUB("QOL");
    SUB("ADVANCED");
    break;

  /* The engine's own quality-of-life flags (features.h).  Nothing here
   * implements behaviour -- each row toggles the native feature bit. */
  case SW_QOL:
    for (int i = 0; i < (int)(sizeof(kQolMask) / sizeof(kQolMask[0])); i++)
      ROW(kQolLabel[i], sw_onoff((feats & kQolMask[i]) != 0));
    /* Not an engine feature bit -- an ALEKS config field -- so it lives after
     * the mask-driven rows rather than inside kQolMask. */
    ROW("HOLD TO ADVANCE", sw_onoff(g_config.aleks_hold_to_advance != 0));
    break;

  /* Kept apart because it deliberately changes more behaviour than the rest. */
  case SW_ADVANCED:
    ROW("GAME CHANGING FIXES", sw_onoff((feats & kFeatures0_GameChangingBugFixes) != 0));
    break;

  case SW_CONTROLS:
    /* Cheap, and it keeps the rows honest after a queued reset lands. */
    SS_GetGamepadControls(pad_controls);
    for (int i = 0; i < 12; i++)
      ROW(kPadCmdNames[i], sw_button_label(pad_controls[i]));
    SUB("ALEKS SHORTCUTS");
    ROW("RESET DEFAULTS", NULL);
    break;

  case SW_SHORTCUTS:
    for (int i = 0; i < kAleksShortcut_Count; i++)
      ROW(kShortcutActionName[i], sw_shortcut_label(i));
    /* Quick Items live with the other shortcut bindings because they compete
     * for the same physical controls (see sw_choice_taken). */
    ROW("QUICK ITEM 1", sw_quick_item_label(0));
    ROW("QUICK ITEM 1 BUTTON", sw_quick_item_button_label(0));
    ROW("QUICK ITEM 2", sw_quick_item_label(1));
    ROW("QUICK ITEM 2 BUTTON", sw_quick_item_button_label(1));
    /* Shown 1-based; stored 0-based (backend slot 0 is the autosave). */
    ROW("QUICK SLOT", FMT("%u", (unsigned)(g_config.aleks_quick_slot + 1)));
    break;

  case SW_AUDIO:
    ROW("MSU1 MUSIC", sw_msu_pack() ? sw_onoff(g_config.enable_msu)
                                    : "NO PACK");
    break;

  case SW_SYSTEM:
    SUB("SAVE STATES");
    ROW("AUTOSAVE", sw_onoff(g_config.autosave != 0));
    ROW("RESET GAME", NULL);
    /* Only languages with a usable pack are ever listed, so this row shows
     * ENGLISH alone until one is extracted. */
    ROW("LANGUAGE", AleksLang_DisplayAt(AleksLang_CurrentIndex()));
    break;

  case SW_STATES:
    for (int i = 0; i < SW_STATE_SLOTS; i++) {
      const char *v = sw_state_flash == i && SDL_GetTicks() < sw_state_flash_until ?
        sw_state_flash_msg : (sw_state_stamp[i] ? "SAVE   LOAD" : "EMPTY");
      ROW(FMT("SLOT %d", i + 1), v);
    }
    break;
  }
  #undef ROW
  #undef SUB
  #undef FMT
  sw_row_count = n;
  return n;
}

static const char *sw_menu_title(int menu) {
  switch (menu) {
  case SW_SCREEN:   return "SCREEN";
  case SW_DUAL:     return "DUAL LAYOUT";
  case SW_FLIP:     return "FLIP LAYOUT";
  case SW_GAMEPLAY: return "GAMEPLAY";
  case SW_QOL:      return "QOL";
  case SW_ADVANCED: return "ADVANCED";
  case SW_SHORTCUTS: return "ALEKS SHORTCUTS";
  case SW_CONTROLS: return "CONTROLS";
  case SW_AUDIO:    return "AUDIO";
  case SW_SYSTEM:   return "SYSTEM";
  case SW_RETRO:    return "RETROACHIEVEMENTS";
  case SW_STATES:   return "SAVE STATES";
  default:          return "SETTINGS";
  }
}

/* ---- painter ---------------------------------------------------------- */
static void sw_draw_chevron(RectFS row) {
  float ax = row.x + row.w - 26 * u, ay = row.y + row.h / 2;
  for (float d = 0; d < 10 * u; d += 1.0f) {
    fill_rect(ax - 6 * u + d, ay - 8 * u + d * 0.8f, 4 * u, 2 * u, COL_GOLD);
    fill_rect(ax - 6 * u + d, ay + 8 * u - d * 0.8f - 2 * u, 4 * u, 2 * u, COL_GOLD);
  }
}

/* SAVE STATES draws its own card: thumbnail on the left, state on the right.
 * Same row frame as every other menu, so it still reads as one menu. */
static void sw_draw_state_row(RectFS row, int slot, bool armed) {
  draw_settings_row(&row, armed);
  float pad = 6 * u;
  float th = row.h - pad * 2;
  float tw = th * kSsThumbW / kSsThumbH;
  if (sw_state_thumb[slot]) {
    SDL_FRect dst = { row.x + pad, row.y + pad, tw, th };
    SDL_RenderCopyF(ss_r, sw_state_thumb[slot], NULL, &dst);
  } else {
    fill_rect(row.x + pad, row.y + pad, tw, th, COL(20, 20, 20));
  }
  float tx = row.x + pad * 2 + tw;
  float ty = row.y + row.h / 2 - 8 * u;
  draw_text_fit(sw_rows[slot].label, tx, ty, 2 * u, row.w - (tx - row.x) - 90 * u);
  draw_text_right_fit(sw_rows[slot].value, row.x + row.w - 12 * u, ty, 1.8f * u,
                      86 * u);
}

/* ------------------------------------------------------------------ *
 * GUIDE.  Architecture only in this pass: the page exists, participates in
 * the one page enum, and renders through the same donor frame -- but it has
 * no progression rules and no prose of its own.  Everything it shows comes
 * from story_guide.c as STRING KEYS, so the future content pass writes rules
 * and a string table without touching this renderer.
 * ------------------------------------------------------------------ */
static void draw_guide(RectFS r) {
  StoryGuideEntry entry;
  menu_box(r, COL_BOX_BORDER);
  draw_text("GUIDE", r.x + r.w / 2 - text_width("GUIDE", 3 * u) / 2, r.y + 16 * u, 3 * u);

  StoryGuide_GetCurrentEntry(&entry);

  float y = r.y + 60 * u;
  float inner = r.w - 32 * u;
  draw_text_fit(StoryGuide_Text(entry.heading_key), r.x + 16 * u, y, 2 * u, inner);
  y += 26 * u;
  draw_text_fit(StoryGuide_Text(entry.objective_key), r.x + 16 * u, y, 1.8f * u, inner);
  if (entry.hint_key) {
    y += 30 * u;
    draw_text_fit(StoryGuide_Text(entry.hint_key), r.x + 16 * u, y, 1.6f * u, inner);
  }
  if (entry.detail_key) {
    y += 24 * u;
    draw_text_fit(StoryGuide_Text(entry.detail_key), r.x + 16 * u, y, 1.6f * u, inner);
  }
}

/* SAVE STATES is a top-level page that borrows the settings row machinery, so
 * "which menu am I drawing" is not always the settings stack. */
static int sw_current_menu(void) {
  return tab == TAB_SAVE ? SW_STATES : sw_stack[sw_depth];
}

#ifdef __SWITCH__
/*
 * The save-state confirmation.  Drawn last, over everything, because it owns
 * the input for as long as it is up -- a modal that the page can show through
 * but that still eats the presses would be the invisible-menu bug again.
 *
 * NO sits on the left and is selected by default, so a reflexive A can never
 * overwrite a slot.
 */
static void draw_confirm_modal(void) {
  if (!sw_confirm_active) return;

  /* Dim the whole companion surface first. */
  SDL_SetRenderDrawBlendMode(ss_r, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(ss_r, 0, 0, 0, 170);
  { SDL_Rect all = {0, 0, W, H}; SDL_RenderFillRect(ss_r, &all); }

  RectFS box = { W * 0.10f, H * 0.28f, W * 0.80f, H * 0.44f };
  menu_box(box, COL_BOX_BORDER);

  const char *title =
      sw_confirm_kind == kConfirm_LanguageRestart ? "RESTART TO APPLY LANGUAGE?"
      : sw_confirm_quick ? "SAVE TO QUICK SLOT?"
      : (sw_confirm_overwrite ? "OVERWRITE SAVE STATE?" : "SAVE STATE?");
  draw_text_fit(title, box.x + box.w / 2 - text_width(title, 2.4f * u) / 2,
                box.y + 26 * u, 2.4f * u, box.w - 36 * u);

  {
    char sub[48];
    if (sw_confirm_kind == kConfirm_LanguageRestart)
      /* The setting is already saved either way; this only offers to apply it
       * now instead of on the next launch. */
      snprintf(sub, sizeof(sub), "%s ON NEXT LAUNCH",
               AleksLang_DisplayAt(AleksLang_CurrentIndex()));
    else
      snprintf(sub, sizeof(sub), "SLOT %d", sw_confirm_slot + 1);
    draw_text_fit(sub, box.x + box.w / 2 - text_width(sub, 2 * u) / 2,
                  box.y + 60 * u, 2 * u, box.w - 36 * u);
  }

  float bw = box.w * 0.34f, bh = 46 * u;
  float by = box.y + box.h - bh - 26 * u;
  sw_confirm_no_r  = (RectFS){ box.x + box.w * 0.10f, by, bw, bh };
  sw_confirm_yes_r = (RectFS){ box.x + box.w * 0.56f, by, bw, bh };

  draw_settings_row(&sw_confirm_no_r,  sw_confirm_choice == 0);
  draw_settings_row(&sw_confirm_yes_r, sw_confirm_choice == 1);
  draw_text("NO", sw_confirm_no_r.x + sw_confirm_no_r.w / 2 - text_width("NO", 2.2f * u) / 2,
            sw_confirm_no_r.y + sw_confirm_no_r.h / 2 - 8 * u, 2.2f * u);
  draw_text("YES", sw_confirm_yes_r.x + sw_confirm_yes_r.w / 2 - text_width("YES", 2.2f * u) / 2,
            sw_confirm_yes_r.y + sw_confirm_yes_r.h / 2 - 8 * u, 2.2f * u);
}
#endif

static void draw_settings(RectFS r) {
  int menu = sw_current_menu();
  int count = sw_build(menu);
  float row_h = menu == SW_STATES ? 44 * u : 40 * u;
  float gap = 8 * u;
  float y0 = r.y + 56 * u;
  float list_h = r.y + r.h - 10 * u - y0;

  menu_box(r, COL_BOX_BORDER);

  /* Title, and a BACK affordance on every sub-menu (B does the same thing;
   * the box is what makes it discoverable and touch-reachable). */
  {
    const char *title = sw_menu_title(menu);
    draw_text_fit(title, r.x + r.w / 2 - text_width(title, 3 * u) / 2,
                  r.y + 16 * u, 3 * u, r.w - 40 * u);
  }
  sw_back_r = (RectFS){0, 0, 0, 0};
  if (sw_depth > 0) {
    sw_back_r = (RectFS){r.x + 12 * u, r.y + 10 * u, 74 * u, 32 * u};
    draw_settings_row(&sw_back_r, false);
    draw_text("BACK", sw_back_r.x + sw_back_r.w / 2 - text_width("BACK", 2 * u) / 2,
              sw_back_r.y + sw_back_r.h / 2 - 8 * u, 2 * u);
  }

  sw_visible = (int)((list_h + gap) / (row_h + gap));
  if (sw_visible > SW_MAX_VISIBLE) sw_visible = SW_MAX_VISIBLE;
  if (sw_visible < 1) sw_visible = 1;

  /* Keep the cursor on screen; the list scrolls rather than the rows shrinking
   * (long menus like CONTROLS have 13 rows). */
  if (sw_sel >= 0) {
    if (sw_sel < sw_scroll) sw_scroll = sw_sel;
    if (sw_sel >= sw_scroll + sw_visible) sw_scroll = sw_sel - sw_visible + 1;
  }
  if (sw_scroll > count - sw_visible) sw_scroll = count - sw_visible;
  if (sw_scroll < 0) sw_scroll = 0;

  sw_up_r = sw_down_r = (RectFS){0, 0, 0, 0};
  for (int v = 0; v < sw_visible; v++) {
    int i = sw_scroll + v;
    if (i >= count) break;
    RectFS row = { r.x + 24 * u, y0 + v * (row_h + gap), r.w - 48 * u, row_h };
    sw_row_r[v] = row;
    if (menu == SW_STATES) { sw_draw_state_row(row, i, i == sw_sel); continue; }

    draw_settings_row(&row, i == sw_sel);
    float ty = row.y + row.h / 2 - 8 * u;
    float inner = row.w - 24 * u;
    if (sw_rows[i].submenu) {
      draw_text_fit(sw_rows[i].label, row.x + 12 * u, ty, 2 * u, inner - 30 * u);
      sw_draw_chevron(row);
    } else if (sw_rows[i].value) {
      /* The value takes what it needs up to 45% of the row, the label gets
         the rest -- both shrink to fit rather than clipping. */
      float value_w = inner * 0.45f;
      draw_text_right_fit(sw_rows[i].value, row.x + row.w - 12 * u, ty, 2 * u, value_w);
      draw_text_fit(sw_rows[i].label, row.x + 12 * u, ty, 2 * u, inner - value_w - 8 * u);
    } else {
      draw_text_fit(sw_rows[i].label, row.x + 12 * u, ty, 2 * u, inner);
    }
  }

  /* Scroll affordances, tappable. */
  if (count > sw_visible) {
    float ax = r.x + r.w - 20 * u;
    if (sw_scroll > 0) {
      sw_up_r = (RectFS){ax - 12 * u, y0 - 18 * u, 24 * u, 18 * u};
      tri_up(ax, sw_up_r.y + 2 * u, 7 * u, COL_GOLD);
    }
    if (sw_scroll + sw_visible < count) {
      float by = y0 + sw_visible * (row_h + gap) - gap + 2 * u;
      sw_down_r = (RectFS){ax - 12 * u, by, 24 * u, 18 * u};
      for (int i = 0; i < (int)(7 * u); i++)
        fill_rect(ax - (7 * u - i), by + i, (7 * u - i) * 2, 1, COL_GOLD);
    }
  }
}

/* ---- activation ------------------------------------------------------- */
static void sw_push(int menu) {
  if (sw_depth + 1 < (int)(sizeof(sw_stack) / sizeof(sw_stack[0]))) {
    sw_stack[++sw_depth] = (uint8)menu;
    sw_sel = -1;
    sw_scroll = 0;
  }
  if (menu == SW_CONTROLS || menu == SW_SHORTCUTS) {
    SS_GetGamepadControls(pad_controls);
    sw_sanitize_bindings();   /* a mangled ini heals on the way in */
  }
  if (menu == SW_STATES) sw_state_refresh_all();
}

static bool sw_pop(void) {
  if (sw_depth == 0) return false;
  sw_depth--;
  sw_sel = -1;
  sw_scroll = 0;
  return true;
}

/* dir is +1 for a plain activate (tap, A, right) and -1 for left, so value
 * rows that cycle several ways get real previous/next semantics.  Two-state
 * toggles ignore it and just toggle. */
static void sw_activate(int menu, int row, int dir) {
  unsigned feats = SS_GetFeatures();
  switch (menu) {
  case SW_ROOT:
    sw_push(row == 0 ? SW_GAMEPLAY : row == 1 ? SW_SCREEN :
            row == 2 ? SW_CONTROLS : row == 3 ? SW_AUDIO :
            row == 4 ? SW_RETRO : SW_SYSTEM);
    return;

  case SW_RETRO:
    if (row == 0) {
      bool on = !AleksRA_IsEnabled();
      AleksRA_SetEnabled(on);
      update_ini("[General]", "AleksRetroAchievements", on ? "true" : "false");
    } else if (row == 3) {
      if (AleksRA_IsLoggedIn()) AleksRA_Logout();
      else                      AleksRA_InteractiveLogin();
    }
    return;

  case SW_SCREEN:
    switch (row) {
    case 0: {
      static const char *const kName[3] = { "Normal", "Dual", "Flip" };
      int from = g_config.aleks_display_mode % 3;
      int to = (from + (dir < 0 ? 2 : 1)) % 3;
      /* Edge-driven: one line per request, which is what makes a log that ends
       * abruptly still say what was being asked for. */
      AleksCrash_DisplayRequest(kName[from], kName[to]);
      g_config.aleks_display_mode = (uint8)to;
      /* Keep an interactive companion session visible across the switch --
       * this row is exactly where the invisible-menu bug was reported. */
      AleksCompositor_NotifyDisplayModeChanged(from, to);
      save_display_mode();
      return;
    }
    case 1: {
      /* Three-way now: 4:3 -> WIDE -> TRUE 16:9.  dir < 0 steps back, so the
       * row is usable in both directions like every other cycling row. */
      int from = SS_GetAspect();
      int to = AleksAspect_NextInCycle(from, dir);
      AleksCrash_AspectRequest(AleksAspect_Label(from), AleksAspect_Label(to));
      SS_SetAspect(to);
      update_ini("[General]", "AleksGameplayAspect", AleksAspect_IniValue(to));
      return;
    }
    case 2: {
      /* No texture work and no engine reconfigure: the compositor recomputes
       * the layout every present, so this lands on the next frame.  That is
       * why it does not go anywhere near the aspect-change safepoint or the
       * WIDE-return protections. */
      bool square = !g_config.aleks_square_pixels;
      g_config.aleks_square_pixels = square;
      update_ini("[General]", "AleksSquarePixels", square ? "true" : "false");
      StartupLog("PIXEL ASPECT: %s", square ? "SQUARE 1:1" : "TV 7:6");
      return;
    }
    case 3: {
      int fixed = SS_GetWideEdgeMode() == 1 ? 0 : 1;
      SS_SetWideEdgeMode(fixed);
      update_ini("[General]", "AleksWideCamera", fixed ? "Fixed" : "Standard");
      return;
    }
    case 4:
      g_config.aleks_screen_order = !g_config.aleks_screen_order;
      update_ini("[General]", "AleksScreenOrder",
                 g_config.aleks_screen_order ? "CompanionFirst" : "GameFirst");
      return;
    case 5: sw_push(SW_DUAL); return;
    case 6: sw_push(SW_FLIP); return;
    }
    return;

  case SW_DUAL:
  case SW_FLIP: {
    bool flip = menu == SW_FLIP;
    int step = dir < 0 ? -10 : 10;
    if (row == 0) {
      if (flip) {
        g_config.aleks_flip_game_scale = cycle_pct(g_config.aleks_flip_game_scale, 50, 100, step);
        ini_set_int("AleksFlipGameScale", g_config.aleks_flip_game_scale);
      } else {
        g_config.aleks_dual_game_scale = cycle_pct(g_config.aleks_dual_game_scale, 50, 150, step);
        ini_set_int("AleksDualGameScale", g_config.aleks_dual_game_scale);
      }
    } else if (row == 1) {
      if (flip) {
        g_config.aleks_flip_companion_scale = cycle_pct(g_config.aleks_flip_companion_scale, 50, 110, step);
        ini_set_int("AleksFlipCompanionScale", g_config.aleks_flip_companion_scale);
      } else {
        g_config.aleks_dual_companion_scale = cycle_pct(g_config.aleks_dual_companion_scale, 50, 150, step);
        ini_set_int("AleksDualCompanionScale", g_config.aleks_dual_companion_scale);
      }
    } else if (row == 2) {
      int gap = flip ? g_config.aleks_flip_gap : g_config.aleks_dual_gap;
      gap += dir < 0 ? -8 : 8;
      if (gap > 128) gap = 0;
      if (gap < 0) gap = 128;
      if (flip) {
        g_config.aleks_flip_gap = (uint8)gap;
        ini_set_int("AleksFlipGap", gap);
      } else {
        g_config.aleks_dual_gap = (uint8)gap;
        ini_set_int("AleksDualGap", gap);
      }
    }
    return;
  }

  case SW_GAMEPLAY:
    switch (row) {
    case 0:
      g_config.aleks_hud_mode = (uint8)((g_config.aleks_hud_mode + 1) % 3);
      update_ini("[General]", "AleksHudMode",
                 g_config.aleks_hud_mode == 1 ? "On" :
                 g_config.aleks_hud_mode == 2 ? "Off" : "Auto");
      return;
    case 1:
      g_config.aleks_companion_hud = !g_config.aleks_companion_hud;
      update_ini("[General]", "AleksCompanionHud",
                 g_config.aleks_companion_hud ? "true" : "false");
      return;
    case 2:
      g_config.aleks_tap_equip = !g_config.aleks_tap_equip;
      update_ini("[General]", "AleksTapToEquip",
                 g_config.aleks_tap_equip ? "true" : "false");
      return;
    case 3: {
      /* Donor rule: the X ring is an ItemSwitchLR affordance, so turning it on
       * turns that on too rather than leaving a ring that assigns nothing. */
      bool on = !g_config.aleks_x_item_ring;
      g_config.aleks_x_item_ring = on;
      if (on && !(feats & kFeatures0_SwitchLR)) {
        SS_SetFeature(kFeatures0_SwitchLR, true);
        /* The feature it silently turned on has to persist too, or the ring
         * comes back next launch with nothing behind it. */
        update_ini("[Features]", "ItemSwitchLR", "1");
      }
      update_ini("[General]", "AleksXItemRing", on ? "true" : "false");
      return;
    }
    case 4: sw_push(SW_QOL); return;
    case 5: sw_push(SW_ADVANCED); return;
    }
    return;

  case SW_QOL: {
    int n = (int)(sizeof(kQolMask) / sizeof(kQolMask[0]));
    if (row == n) {                       /* the extra ALEKS row */
      bool on = !g_config.aleks_hold_to_advance;
      g_config.aleks_hold_to_advance = on ? 1 : 0;
      update_ini("[General]", "AleksHoldToAdvance", on ? "true" : "false");
      return;
    }
    if (row < 0 || row >= n) return;
    bool on = (feats & kQolMask[row]) == 0;
    SS_SetFeature(kQolMask[row], on);
    /* ...and persist it, which is the half that was missing. */
    update_ini("[Features]", kQolIniKey[row], on ? "1" : "0");
    /* Turning ItemSwitchLR off would strand the X ring. */
    if (kQolMask[row] == kFeatures0_SwitchLR && !on && g_config.aleks_x_item_ring) {
      g_config.aleks_x_item_ring = false;
      update_ini("[General]", "AleksXItemRing", "false");
    }
    return;
  }

  case SW_ADVANCED:
    if (row == 0) {
      bool on = (feats & kFeatures0_GameChangingBugFixes) == 0;
      SS_SetFeature(kFeatures0_GameChangingBugFixes, on);
      update_ini("[Features]", "GameChangingBugFixes", on ? "1" : "0");
    }
    return;

  case SW_SHORTCUTS:
    if (row >= 0 && row < kAleksShortcut_Count) {
      sw_cycle_shortcut(row, dir);
    } else switch (row - kAleksShortcut_Count) {
    case 0: sw_cycle_quick_item_slot(0, dir); break;
    case 1: sw_cycle_quick_item_button(0, dir); break;
    case 2: sw_cycle_quick_item_slot(1, dir); break;
    case 3: sw_cycle_quick_item_button(1, dir); break;
    case 4: {
      int slot = g_config.aleks_quick_slot + (dir < 0 ? -1 : 1);
      if (slot < 0) slot = SW_STATE_SLOTS - 1;
      if (slot >= SW_STATE_SLOTS) slot = 0;
      g_config.aleks_quick_slot = (uint8)slot;
      ini_set_int("AleksQuickSlot", slot + 1);   /* persisted 1-based */
      break;
    }
    }
    return;

  case SW_CONTROLS:
    if (row < 12) {
      /* LEFT/RIGHT step the binding; A (dir 1) also steps forward, so the row
       * is usable without knowing the gesture. */
      sw_cycle_binding(row, dir);
      return;
    }
    if (row == 12) { sw_push(SW_SHORTCUTS); return; }
    if (row == 13) {
      SS_SetGamepadControls(NULL);        /* NULL = restore defaults (queued) */
      sw_controls_dirty = true;           /* persist once it has landed */
    }
    return;

  case SW_AUDIO:
    if (row == 0 && sw_msu_pack()) {
      g_config.enable_msu = !g_config.enable_msu;
      update_ini("[Sound]", "EnableMSU", g_config.enable_msu ? "1" : "0");
    }
    return;

  case SW_SYSTEM:
    /* SAVE STATES is a top-level page now, not a settings sub-menu.  This row
     * is a shortcut TO that page, so there is one save-state page with one
     * selection and one modal rather than two copies that can disagree. */
    if (row == 0) { SecondScreenSDL_SetTab(TAB_SAVE); return; }
    if (row == 1) {
      g_config.autosave = !g_config.autosave;
      SS_SetAutosave(g_config.autosave != 0);
      update_ini("[General]", "Autosave", g_config.autosave ? "1" : "0");
      return;
    }
    if (row == 2) { SS_RequestRestart(); return; }
    if (row == 3) {
      /* Cycles only over languages that actually have a pack, so an
       * unavailable one can never be selected. */
      /*
       * The setting is stored and applied AT THE NEXT BOOT, not live.
       *
       * ZeldaSetLanguage swaps the dialogue and font blocks the text engine is
       * using.  Doing that to a running game is the risky path -- it is what
       * crashed on the way back to English -- while doing it once at startup,
       * before anything has drawn text, is the path that is known to work.  So
       * the row only records the choice, and offers to restart.
       */
      int n = AleksLang_Count();
      int at = (AleksLang_CurrentIndex() + (dir < 0 ? n - 1 : 1)) % n;
      const char *code = AleksLang_CodeAt(at);
      g_config.language = code;             /* the row reads this back */
      update_ini("[General]", "Language", code ? code : "en");
      sw_confirm_active = true;
      sw_confirm_kind = kConfirm_LanguageRestart;
      sw_confirm_choice = 0;                /* NO: never restart unasked */
      sw_confirm_quick = false;
      sw_confirm_restore_tab = -1;
      sw_confirm_close_overlay = false;
    }
    return;

  case SW_STATES:
    /* Empty slot: only SAVE makes sense.  Used slot: the right half is LOAD,
     * the left half SAVE -- resolved from the tap x in sw_hit_state. */
    return;
  }
}

static void sw_state_flash_now(int slot, const char *msg) {
  sw_state_flash = slot;
  sw_state_flash_msg = msg;
  sw_state_flash_until = SDL_GetTicks() + 900;
}

/* SAVE is always gated behind the modal, empty slot included. */
static void sw_state_arm_save(int slot, bool quick) {
  if (slot < 0 || slot >= SW_STATE_SLOTS) return;
  sw_confirm_active = true;
  sw_confirm_kind = kConfirm_Save;
  sw_confirm_slot = slot;
  sw_confirm_overwrite = sw_state_stamp[slot] != 0;
  sw_confirm_quick = quick;
  sw_confirm_choice = 0;            /* NO by default: never a stray overwrite */
}

/* LOAD is immediate -- the deliberate asymmetry: a save can destroy work, a
 * load cannot.  An empty slot does nothing but say so. */
static void sw_state_load(int slot) {
  if (slot < 0 || slot >= SW_STATE_SLOTS) return;
  if (!sw_state_stamp[slot]) { sw_state_flash_now(slot, "EMPTY"); return; }
  SS_RequestLoadState(sw_backend_slot(slot));
  sw_state_flash_now(slot, "LOADED");
}

static void sw_confirm_clear(void) {
  sw_confirm_active = false;
  sw_confirm_quick = false;
  sw_confirm_restore_tab = -1;
  sw_confirm_close_overlay = false;
}

/* Touch on a SAVE STATES card: left half saves (confirmed), right half loads. */
static void sw_state_action(int slot, float x) {
  bool load = sw_state_stamp[slot] &&
              x > sw_row_r[0].x + sw_row_r[0].w * 0.72f;
  if (load) sw_state_load(slot);
  else      sw_state_arm_save(slot, false);
}

/* Poll the armed remap capture.  Called from the draw so it lands on the same
 * cadence as the row that shows PRESS A BUTTON. */
/* A binding change is applied on the game thread, so the ini write waits one
 * UI frame for it to land -- otherwise a RESET DEFAULTS would persist the
 * pre-reset values. */
static void sw_poll_remap(void) {
  if (!sw_controls_dirty) return;
  sw_controls_dirty = false;
  SS_GetGamepadControls(pad_controls);
  /* What comes back can contain -1 for any command the mapper has no entry
   * for -- repair before it is written back out. */
  sw_sanitize_bindings();
  write_ini_gamepad_controls();
}
#else
static void draw_settings(RectFS r) {
  menu_box(r, COL_BOX_BORDER);
  if (remap_mode) {
    draw_remap_panel(r);
    return;
  }
  if (screen_mode) {
    draw_screen_panel(r);
    return;
  }
  if (developer_mode) {
    if (developer_overlay_mode)
      draw_developer_overlay_panel(r);
    else
      draw_developer_panel(r);
    return;
  }
  draw_text("SETTINGS", r.x + r.w / 2 - text_width("SETTINGS", 3 * u) / 2, r.y + 18 * u, 3 * u);

  char turbo_value[12];
#ifdef __3DS__
  int turbo_multiplier = Platform3DS_GetTurboMultiplier();
  if (turbo_multiplier <= 0)
    snprintf(turbo_value, sizeof(turbo_value), "OFF");
  else
    snprintf(turbo_value, sizeof(turbo_value), "X%d", turbo_multiplier);
#else
  snprintf(turbo_value, sizeof(turbo_value), "X5");
#endif
  static const char *const labels[6] = {
    "SCREEN", "TURBO SPEED", "REMAP BUTTONS",
    "DEVELOPER", "RESTART", "SELECT ROM",
  };
  const char *values[6] = {
    "", turbo_value, "", "", NULL, NULL,
  };
  float row_h = 44 * u, gap = 8 * u;
  float y0 = r.y + 55 * u;
  for (int i = 0; i < 6; i++) {
    RectFS *row = &settings_row_r[i];
    *row = (RectFS){r.x + 28 * u, y0 + i * (row_h + gap), r.w - 56 * u, row_h};
    draw_settings_row(row, false);
    float ty = row->y + row->h / 2 - 8 * u;
    draw_text(labels[i], row->x + 16 * u, ty, 2 * u);
    if (!values[i]) {
      continue;
    }
    if (values[i][0] == 0) {
      // chevron for sub-screens
      float ax = row->x + row->w - 26 * u, ay = row->y + row->h / 2;
      for (float d = 0; d < 10 * u; d += 1.0f) {
        fill_rect(ax - 6 * u + d, ay - 8 * u + d * 0.8f, 4 * u, 2 * u, COL_GOLD);
        fill_rect(ax - 6 * u + d, ay + 8 * u - d * 0.8f - 2 * u, 4 * u, 2 * u, COL_GOLD);
      }
    } else {
      draw_text(values[i], row->x + row->w - 16 * u - text_width(values[i], 2 * u), ty, 2 * u);
    }
  }
}

#endif  // __SWITCH__

static void draw_tab_button(RectFS r, const char *label, bool active) {
  uint32_t bg = active ? COL(40, 34, 12) : COL_BOX;
  fill_round(r.x, r.y, r.w, r.h, 10 * u, bg);
  fill_round(r.x + 3 * u, r.y + 3 * u, r.w - 6 * u, r.h - 6 * u, 8 * u,
             active ? COL_GOLD : COL_BOX_BORDER2);
  fill_round(r.x + 7 * u, r.y + 7 * u, r.w - 14 * u, r.h - 14 * u, 6 * u, bg);
  float s = 3 * u;
  if (label)
    draw_text(label, r.x + r.w / 2 - text_width(label, s) / 2, r.y + r.h / 2 - 4 * s, s);
}

static void draw_tab_bar(float tab_h) {
  float y = H - tab_h + 4 * u;
  float bh = tab_h - 16 * u;
  float sq = bh;   // square settings button on the right
  tab_settings_r = (RectFS){W - 8 * u - sq, y, sq, bh};
  float x0 = 8 * u, xr = tab_settings_r.x - 8 * u, tgap = 8 * u;
#ifdef __SWITCH__
  /* Five equal buttons: GEAR | MAP | ITEMS | SAVE | GUIDE, left of the
   * settings cog.  Every destination in the page enum gets a real tab rather
   * than being reachable only by controller. */
  int slots = 5;
#else
  int slots = 3;
#endif
  float bw = (xr - x0 - (slots - 1) * tgap) / (float)slots;
  tab_gear_r  = (RectFS){x0, y, bw, bh};
  tab_map_r   = (RectFS){x0 + bw + tgap, y, bw, bh};
  tab_items_r = (RectFS){x0 + 2 * (bw + tgap), y, bw, bh};
  draw_tab_button(tab_gear_r, "GEAR", tab == TAB_GEAR);
  draw_tab_button(tab_map_r, "MAP", tab == TAB_MAP);
  draw_tab_button(tab_items_r, "ITEMS", tab == TAB_ITEMS);
#ifdef __SWITCH__
  tab_save_r  = (RectFS){x0 + 3 * (bw + tgap), y, bw, bh};
  draw_tab_button(tab_save_r, "SAVE", tab == TAB_SAVE);
  tab_guide_r = (RectFS){x0 + 4 * (bw + tgap), y, bw, bh};
  draw_tab_button(tab_guide_r, "GUIDE", tab == TAB_GUIDE);
#endif
  draw_tab_button(tab_settings_r, NULL, tab == TAB_SETTINGS);
  draw_cog(tab_settings_r.x + tab_settings_r.w / 2, tab_settings_r.y + tab_settings_r.h / 2,
           bh * 0.28f);
}

// public API

static SDL_Window *main_win;
static bool ss_enabled;
#ifdef __3DS__
static uint8_t *ss_present_pixels[2];
static bool ss_is_new_3ds;
static int ss_front_buffer = -1;
static int ss_worker_buffer;
static bool ss_worker_busy;
static bool ss_frame_ready;
static uint32_t ss_redraw_requests;
static bool ss_worker_sidebar_patch;
static bool ss_worker_running;
static Thread ss_worker_thread;
static LightEvent ss_worker_start;
static LightEvent ss_worker_done;
static int ss_worker_logic_frames;
static s32 ss_worker_idle_priority;
static s32 ss_worker_interactive_priority;
static bool ss_worker_interactive;
static uint64_t ss_full_redraw_count;
static uint64_t ss_full_redraw_total_ticks;
static uint64_t ss_full_redraw_max_ticks;
static uint64_t ss_patch_redraw_count;
static uint64_t ss_patch_redraw_total_ticks;
static uint64_t ss_patch_redraw_max_ticks;
static bool ss_touch_redraw_pending;
static uint64_t ss_touch_request_ticks;
static uint64_t ss_worker_touch_request_ticks;
static uint64_t ss_touch_redraw_count;
static uint64_t ss_touch_redraw_total_ticks;
static uint64_t ss_touch_redraw_max_ticks;
enum {
  k3DSBottomTextureWidth = 512,
  k3DSBottomTextureHeight = 256,
};
static void draw_second_screen(int logic_frames);

static int bottom_bytes_per_pixel(void) {
  return ss_is_new_3ds ? 4 : 2;
}

static int bottom_buffer_pitch(void) {
  return k3DSBottomTextureWidth * bottom_bytes_per_pixel();
}

static uint32_t bottom_sdl_pixel_format(void) {
  return ss_is_new_3ds ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_RGB565;
}

enum {
  kBottomRedrawHud = 1 << 0,
  kBottomRedrawFull = 1 << 1,
};

static void request_bottom_redraw(uint32_t request) {
  __atomic_fetch_or(&ss_redraw_requests, request, __ATOMIC_RELEASE);
}

static void prioritize_bottom_touch(void) {
  if (!ss_is_new_3ds && ss_worker_thread)
    svcSetThreadPriority(threadGetHandle(ss_worker_thread),
                         ss_worker_interactive_priority);
}
#endif

static void request_dump_now(void) {
  char dump_dir[160] = {0};
#ifdef __3DS__
  if (Platform3DS_CreateDumpDirectory(dump_dir, sizeof(dump_dir)) &&
      ss_front_buffer >= 0 && ss_present_pixels[ss_front_buffer]) {
    char path[192];
    snprintf(path, sizeof(path), "%s/bottom-screen.bmp", dump_dir);
    if (ss_is_new_3ds) {
      Platform3DS_SaveARGB8888Bmp(
        path, ss_present_pixels[ss_front_buffer], bottom_buffer_pitch(), W, H);
    } else {
      Platform3DS_SaveRGB565Bmp(
        path, ss_present_pixels[ss_front_buffer], bottom_buffer_pitch(), W, H);
    }
  }
#endif
  SS_RequestMemoryDump(dump_dir);
  dump_flash_until = SDL_GetTicks() + 1200;
}

bool SecondScreenSDL_Init(SDL_Window *main_window) {
#ifdef __3DS__
  main_win = main_window;
  ss_is_new_3ds = Platform3DS_IsNew3DS();
  ss_enabled = true;
  return true;
#else
  const char *env = SDL_getenv("ZELDA3_SECOND_SCREEN");
  if (!env || env[0] != '1') return false;
  main_win = main_window;
  ss_enabled = true;
  return true;
#endif
}

void SecondScreenSDL_RequestDump(void) {
  if (!ss_enabled)
    return;
  request_dump_now();
}

void SecondScreenSDL_SetDiagnostics(int current_fps, int average_fps) {
  ss_diag_current_fps = current_fps;
  ss_diag_average_fps = average_fps;
}

void SecondScreenSDL_OpenDeveloperOverlay(void) {
  tab = TAB_SETTINGS;
  leave_remap();
  screen_mode = false;
  developer_mode = true;
  developer_overlay_mode = true;
#ifdef __3DS__
  request_bottom_redraw(kBottomRedrawFull);
#endif
}

#ifdef __SWITCH__
static SDL_Texture *ss_target;

// The Switch has one window and one renderer.  "Ensuring the window" here
// means ensuring the render target that stands in for the 3DS bottom
// screen: a 320x240 texture the compositor then places and scales.  ss_win
// is set to the game window purely so the readiness checks below (and the
// donor's own !ss_win guards) keep reading naturally.
static bool ensure_window(void) {
  if (ss_win) return true;
  if (!main_win || !ss_r) return false;
  ss_target = SDL_CreateTexture(ss_r, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_TARGET,
                                ALEKS_COMPANION_W, ALEKS_COMPANION_H);
  if (!ss_target) {
    ss_enabled = false;
    return false;
  }
  SDL_SetTextureBlendMode(ss_target, SDL_BLENDMODE_NONE);
  SDL_SetTextureScaleMode(ss_target, SDL_ScaleModeNearest);
  // A fresh render target holds undefined pixels, and the compositor draws it
  // from the very next frame -- two frames before the first UI draw lands.
  if (SDL_SetRenderTarget(ss_r, ss_target) == 0) {
    SDL_SetRenderDrawColor(ss_r, 12, 12, 12, 255);
    SDL_RenderClear(ss_r);
    SDL_SetRenderTarget(ss_r, NULL);
  }
  W = ALEKS_COMPANION_W;
  H = ALEKS_COMPANION_H;
  u = unit_for_size(W, H);
  ss_win = main_win;
  ss_winid = SDL_GetWindowID(main_win);
  return true;
}
#else
// Create the bottom window lazily on the other display, after the game has
// drawn its first frames -- opening a second fullscreen window on the same
// output mid-init can resize the game window under its GL renderer.
static bool ensure_window(void) {
  if (ss_win) return true;

  printf("second screen: creating window...\n");
  fflush(stdout);
  int n = SDL_GetNumVideoDisplays();
#ifdef __3DS__
  int target = n > 1 ? 1 : 0;
#else
  int main_disp = main_win ? SDL_GetWindowDisplayIndex(main_win) : 0;
  if (main_disp < 0) main_disp = 0;
  int target = -1;
  for (int i = 0; i < n; i++)
    if (i != main_disp) { target = i; break; }
  if (target < 0) target = main_disp;
  const char *disp_env = SDL_getenv("ZELDA3_SECOND_SCREEN_DISPLAY");
  if (disp_env && disp_env[0]) {
    target = SDL_atoi(disp_env);
    if (target < 0 || target >= n) target = main_disp;
  }
#endif

  const char *title = SDL_getenv("ZELDA3_SECOND_SCREEN_TITLE");
  if (!title || !title[0]) title = "Zelda3 Bottom Screen";
  ss_win = SDL_CreateWindow(title,
                            SDL_WINDOWPOS_CENTERED_DISPLAY(target),
                            SDL_WINDOWPOS_CENTERED_DISPLAY(target),
#ifdef __3DS__
                            320, 240,
#else
                            640, 480,
#endif
                            SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_BORDERLESS);
  if (!ss_win) {
    fprintf(stderr, "second screen: CreateWindow failed: %s\n", SDL_GetError());
#ifdef __3DS__
    Platform3DS_LogRuntime("ERROR bottom window: %s", SDL_GetError());
#endif
    ss_enabled = false;
    return false;
  }
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
  ss_r = SDL_CreateRenderer(ss_win, -1, SDL_RENDERER_SOFTWARE);
  if (!ss_r) {
    fprintf(stderr, "second screen: CreateRenderer failed: %s\n", SDL_GetError());
#ifdef __3DS__
    Platform3DS_LogRuntime("ERROR bottom renderer: %s", SDL_GetError());
#endif
    SDL_DestroyWindow(ss_win); ss_win = NULL;
    ss_enabled = false;
    return false;
  }
  ss_winid = SDL_GetWindowID(ss_win);
  SDL_GetRendererOutputSize(ss_r, &W, &H);
#ifdef __3DS__
  W = 320;
  H = 240;
#endif
  if (W <= 0 || H <= 0) { W = 640; H = 480; }
#ifdef __3DS__
  for (int i = 0; i < 2; i++) {
    ss_present_pixels[i] = linearMemAlign(
      k3DSBottomTextureWidth * k3DSBottomTextureHeight *
        bottom_bytes_per_pixel(), 64);
    if (ss_present_pixels[i]) {
      memset(ss_present_pixels[i], 0,
             k3DSBottomTextureWidth * k3DSBottomTextureHeight *
               bottom_bytes_per_pixel());
    }
  }
  if (!ss_present_pixels[0] || !ss_present_pixels[1]) {
    Platform3DS_LogRuntime("ERROR bottom GPU upload buffer allocation");
    for (int i = 0; i < 2; i++) {
      linearFree(ss_present_pixels[i]);
      ss_present_pixels[i] = NULL;
    }
    SDL_DestroyRenderer(ss_r); ss_r = NULL;
    SDL_DestroyWindow(ss_win); ss_win = NULL;
    ss_enabled = false;
    return false;
  }
#endif
  u = unit_for_size(W, H);
  printf("second screen: display %d of %d, %dx%d (u=%.2f)\n", target, n, W, H, u);
#ifdef __3DS__
  Platform3DS_LogRuntime("Bottom screen initialized: display %d of %d, %dx%d",
                         target, n, W, H);
#endif
  return true;
}

#endif  // __SWITCH__

static void present_second_screen(void) {
#if defined(__SWITCH__)
  // Nothing is presented from here: the compositor owns the single final
  // present.  Handing the renderer back is the whole job.
  SDL_SetRenderTarget(ss_r, NULL);
#elif defined(__3DS__)
  if (!ss_present_pixels[ss_worker_buffer])
    return;
  SDL_RenderReadPixels(
    ss_r, NULL, bottom_sdl_pixel_format(),
    ss_present_pixels[ss_worker_buffer], bottom_buffer_pitch());
#else
  SDL_RenderPresent(ss_r);
#endif
}

#ifdef __3DS__
typedef struct BottomCriticalState {
  int module;
  int area;
  int dungeon;
  int indoors;
  int equipped;
  uint8_t health_cap;
  uint8_t health_cur;
  uint8_t magic;
  uint8_t keys;
  uint8_t bombs;
  uint8_t arrows;
  uint16_t rupees;
  uint32_t inventory_hash;
} BottomCriticalState;

static void request_bottom_redraw_on_state_change(void) {
  static bool initialized;
  static BottomCriticalState previous;
  BottomCriticalState current;
  uint8_t local_sram[0x80];

  memset(&current, 0, sizeof(current));
  SS_ReadSram(local_sram, sizeof(local_sram));
  current.module = SS_GetModule() & 0xff;
  current.area = SS_GetArea();
  current.dungeon = SS_GetDungeon();
  current.indoors = SS_IsIndoors() ? 1 : 0;
  current.equipped = SS_GetEquippedSlot();
  current.health_cap = local_sram[0x6c];
  current.health_cur = local_sram[0x6d];
  current.magic = local_sram[0x6e];
  current.keys = local_sram[0x6f];
  current.bombs = local_sram[0x43];
  current.arrows = local_sram[0x77];
  current.rupees = (uint16_t)local_sram[0x62] |
                   ((uint16_t)local_sram[0x63] << 8);
  current.inventory_hash = 2166136261u;
  if (!ss_is_new_3ds) {
    for (int i = 0x40; i < 0x80; i++) {
      if (i == 0x43 || i == 0x62 || i == 0x63 ||
          (i >= 0x6c && i <= 0x6f) || i == 0x77)
        continue;
      current.inventory_hash =
        (current.inventory_hash ^ local_sram[i]) * 16777619u;
    }
  }

  bool full_changed = initialized &&
    (current.module != previous.module ||
     current.area != previous.area ||
     current.dungeon != previous.dungeon ||
     current.indoors != previous.indoors ||
     current.equipped != previous.equipped ||
     (!ss_is_new_3ds &&
      current.inventory_hash != previous.inventory_hash));
  bool hud_changed = initialized &&
    (current.health_cap != previous.health_cap ||
     current.health_cur != previous.health_cur ||
     current.magic != previous.magic ||
     current.keys != previous.keys ||
     current.bombs != previous.bombs ||
     current.arrows != previous.arrows ||
     current.rupees != previous.rupees);
  previous = current;
  initialized = true;
  if (full_changed || (ss_is_new_3ds && hud_changed))
    request_bottom_redraw(kBottomRedrawFull);
  else if (hud_changed)
    request_bottom_redraw(kBottomRedrawHud);
}

static bool can_patch_bottom_sidebar(void) {
  int module = SS_GetModule() & 0xff;
  return !ss_is_new_3ds && art_ready && ss_front_buffer >= 0 &&
         (tab == TAB_MAP || tab == TAB_ITEMS) &&
         mode_for_module(module) == MODE_GAME;
}

static bool bottom_needs_periodic_redraw(void) {
  if (ss_is_new_3ds || developer_overlay_mode)
    return true;
  if (mode_for_module(SS_GetModule() & 0xff) != MODE_GAME)
    return true;
  return tab == TAB_MAP || tab == TAB_ITEMS;
}

static void draw_bottom_sidebar_patch(void) {
  SS_ReadSram(sram, sizeof(sram));
  bool indoors = SS_IsIndoors();
  int dungeon_info = SS_GetDungeon();
  bool dungeon_mode = indoors && (dungeon_info & 0xff) != 0xff;
  float tab_h = 84 * u;
  float side_w = 200 * u;
  RectFS side = {
    W - side_w + 4 * u,
    10 * u,
    side_w - 14 * u,
    H - tab_h - 14 * u,
  };
  SDL_Rect clip = {
    (int)side.x, (int)side.y, (int)side.w, (int)side.h,
  };

  SDL_RenderSetClipRect(ss_r, &clip);
  SDL_Texture *background = dungeon_mode ? tex_bg_stone : tex_bg_menu;
  int texture_width = dungeon_mode ? kSSTexStone_W : kSSTexMenu_W;
  int texture_height = dungeon_mode ? kSSTexStone_H : kSSTexMenu_H;
  if (background) {
    float tile_width = texture_width * 2.0f;
    float tile_height = texture_height * 2.0f;
    for (float y = 0; y < H; y += tile_height) {
      for (float x = 0; x < W; x += tile_width) {
        SDL_FRect destination = {x, y, tile_width, tile_height};
        SDL_RenderCopyF(ss_r, background, NULL, &destination);
      }
    }
  } else {
    fill_rect(side.x, side.y, side.w, side.h,
              dungeon_mode ? COL_BG_STONE : COL_BG_MENU);
  }
  draw_sidebar(side.x, side.y, side.w, side.h, dungeon_mode);
  SDL_RenderSetClipRect(ss_r, NULL);

  uint8_t *destination = ss_present_pixels[ss_worker_buffer] +
    clip.y * bottom_buffer_pitch() + clip.x * bottom_bytes_per_pixel();
  SDL_RenderReadPixels(ss_r, &clip, bottom_sdl_pixel_format(),
                       destination, bottom_buffer_pitch());
}
#endif

static bool sw_tap_long;   /* set by SecondScreenSDL_TapLocal */

static void handle_tap(float x, float y) {
  int module = SS_GetModule() & 0xFF;
#ifdef __SWITCH__
  /* FIRST, ahead of everything including the tab bar: the modal owns the
   * screen while it is up, so no tap may change the page underneath it.  Also
   * ahead of the MODE_GAME test, because the modal stays up (and keeps owning
   * the pad) if the module changes while it is open. */
  if (sw_confirm_active) {
    if (sw_confirm_no_r.w > 0 && in_rect(&sw_confirm_no_r, x, y)) {
      sw_confirm_choice = 0;
      SecondScreenSDL_ConfirmCancel();
    } else if (sw_confirm_yes_r.w > 0 && in_rect(&sw_confirm_yes_r, x, y)) {
      sw_confirm_choice = 1;
      SecondScreenSDL_ConfirmCommit();
    }
    return;
  }
#endif
  if (mode_for_module(module) != MODE_GAME || !art_ready) {
#ifdef __SWITCH__
    /* The interactive pages stay usable during a cutscene because they stay
     * drawn there (draw_second_screen) and keep owning the pad; touch must
     * follow the same rule or they would be visible but unreachable. */
    if (art_ready && (tab == TAB_SETTINGS || tab == TAB_SAVE)) goto interactive;
#endif
    return;
  }
#ifdef __SWITCH__
  /* Long press on the live overworld map drops or lifts a pin -- the
   * donor's gesture, and the reason the zoom button is excluded. */
  if (sw_tap_long && tab == TAB_MAP && map_live &&
      in_rect(&map_view_r, x, y) && !in_rect(&map_zoom_r, x, y)) {
    int wx = (int)(((x - map_ox) / map_scale - 128.0f) * 16.0f);
    int wy = (int)(((y - map_oy) / map_scale - 128.0f) * 16.0f);
    int hit = (int)(26 * u / map_scale * 16.0f);
    pins_toggle_world(wx, wy, hit);
    return;
  }
#endif
#ifdef __3DS__
  if (!ss_is_new_3ds) {
    ss_touch_request_ticks = svcGetSystemTick();
    ss_touch_redraw_pending = true;
    ss_worker_interactive = true;
    prioritize_bottom_touch();
  }
  request_bottom_redraw(kBottomRedrawFull);
#endif

#ifdef __SWITCH__
interactive:
#endif
  if (in_rect(&tab_items_r, x, y)) { tab = (tab == TAB_ITEMS) ? TAB_MAP : TAB_ITEMS; leave_settings_submenu(); return; }
  if (in_rect(&tab_map_r, x, y))   { tab = TAB_MAP; leave_settings_submenu(); return; }
  if (in_rect(&tab_gear_r, x, y))  { tab = (tab == TAB_GEAR) ? TAB_MAP : TAB_GEAR; leave_settings_submenu(); return; }
#ifdef __SWITCH__
  if (in_rect(&tab_save_r, x, y)) {
    SecondScreenSDL_SetTab(tab == TAB_SAVE ? TAB_MAP : TAB_SAVE);
    return;
  }
  if (in_rect(&tab_guide_r, x, y)) { tab = (tab == TAB_GUIDE) ? TAB_MAP : TAB_GUIDE; leave_settings_submenu(); return; }
#endif
  if (in_rect(&tab_settings_r, x, y)) { tab = (tab == TAB_SETTINGS) ? TAB_MAP : TAB_SETTINGS; leave_settings_submenu(); return; }

  if (tab == TAB_SETTINGS || tab == TAB_SAVE) {
#ifdef __SWITCH__
    /* Rows come from the same table the painter used this frame, so a tap and
     * a controller press run the identical action. */
    int menu = sw_current_menu();
    if (sw_back_r.w > 0 && in_rect(&sw_back_r, x, y)) { sw_pop(); return; }
    if (sw_up_r.w > 0 && in_rect(&sw_up_r, x, y)) {
      if (sw_scroll > 0) sw_scroll--;
      return;
    }
    if (sw_down_r.w > 0 && in_rect(&sw_down_r, x, y)) {
      if (sw_scroll + sw_visible < sw_row_count) sw_scroll++;
      return;
    }
    for (int v = 0; v < sw_visible; v++) {
      int i = sw_scroll + v;
      if (i >= sw_row_count) break;
      if (in_rect(&sw_row_r[v], x, y)) {
        sw_sel = i;
        if (menu == SW_STATES) sw_state_action(i, x);
        else sw_activate(menu, i, 1);
        return;
      }
    }
    return;
#else
    if (remap_mode) {
      if (in_rect(&remap_back_r, x, y)) { leave_remap(); return; }
      if (in_rect(&remap_page_r, x, y)) {
        remap_first_row = remap_first_row == 0 ? 6 : 0;
        return;
      }
      for (int visible = 0; visible < 6; visible++) {
        int i = remap_first_row + visible;
        if (in_rect(&remap_row_r[visible], x, y)) {
          if (remap_arm == i) {
            SS_ArmButtonCapture(false);
            remap_arm = -1;
          } else {
            remap_arm = i;
            remap_arm_at = SDL_GetTicks();
            SS_ArmButtonCapture(true);
          }
          return;
        }
      }
    } else if (screen_mode) {
      if (in_rect(&screen_back_r, x, y)) {
        screen_mode = false;
      } else if (in_rect(&screen_row_r[0], x, y)) {
        enum Platform3DSDisplayMode mode = kPlatform3DSDisplayUltraWideMod;
#ifdef __3DS__
        switch (Platform3DS_GetDisplayMode()) {
        case kPlatform3DSDisplayOriginal:
          mode = kPlatform3DSDisplayStretch;
          break;
        case kPlatform3DSDisplayStretch:
          mode = kPlatform3DSDisplayUltraWideMod;
          break;
        case kPlatform3DSDisplayUltraWideMod:
        default:
          mode = kPlatform3DSDisplayOriginal;
          break;
        }
#else
        mode = SS_IsWidescreen() ? kPlatform3DSDisplayOriginal :
               kPlatform3DSDisplayUltraWideMod;
#endif
        SS_Set3DSDisplayMode((int)mode);
#ifdef __3DS__
        Platform3DS_SetDisplayMode(mode);
#endif
        update_ini("[General]", "DisplayMode",
                   mode == kPlatform3DSDisplayOriginal ? "Original" :
                   mode == kPlatform3DSDisplayStretch ? "Stretch" : "Wide");
      } else if (in_rect(&screen_row_r[1], x, y)) {
        enum Platform3DSWideEdgeMode mode = kPlatform3DSWideEdgeStandard;
#ifdef __3DS__
        mode =
          Platform3DS_GetWideEdgeMode() == kPlatform3DSWideEdgeFixedCamera ?
          kPlatform3DSWideEdgeStandard : kPlatform3DSWideEdgeFixedCamera;
        Platform3DS_SetWideEdgeMode(mode);
#endif
        SS_Set3DSWideEdgeMode((int)mode);
        update_ini("[General]", "WideEdgeMode",
                   mode == kPlatform3DSWideEdgeStandard ? "Standard" : "FixedCamera");
      } else if (in_rect(&screen_row_r[2], x, y)) {
#ifdef __3DS__
        if (Platform3DS_GetDisplayMode() == kPlatform3DSDisplayUltraWideMod) {
          int zoom_index = (Platform3DS_GetWideZoomIndex() + 1) % 5;
          Platform3DS_SetWideZoomIndex(zoom_index);
          update_ini("[General]", "WideZoom",
                     zoom_index == 0 ? "1x" :
                     zoom_index == 1 ? "1.2x" :
                     zoom_index == 2 ? "1.5x" :
                     zoom_index == 3 ? "2x" : "2.5x");
        }
#endif
      } else if (in_rect(&screen_row_r[3], x, y)) {
        bool hide = !SS_IsHudHidden();
        SS_SetHudHidden(hide);
        if (hide) { FILE *f = fopen(".ss_hidehud", "wb"); if (f) fclose(f); }
        else remove(".ss_hidehud");
      }
    } else if (developer_mode) {
      if (developer_overlay_mode) {
        if (in_rect(&developer_back_r, x, y))
          developer_overlay_mode = false;
      } else if (in_rect(&developer_back_r, x, y)) {
        developer_mode = false;
      } else if (in_rect(&developer_row_r[0], x, y)) {
        request_dump_now();
      } else if (in_rect(&developer_row_r[1], x, y)) {
        developer_overlay_mode = true;
#ifdef __3DS__
        request_bottom_redraw(kBottomRedrawFull);
#endif
      }
    } else {
      if (in_rect(&settings_row_r[0], x, y)) {
        screen_mode = true;
      } else if (in_rect(&settings_row_r[1], x, y)) {
        int multiplier = 5;
#ifdef __3DS__
        multiplier = Platform3DS_GetTurboMultiplier();
        multiplier = multiplier >= 5 ? 0 : multiplier <= 0 ? 2 : multiplier + 1;
        Platform3DS_SetTurboMultiplier(multiplier);
#endif
        char value[16];
        if (multiplier <= 0)
          snprintf(value, sizeof(value), "Off");
        else
          snprintf(value, sizeof(value), "%d", multiplier);
        update_ini("[General]", "CStickTurboMultiplier", value);
      } else if (in_rect(&settings_row_r[2], x, y)) {
        SS_GetGamepadControls(pad_controls);
        remap_mode = true;
      } else if (in_rect(&settings_row_r[3], x, y)) {
        developer_mode = true;
      } else if (in_rect(&settings_row_r[4], x, y)) {
        SS_RequestRestart();
      } else if (in_rect(&settings_row_r[5], x, y)) {
#ifdef __3DS__
        Platform3DS_RequestRomSelection();
#endif
      }
    }
    return;
#endif  // __SWITCH__
  }

  if (in_rect(&y_ring_r, x, y)) {
#ifdef __SWITCH__
    if (g_config.aleks_x_item_ring) {
      /* Arming only makes sense where the assignment gesture completes. */
      if (tab == TAB_ITEMS) armed_ring = (armed_ring == 1) ? 0 : 1;
      return;
    }
#endif
    int cur = SS_GetEquippedSlot();
    for (int k = 1; k <= 20; k++) {
      int slot = ((cur - 1 + k) % 20) + 1;
      if (slot_owned(slot - 1)) { SS_EquipSlot(slot); break; }
    }
    return;
  }
#ifdef __SWITCH__
  if (g_config.aleks_x_item_ring && x_ring_r.w > 0 && in_rect(&x_ring_r, x, y)) {
    if (tab == TAB_ITEMS) armed_ring = (armed_ring == 2) ? 0 : 2;
    return;
  }
#endif
  if (tab == TAB_ITEMS && grid_cell > 0) {
    int col = (int)((x - grid_x) / grid_cell);
    int row = (int)((y - grid_y) / grid_cell);
    if (x >= grid_x && y >= grid_y && col >= 0 && col <= 4 && row >= 0 && row <= 3) {
      int i = row * 5 + col;
      if (i < 20 && slot_owned(i)) {
#ifdef __SWITCH__
        /* Assignment goes through the engine's queued action, never a direct
         * SRAM write -- the same path tap-to-equip already uses. */
        if (armed_ring == 2) SS_AssignSlotX(i + 1);
        else                 SS_EquipSlot(i + 1);
        armed_ring = 0;
#else
        SS_EquipSlot(i + 1);
#endif
        tap_flash_slot = i;
        tap_flash_until = SDL_GetTicks() + 250;
      }
    }
    return;
  }
  if (tab == TAB_MAP) {
    for (int i = 0; i < plaque_count; i++) {
      if (in_rect(&plaque_r[i], x, y)) {
        int floor = (int8_t)(SS_GetDungeon() >> 8);
        view_floor_offset = plaque_floor[i] - floor;
        view_floor_touched_at = SDL_GetTicks();
        return;
      }
    }
    if (in_rect(&map_area_r, x, y)) whole_map = !whole_map;
  }
}

bool SecondScreenSDL_HandleEvent(const SDL_Event *e) {
#ifdef __SWITCH__
  // Both halves live in one window, so a window-id test cannot tell a tap
  // on the companion from a tap on the game.  The compositor maps the point
  // through the live layout and calls SecondScreenSDL_TapLocal instead.
  (void)e;
  return false;
#else
  if (!ss_win) return false;
  switch (e->type) {
  case SDL_FINGERDOWN:
#ifdef __3DS__
    return e->tfinger.windowID == ss_winid;
#else
    if (e->tfinger.windowID == ss_winid) { handle_tap(e->tfinger.x * W, e->tfinger.y * H); return true; }
    return false;
#endif
  case SDL_FINGERUP: case SDL_FINGERMOTION:
    return e->tfinger.windowID == ss_winid;
  case SDL_MOUSEBUTTONDOWN:
    if (e->button.windowID == ss_winid) {
      if (e->button.which != SDL_TOUCH_MOUSEID)  // real mouse (dev); touch already handled
        handle_tap((float)e->button.x, (float)e->button.y);
      return true;
    }
    return false;
  case SDL_MOUSEBUTTONUP: case SDL_MOUSEMOTION:
    return e->button.windowID == ss_winid;
  case SDL_WINDOWEVENT:
    if (e->window.windowID != ss_winid) return false;
    if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
      ss_needs_rebuild = true;
    return true;
  default:
    return false;
  }
#endif
}

void SecondScreenSDL_Handle3DSTouch(void) {
#ifdef __3DS__
  if (!ss_win)
    return;
  static bool was_touching;
  u32 keys = hidKeysHeld();
  bool touching = (keys & KEY_TOUCH) != 0;
  if (touching && !was_touching) {
    touchPosition pos;
    hidTouchRead(&pos);
    float draw_x = (320.0f - W) * 0.5f;
    float draw_y = (240.0f - H) * 0.5f;
    float x = (float)pos.px - draw_x;
    float y = (float)pos.py - draw_y;
    handle_tap(x, y);
  }
  was_touching = touching;
#endif
}

static void draw_second_screen(int logic_frames) {
  if (!ss_enabled) return;
#ifndef __3DS__
  static uint32_t frame_no;
  frame_no++;
  if (!ss_win) {
    if (frame_no < 3) return;      // let the game settle its first GL frames
    if (!ensure_window()) return;  // disables itself on failure
  }
  (void)logic_frames;
  if (frame_no & 1) return;   // UI renders at 30fps
#endif
#ifdef __SWITCH__
  if (SDL_SetRenderTarget(ss_r, ss_target) != 0) return;
#endif

  // rebuild the renderer if the compositor resized us
  if (ss_needs_rebuild) {
    ss_needs_rebuild = false;
    int w2, h2;
    SDL_GetWindowSize(ss_win, &w2, &h2);
    if ((w2 != W || h2 != H) && w2 > 0 && h2 > 0) {
      rebuild_renderer(w2, h2);
      if (!ss_win || !ss_r) return;
    }
  }

  if (!art_ready) {
    art_ready = try_load_art();
    if (art_ready && !hud_pref_applied) {
      hud_pref_applied = true;
      FILE *f = fopen(".ss_hidehud", "rb");
      if (f) { fclose(f); SS_SetHudHidden(true); }
    }
    if (!art_ready) {
      // engine still booting: quiet dark frame
      set_color(COL_BOX);
      SDL_RenderClear(ss_r);
      present_second_screen();
      return;
    }
  }

#ifdef __SWITCH__
  sw_poll_remap();
  sw_state_poll_thumb();
#endif

  int link_x = SS_GetLinkX(), link_y = SS_GetLinkY();
  int area = SS_GetArea();
  bool indoors = SS_IsIndoors();
  int dungeon_info = SS_GetDungeon();
  int module = SS_GetModule() & 0xFF;
  SS_ReadSram(sram, sizeof(sram));
  SS_ReadDungFlags(dung_flags, sizeof(dung_flags));
  cur_room = area; cur_palace = dungeon_info & 0xFF; cur_floor_now = (int8_t)(dungeon_info >> 8);

  bool dungeon_mode = indoors;
  int ui_mode = mode_for_module(module);
  if (module == 0x12 || module <= 0x05) has_last_outdoor = false;
  // houses/caves have no dungeon map: keep the overworld view frozen at the door.
  // The special overworld screens (>= 0x80: Master Sword glade, Zora's Domain,
  // under the bridge) run in their own small coordinate space near the map origin,
  // which would park the marker in the Lost Woods (#23); freeze it there too.
  // When the live "last outdoor" spot is unknown (fresh save-load, or the view was
  // rebuilt on refocus) recover the doorway from the engine (last tracked outdoor
  // position, else its exit table) so the map still shows instead of getting stuck
  // on the cinema card (#9).
  bool in_house = ui_mode == MODE_GAME && indoors && (dungeon_info & 0xFF) == 0xFF;
  bool special = ui_mode == MODE_GAME && !indoors && area >= 0x80;
  int exit_pos[3];
  bool have_exit = (in_house || special) && !has_last_outdoor && SS_GetIndoorExit(exit_pos);
  if ((in_house || special) && !has_last_outdoor && !have_exit) ui_mode = MODE_CINEMA;
  if (ui_mode != MODE_GAME) {
    draw_cinema();
#ifdef __SWITCH__
    /*
     * ANYTHING THAT OWNS THE PAD MUST BE DRAWN -- including here, where the
     * game is in a cutscene, on the title screen or dying and the companion
     * would otherwise show only the cinema card.
     *
     * Input ownership (aleks_compositor.c: companion_ui_state_active) does not
     * consult the game module, so a SETTINGS or SAVE STATES page opened while
     * a cutscene is running would keep eating every press with nothing on
     * screen.  That is the same invisible-menu bug this pass exists to remove,
     * so the rule is applied to all three owners, not just the modal.
     */
    if (tab == TAB_SETTINGS || tab == TAB_SAVE) {
      float cine_tab_h = 84 * u;
      map_area_r = (RectFS){10 * u, 10 * u, W - 20 * u, H - cine_tab_h - 14 * u};
      draw_settings(map_area_r);
      draw_tab_bar(cine_tab_h);
    }
    draw_confirm_modal();
#endif
    present_second_screen();
    return;
  }
  if (!indoors && area < 0x80 && (module == 0x09 || module == 0x0B)) {
    last_out_x = link_x; last_out_y = link_y; last_out_area = area;
    has_last_outdoor = true;
  } else if (in_house || special) {
    dungeon_mode = false;
    if (has_last_outdoor) {
      link_x = last_out_x; link_y = last_out_y; area = last_out_area;
    } else {
      link_x = exit_pos[0]; link_y = exit_pos[1]; area = exit_pos[2];
    }
  }

  draw_tiled(dungeon_mode ? tex_bg_stone : tex_bg_menu,
             dungeon_mode ? kSSTexStone_W : kSSTexMenu_W,
             dungeon_mode ? kSSTexStone_H : kSSTexMenu_H,
             (RectFS){0, 0, W, H},
             dungeon_mode ? COL_BG_STONE : COL_BG_MENU);
  float tab_h = 84 * u;
  float side_w = 200 * u;
  bool full_width = tab == TAB_GEAR || tab == TAB_SETTINGS ||
                    tab == TAB_GUIDE || tab == TAB_SAVE;
  map_area_r = full_width ?
    (RectFS){10 * u, 10 * u, W - 20 * u, H - tab_h - 14 * u} :
    (RectFS){10 * u, 10 * u, W - side_w - 14 * u, H - tab_h - 14 * u};

  if (tab == TAB_ITEMS)      draw_items(map_area_r);
  else if (tab == TAB_GEAR)  draw_gear(map_area_r);
#ifdef __SWITCH__
  else if (tab == TAB_GUIDE) draw_guide(map_area_r);
#endif
  else if (tab == TAB_SETTINGS || tab == TAB_SAVE) draw_settings(map_area_r);
  else if (dungeon_mode)     draw_dungeon(map_area_r, link_x, link_y, area & 0xFF, dungeon_info);
  else                       draw_overworld(map_area_r, link_x, link_y, area);

  if (!full_width)
    draw_sidebar(W - side_w + 4 * u, 10 * u, side_w - 14 * u, H - tab_h - 14 * u, dungeon_mode);
  draw_tab_bar(tab_h);
#ifdef __SWITCH__
  /* Last, so it covers the page and the tab bar alike -- it owns the screen
   * for as long as it owns the input. */
  draw_confirm_modal();
#endif

  present_second_screen();
}

#ifdef __3DS__
static void second_screen_worker_main(void *unused) {
  (void)unused;
  for (;;) {
    LightEvent_Wait(&ss_worker_start);
    if (!ss_worker_running)
      break;
    if (!ss_is_new_3ds) {
      s32 priority = ss_worker_interactive ? ss_worker_interactive_priority :
                                             ss_worker_idle_priority;
      svcSetThreadPriority(CUR_THREAD_HANDLE, priority);
    }
    uint64_t start = !ss_is_new_3ds ? svcGetSystemTick() : 0;
    if (ss_worker_sidebar_patch)
      draw_bottom_sidebar_patch();
    else
      draw_second_screen(ss_worker_logic_frames);
    if (!ss_is_new_3ds) {
      uint64_t elapsed = svcGetSystemTick() - start;
      if (ss_worker_sidebar_patch) {
        ss_patch_redraw_count++;
        ss_patch_redraw_total_ticks += elapsed;
        if (elapsed > ss_patch_redraw_max_ticks)
          ss_patch_redraw_max_ticks = elapsed;
      } else {
        ss_full_redraw_count++;
        ss_full_redraw_total_ticks += elapsed;
        if (elapsed > ss_full_redraw_max_ticks)
          ss_full_redraw_max_ticks = elapsed;
      }
      if (ss_worker_touch_request_ticks != 0) {
        uint64_t touch_elapsed =
          svcGetSystemTick() - ss_worker_touch_request_ticks;
        ss_touch_redraw_count++;
        ss_touch_redraw_total_ticks += touch_elapsed;
        if (touch_elapsed > ss_touch_redraw_max_ticks)
          ss_touch_redraw_max_ticks = touch_elapsed;
        ss_worker_touch_request_ticks = 0;
      }
      svcSetThreadPriority(CUR_THREAD_HANDLE, ss_worker_idle_priority);
    }
    LightEvent_Signal(&ss_worker_done);
  }
}

static bool ensure_second_screen_worker(void) {
  if (ss_worker_thread)
    return true;
  LightEvent_Init(&ss_worker_start, RESET_ONESHOT);
  LightEvent_Init(&ss_worker_done, RESET_ONESHOT);
  s32 main_priority = 0x30;
  svcGetThreadPriority(&main_priority, CUR_THREAD_HANDLE);
  ss_worker_idle_priority = main_priority < 0x3f ? main_priority + 1 :
                                                   main_priority;
  ss_worker_interactive_priority = main_priority > 0 ? main_priority - 1 :
                                                       main_priority;
  ss_worker_running = true;
  ss_worker_thread = threadCreate(
    second_screen_worker_main, NULL, 48 * 1024, ss_worker_idle_priority, 0,
    false);
  if (!ss_worker_thread) {
    ss_worker_running = false;
    Platform3DS_LogRuntime(
      "Bottom worker: asynchronous thread unavailable, using synchronous redraws");
    return false;
  }
  Platform3DS_LogRuntime(
    "Bottom worker: Old 3DS touch-priority Core 0 renderer enabled");
  return true;
}

void SecondScreenSDL_BeginFrame(int logic_frames) {
  if (!ss_enabled || Platform3DS_IsSystemClosing())
    return;
  static uint32_t frame_no;
  frame_no++;
  if (!ss_win) {
    if (frame_no < 3 || !ensure_window())
      return;
  }

  request_bottom_redraw_on_state_change();

  if (!ensure_second_screen_worker()) {
    ss_worker_buffer = ss_front_buffer < 0 ? 0 : 1 - ss_front_buffer;
    draw_second_screen(logic_frames);
    ss_front_buffer = ss_worker_buffer;
    ss_frame_ready = true;
    return;
  }

  if (ss_worker_busy && LightEvent_TryWait(&ss_worker_done)) {
    ss_front_buffer = ss_worker_buffer;
    ss_worker_busy = false;
    ss_frame_ready = true;
  }

  int divisor = ss_is_new_3ds ? (logic_frames <= 1 ? 2 : 6) : 180;
  if (developer_overlay_mode)
    request_bottom_redraw(kBottomRedrawFull);

  uint32_t requests =
    __atomic_load_n(&ss_redraw_requests, __ATOMIC_ACQUIRE);
  bool periodic_redraw = ss_front_buffer < 0 ||
    (bottom_needs_periodic_redraw() && frame_no % divisor == 0);
  bool can_start_worker = ss_is_new_3ds || !ss_frame_ready;
  if (!ss_worker_busy && can_start_worker &&
      (requests != 0 || periodic_redraw)) {
    requests = __atomic_exchange_n(&ss_redraw_requests, 0,
                                   __ATOMIC_ACQ_REL);
    ss_worker_sidebar_patch = !periodic_redraw &&
      (requests & kBottomRedrawFull) == 0 &&
      (requests & kBottomRedrawHud) != 0 &&
      can_patch_bottom_sidebar();
    ss_worker_buffer = ss_worker_sidebar_patch ? ss_front_buffer :
      (ss_front_buffer < 0 ? 0 : 1 - ss_front_buffer);
    ss_worker_touch_request_ticks = 0;
    ss_worker_interactive = false;
    if (!ss_worker_sidebar_patch && !ss_is_new_3ds &&
        ss_touch_redraw_pending) {
      ss_worker_touch_request_ticks = ss_touch_request_ticks;
      ss_touch_redraw_pending = false;
      ss_worker_interactive = true;
      prioritize_bottom_touch();
    }
    ss_worker_logic_frames = logic_frames;
    ss_worker_busy = true;
    LightEvent_Signal(&ss_worker_start);
  }
}

void SecondScreenSDL_Update(int logic_frames) {
  (void)logic_frames;
  if (!ss_frame_ready || ss_front_buffer < 0)
    return;
  Platform3DS_PresentBottomFrame(
    ss_present_pixels[ss_front_buffer],
    bottom_buffer_pitch(), W, H);
  ss_frame_ready = false;
}

void SecondScreenSDL_GetOld3DSWorkerStats(
    uint64_t *full_count, uint32_t *full_average_us, uint32_t *full_max_us,
    uint64_t *patch_count, uint32_t *patch_average_us, uint32_t *patch_max_us,
    uint64_t *touch_count, uint32_t *touch_average_us, uint32_t *touch_max_us) {
  if (full_count)
    *full_count = ss_full_redraw_count;
  if (full_average_us)
    *full_average_us = ss_full_redraw_count ? (uint32_t)(
      ss_full_redraw_total_ticks * 1000000ull /
      (SYSCLOCK_ARM11 * ss_full_redraw_count)) : 0;
  if (full_max_us)
    *full_max_us = (uint32_t)(
      ss_full_redraw_max_ticks * 1000000ull / SYSCLOCK_ARM11);
  if (patch_count)
    *patch_count = ss_patch_redraw_count;
  if (patch_average_us)
    *patch_average_us = ss_patch_redraw_count ? (uint32_t)(
      ss_patch_redraw_total_ticks * 1000000ull /
      (SYSCLOCK_ARM11 * ss_patch_redraw_count)) : 0;
  if (patch_max_us)
    *patch_max_us = (uint32_t)(
      ss_patch_redraw_max_ticks * 1000000ull / SYSCLOCK_ARM11);
  if (touch_count)
    *touch_count = ss_touch_redraw_count;
  if (touch_average_us)
    *touch_average_us = ss_touch_redraw_count ? (uint32_t)(
      ss_touch_redraw_total_ticks * 1000000ull /
      (SYSCLOCK_ARM11 * ss_touch_redraw_count)) : 0;
  if (touch_max_us)
    *touch_max_us = (uint32_t)(
      ss_touch_redraw_max_ticks * 1000000ull / SYSCLOCK_ARM11);
}
#else
void SecondScreenSDL_Update(int logic_frames) {
  draw_second_screen(logic_frames);
}
#endif

static void destroy_textures(void) {
  SDL_Texture **texes[] = {&tex_map[0], &tex_map[1], &tex_icons, &tex_glyphs,
                           &tex_letters, &tex_face, &tex_floor, &tex_mapicons,
                           &tex_bg_menu, &tex_bg_parch, &tex_bg_stone};
  for (size_t i = 0; i < sizeof(texes) / sizeof(texes[0]); i++) {
    if (*texes[i]) SDL_DestroyTexture(*texes[i]);
    *texes[i] = NULL;
  }
  art_ready = false;
}

// The window surface is invalidated when the compositor resizes us; rebuild
// the renderer and let the art regenerate at the new size.
static void rebuild_renderer(int w2, int h2) {
#ifdef __SWITCH__
  // Fixed 320x240 surface: nothing can resize it.
  (void)w2; (void)h2;
  return;
#else
  destroy_textures();
  if (ss_r) SDL_DestroyRenderer(ss_r);
  ss_r = SDL_CreateRenderer(ss_win, -1, SDL_RENDERER_SOFTWARE);
  if (!ss_r) {
    fprintf(stderr, "second screen: renderer rebuild failed: %s\n", SDL_GetError());
    SDL_DestroyWindow(ss_win); ss_win = NULL;
    ss_enabled = false;
    return;
  }
  W = w2; H = h2;
  u = unit_for_size(W, H);
  printf("second screen resized: %dx%d (u=%.2f)\n", W, H, u);
  fflush(stdout);
#endif
}

#ifdef __SWITCH__
// ---------------------------------------------------------------------
// Switch entry points.  The compositor owns the renderer, the final present
// and the touch transform; this is everything it needs from the donor UI.
// ---------------------------------------------------------------------

bool SecondScreenSDL_InitSwitch(SDL_Window *window, SDL_Renderer *renderer) {
  if (!window || !renderer) return false;
  main_win = window;
  ss_r = renderer;
  ss_enabled = true;
  tab = g_config.aleks_companion_page == 2 ? TAB_ITEMS :
        g_config.aleks_companion_page == 3 ? TAB_GEAR :
        g_config.aleks_companion_page == 4 ? TAB_GUIDE : TAB_MAP;
  return true;
}

// The finished 320x240 companion frame, or NULL until the engine has parsed
// zelda3_assets.dat and the first draw has run.
SDL_Texture *SecondScreenSDL_GetTexture(void) {
  return ss_target;
}

bool SecondScreenSDL_IsReady(void) {
  return ss_target != NULL && art_ready;
}

// Companion-local point (0..319, 0..239) from the compositor.  long_press
// carries the donor's tap/hold distinction (hold = drop a map pin).
void SecondScreenSDL_TapLocal(int x, int y, int long_press) {
  if (!ss_win) return;
  sw_tap_long = long_press != 0;
  handle_tap((float)x, (float)y);
  sw_tap_long = false;
}

/* ONE authoritative page order, shared by the tab bar, the controller cycle
 * and the NORMAL overlay.  GUIDE is skipped when the Story Guide is off, so
 * turning it off removes the page from the cycle rather than leaving a dead
 * stop in it.
 *
 * THIS IS THE ORDER draw_tab_bar() PAINTS, LEFT TO RIGHT.  It used to start
 * at MAP and put GEAR third, so cycling with the shoulder buttons walked the
 * tabs in an order the player could not see anywhere on screen (community
 * report, v1.0.0).  The visible order is public-facing UX and is the one that
 * wins; the cycle follows the UI, never the other way round.
 *
 * SETTINGS is deliberately absent: it is the cog, not one of the five content
 * tabs.  It is reached by the global ZL+R3 shortcut and by tapping the cog.
 */
#define SW_PAGE_CYCLE_N 5
static const int kPageCycle[SW_PAGE_CYCLE_N] = {
  TAB_GEAR, TAB_MAP, TAB_ITEMS, TAB_SAVE, TAB_GUIDE,
};

/* dir is +1 for the next tab and -1 for the previous one, so a controller can
 * walk the bar both ways. */
void SecondScreenSDL_CycleTabDir(int dir) {
  if (tab == TAB_SETTINGS) return;
  if (dir >= 0) dir = 1; else dir = -1;
  int at = 0;
  for (int i = 0; i < SW_PAGE_CYCLE_N; i++)
    if (kPageCycle[i] == tab) { at = i; break; }
  for (int step = 1; step <= SW_PAGE_CYCLE_N; step++) {
    int next = kPageCycle[((at + dir * step) % SW_PAGE_CYCLE_N + SW_PAGE_CYCLE_N) % SW_PAGE_CYCLE_N];
    if (next == TAB_GUIDE && !StoryGuide_IsEnabled()) continue;
    tab = next;
    break;
  }
  /* Arriving on SAVE STATES re-reads the slots so occupancy and thumbnails
   * are current rather than whatever the last visit left behind. */
  if (tab == TAB_SAVE) { sw_state_refresh_all(); sw_sel = 0; sw_scroll = 0; }
  g_config.aleks_companion_page = (uint8)(tab == TAB_ITEMS ? 2 :
                                          tab == TAB_GEAR ? 3 :
                                          tab == TAB_GUIDE ? 4 :
                                          tab == TAB_SAVE ? 5 : 0);
}

void SecondScreenSDL_CycleTab(void) { SecondScreenSDL_CycleTabDir(1); }

/* ZL+R3 from anywhere.  Unlike ToggleSettings this never walks away from
 * SETTINGS, so the shortcut that OPENS the page cannot also close it on the
 * press that first brought the companion on screen. */
void SecondScreenSDL_OpenSettings(void) {
  tab = TAB_SETTINGS;
  leave_settings_submenu();
}

void SecondScreenSDL_SetTab(int page) {
  if (page < 0 || page >= TAB_COUNT) return;
  tab = page;
  leave_settings_submenu();
  if (tab == TAB_SAVE) { sw_state_refresh_all(); sw_sel = 0; sw_scroll = 0; }
}

int SecondScreenSDL_GetTab(void) { return tab; }

bool SecondScreenSDL_IsSaveTab(void)     { return tab == TAB_SAVE; }
bool SecondScreenSDL_IsMapTab(void)      { return tab == TAB_MAP; }
bool SecondScreenSDL_IsSettingsTab(void) { return tab == TAB_SETTINGS; }

/* MAP page, controller: physical R swaps Follow <-> Whole Map.  Same state
 * the touch handler toggles, so the two gestures cannot drift apart. */
void SecondScreenSDL_ToggleWholeMap(void) {
  if (tab == TAB_MAP) whole_map = !whole_map;
}

/* Leave a transient page (SETTINGS / SAVE STATES) back to the browse page.
 * Used when a menu is closed, so no page can stay logically active while the
 * presentation has moved on. */
void SecondScreenSDL_LeaveTransientPage(void) {
  sw_confirm_clear();
  tab = TAB_MAP;
  leave_settings_submenu();
}

/* ---- save-state page + modal, driven by the compositor's input ladder --- */

bool SecondScreenSDL_ConfirmActive(void) { return sw_confirm_active; }

void SecondScreenSDL_ConfirmMove(int dir) {
  if (!sw_confirm_active) return;
  if (dir) sw_confirm_choice = sw_confirm_choice ? 0 : 1;
}

/* Returns true when the caller should also close a NORMAL overlay it opened
 * just to show this modal. */
bool SecondScreenSDL_ConfirmCommit(void) {
  bool close_overlay;
  if (!sw_confirm_active) return false;
  if (sw_confirm_choice == 1) {
    if (sw_confirm_kind == kConfirm_LanguageRestart) {
      SS_RequestRestart();
    } else {
      SS_RequestSaveState(sw_backend_slot(sw_confirm_slot));
      sw_thumb_want = sw_confirm_slot;
      sw_state_flash_now(sw_confirm_slot, "SAVED");
    }
  }
  if (sw_confirm_restore_tab >= 0) {
    tab = sw_confirm_restore_tab;
    leave_settings_submenu();
  }
  close_overlay = sw_confirm_close_overlay;
  sw_confirm_clear();
  return close_overlay;
}

bool SecondScreenSDL_ConfirmCancel(void) {
  bool close_overlay;
  if (!sw_confirm_active) return false;
  if (sw_confirm_restore_tab >= 0) {
    tab = sw_confirm_restore_tab;
    leave_settings_submenu();
  }
  close_overlay = sw_confirm_close_overlay;
  sw_confirm_clear();
  return close_overlay;
}

/* SAVE STATES page, controller.  Physical X saves (confirmed), physical Y
 * loads immediately -- the compositor maps the physical labels. */
void SecondScreenSDL_SaveArmSave(void) {
  if (tab == TAB_SAVE && sw_sel >= 0) sw_state_arm_save(sw_sel, false);
}

void SecondScreenSDL_SaveLoadSelected(void) {
  if (tab == TAB_SAVE && sw_sel >= 0) sw_state_load(sw_sel);
}

/* Quick Save: arm the SAME modal for the configured quick slot, remembering
 * what to restore when it resolves.  restore_tab < 0 means "stay here". */
void SecondScreenSDL_ArmQuickSave(int restore_tab, bool close_overlay_after) {
  int slot = g_config.aleks_quick_slot;
  if (slot < 0 || slot >= SW_STATE_SLOTS) slot = 0;
  sw_state_refresh_all();
  tab = TAB_SAVE;
  leave_settings_submenu();
  sw_sel = slot;
  sw_scroll = 0;
  sw_state_arm_save(slot, true);
  sw_confirm_restore_tab = restore_tab;
  sw_confirm_close_overlay = close_overlay_after;
}

/* Quick Load: immediate, through the same queued path, no modal. */
void SecondScreenSDL_QuickLoad(void) {
  int slot = g_config.aleks_quick_slot;
  if (slot < 0 || slot >= SW_STATE_SLOTS) slot = 0;
  sw_state_refresh_all();
  sw_state_load(slot);
}

/* MAP page, controller: drop or lift a pin where Link stands. */
bool SecondScreenSDL_TogglePinAtLink(void) {
  if (tab != TAB_MAP) return false;
  return pins_toggle_at_link();
}

void SecondScreenSDL_ToggleSettings(void) {
  tab = tab == TAB_SETTINGS ? TAB_MAP : TAB_SETTINGS;
  leave_settings_submenu();
}

/* ---- controller navigation over the settings menu ---------------------
 * Focus is an index into the SAME row table the painter used, and activating
 * runs the SAME sw_activate a tap runs, so the pad and the touchscreen can
 * never drift into two behaviours -- the rule the final TMC panel follows.
 */
void SecondScreenSDL_MoveSelection(int delta) {
  /* SAVE STATES has a known, fixed row count, so it does not depend on a
   * sw_row_count that only becomes correct once the page has been painted. */
  int count = tab == TAB_SAVE ? SW_STATE_SLOTS : sw_row_count;
  if (tab != TAB_SETTINGS && tab != TAB_SAVE) return;
  if (count <= 0) return;
  if (sw_sel < 0) {
    sw_sel = 0;      /* first press only takes focus; it does not also move */
    return;
  }
  if (sw_sel >= count) sw_sel = 0;
  sw_sel = (sw_sel + delta + count) % count;
}

/* dir: +1 confirm/next, -1 previous. */
bool SecondScreenSDL_ActivateSelection(int dir) {
  if ((tab != TAB_SETTINGS && tab != TAB_SAVE) || sw_row_count <= 0) return false;
  if (sw_sel < 0) sw_sel = 0;
  int menu = sw_current_menu();
  if (menu == SW_STATES) {
    /* A is not a save button here: SAVE is physical X and goes through the
     * confirmation, LOAD is physical Y.  Activating a card only focuses it. */
    return true;
  } else {
    sw_activate(menu, sw_sel, dir);
  }
  return true;
}

/* Returns false at the root, where the caller closes SETTINGS instead. */
bool SecondScreenSDL_LeaveSubmenu(void) {
  if (tab != TAB_SETTINGS) return false;
  return sw_pop();
}

int SecondScreenSDL_SettingsRowCount(void) {
  return tab == TAB_SETTINGS ? sw_row_count : 0;
}
#endif  // __SWITCH__

void SecondScreenSDL_Shutdown(void) {
  ss_enabled = false;
#ifdef __SWITCH__
  if (ss_target) { SDL_DestroyTexture(ss_target); ss_target = NULL; }
  destroy_textures();
  ss_win = NULL;
  ss_r = NULL;
  return;
#endif
#ifdef __3DS__
  if (ss_worker_thread) {
    ss_worker_running = false;
    LightEvent_Signal(&ss_worker_start);
    Result join_result = threadJoin(ss_worker_thread, 2000000000ull);
    if (R_FAILED(join_result))
      Platform3DS_LogRuntime("WARNING: second screen worker join timeout: 0x%08lx",
                             (unsigned long)join_result);
    threadFree(ss_worker_thread);
    ss_worker_thread = NULL;
  }
  for (int i = 0; i < 2; i++) {
    linearFree(ss_present_pixels[i]);
    ss_present_pixels[i] = NULL;
  }
#endif
  if (!ss_win) return;
  destroy_textures();
  SDL_DestroyRenderer(ss_r); ss_r = NULL;
  SDL_DestroyWindow(ss_win); ss_win = NULL;
}
