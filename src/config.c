#include "config.h"
#include "types.h"
#include <stdio.h>
#include <string.h>
#include <SDL.h>
#include "features.h"
#include "util.h"

enum {
  kKeyMod_ScanCode = 0x200,
  kKeyMod_Alt = 0x400,
  kKeyMod_Shift = 0x800,
  kKeyMod_Ctrl = 0x1000,
};

Config g_config;

#define REMAP_SDL_KEYCODE(key) ((key) & SDLK_SCANCODE_MASK ? kKeyMod_ScanCode : 0) | (key) & (kKeyMod_ScanCode - 1)
#define _(x) REMAP_SDL_KEYCODE(x)
#define S(x) REMAP_SDL_KEYCODE(x) | kKeyMod_Shift
#define A(x) REMAP_SDL_KEYCODE(x) | kKeyMod_Alt
#define C(x) REMAP_SDL_KEYCODE(x) | kKeyMod_Ctrl
#define N 0
static const uint16 kDefaultKbdControls[kKeys_Total] = {
  0,
  // Controls
  _(SDLK_UP), _(SDLK_DOWN), _(SDLK_LEFT), _(SDLK_RIGHT), _(SDLK_RSHIFT), _(SDLK_RETURN), _(SDLK_x), _(SDLK_z), _(SDLK_s), _(SDLK_a), _(SDLK_c), _(SDLK_v),
  // LoadState
  _(SDLK_F1), _(SDLK_F2), _(SDLK_F3), _(SDLK_F4), _(SDLK_F5), _(SDLK_F6), _(SDLK_F7), _(SDLK_F8), _(SDLK_F9), _(SDLK_F10), N, N, N, N, N, N, N, N, N, N,
  // SaveState
  S(SDLK_F1), S(SDLK_F2), S(SDLK_F3), S(SDLK_F4), S(SDLK_F5), S(SDLK_F6), S(SDLK_F7), S(SDLK_F8), S(SDLK_F9), S(SDLK_F10), N, N, N, N, N, N, N, N, N, N,
  // Replay State
  C(SDLK_F1), C(SDLK_F2), C(SDLK_F3), C(SDLK_F4), C(SDLK_F5), C(SDLK_F6), C(SDLK_F7), C(SDLK_F8), C(SDLK_F9), C(SDLK_F10), N, N, N, N, N, N, N, N, N, N,
  // Load Ref State
  N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N,
  // Replay Ref State
  N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N,
  // CheatLife, CheatKeys, CheatEquipment, CheatWalkThroughWalls
  _(SDLK_w), _(SDLK_o), S(SDLK_w), C(SDLK_e),
  // ClearKeyLog, StopReplay, Fullscreen, Reset, Pause, PauseDimmed, Turbo, ReplayTurbo, WindowBigger, WindowSmaller, DisplayPerf, ToggleRenderer
  _(SDLK_k), _(SDLK_l), A(SDLK_RETURN), C(SDLK_r), S(SDLK_p), _(SDLK_p), _(SDLK_TAB), _(SDLK_t), N, N, _(SDLK_f), _(SDLK_r),
};
#undef _
#undef A
#undef C
#undef S
#undef N

typedef struct KeyNameId {
  const char *name;
  uint16 id, size;
} KeyNameId;

#define M(n) {#n, kKeys_##n, kKeys_##n##_Last - kKeys_##n + 1}
#define S(n) {#n, kKeys_##n, 1}
static const KeyNameId kKeyNameId[] = {
  {"Null", kKeys_Null, 65535},
  M(Controls), M(Load), M(Save), M(Replay), M(LoadRef), M(ReplayRef),
  S(CheatLife), S(CheatKeys), S(CheatEquipment), S(CheatWalkThroughWalls),
  S(ClearKeyLog), S(StopReplay), S(Fullscreen), S(Reset),
  S(Pause), S(PauseDimmed), S(Turbo), S(ReplayTurbo), S(WindowBigger), S(WindowSmaller), S(VolumeUp), S(VolumeDown), S(DisplayPerf), S(ToggleRenderer),
};
#undef S
#undef M
typedef struct KeyMapHashEnt {
  uint16 key, cmd, next;
} KeyMapHashEnt;

static uint16 keymap_hash_first[255];
static KeyMapHashEnt *keymap_hash;
static int keymap_hash_size;
static bool has_keynameid[countof(kKeyNameId)];

static bool KeyMapHash_Add(uint16 key, uint16 cmd) {
  if ((keymap_hash_size & 0xff) == 0) {
    if (keymap_hash_size > 10000)
      Die("Too many keys");
    keymap_hash = realloc(keymap_hash, sizeof(KeyMapHashEnt) * (keymap_hash_size + 256));
  }
  int i = keymap_hash_size++;
  KeyMapHashEnt *ent = &keymap_hash[i];
  ent->key = key;
  ent->cmd = cmd;
  ent->next = 0;
  int j = (uint32)key % 255;

  uint16 *cur = &keymap_hash_first[j];
  while (*cur) {
    KeyMapHashEnt *ent = &keymap_hash[*cur - 1];
    if (ent->key == key)
      return false;
    cur = &ent->next;
  }
  *cur = i + 1;
  return true;
}

static int KeyMapHash_Find(uint16 key) {
  int i = keymap_hash_first[key % 255];
  while (i) {
    KeyMapHashEnt *ent = &keymap_hash[i - 1];
    if (ent->key == key)
      return ent->cmd;
    i = ent->next;
  }
  return 0;
}

int FindCmdForSdlKey(SDL_Keycode code, SDL_Keymod mod) {
  if (code & ~(SDLK_SCANCODE_MASK | 0x1ff))
    return 0;
  int key = 0;
  if (code != SDLK_LALT && code != SDLK_RALT)
    key |=  mod & KMOD_ALT ? kKeyMod_Alt : 0;
  if (code != SDLK_LCTRL && code != SDLK_RCTRL)
    key |= mod & KMOD_CTRL ? kKeyMod_Ctrl : 0;
  if (code != SDLK_LSHIFT && code != SDLK_RSHIFT)
    key |= mod & KMOD_SHIFT ? kKeyMod_Shift : 0;
  key |= REMAP_SDL_KEYCODE(code);
  return KeyMapHash_Find(key);
}

static void ParseKeyArray(char *value, int cmd, int size) {
  char *s;
  int i = 0;
  for (; i < size && (s = NextDelim(&value, ',')) != NULL; i++, cmd += (cmd != 0)) {
    if (*s == 0)
      continue;
    int key_with_mod = 0;
    for (;;) {
      if (StringStartsWithNoCase(s, "Shift+")) {
        key_with_mod |= kKeyMod_Shift, s += 6;
      } else if (StringStartsWithNoCase(s, "Ctrl+")) {
        key_with_mod |= kKeyMod_Ctrl, s += 5;
      } else if (StringStartsWithNoCase(s, "Alt+")) {
        key_with_mod |= kKeyMod_Alt, s += 4;
      } else {
        break;
      }
    }
    SDL_Keycode key = SDL_GetKeyFromName(s);
    if (key == SDLK_UNKNOWN) {
      fprintf(stderr, "Unknown key: '%s'\n", s);
      continue;
    }
    if (!KeyMapHash_Add(key_with_mod | REMAP_SDL_KEYCODE(key), cmd))
      fprintf(stderr, "Duplicate key: '%s'\n", s);
  }
}

typedef struct GamepadMapEnt {
  uint32 modifiers;
  uint16 cmd, next;
} GamepadMapEnt;

static uint16 joymap_first[kGamepadBtn_Count];
static GamepadMapEnt *joymap_ents;
static int joymap_size;
static bool has_joypad_controls;

static int CountBits32(uint32 n) {
  int count = 0;
  for (; n != 0; count++)
    n &= (n - 1);
  return count;
}

static void GamepadMap_Add(int button, uint32 modifiers, uint16 cmd) {
  if ((joymap_size & 0xff) == 0) {
    if (joymap_size > 1000)
      Die("Too many joypad keys");
    joymap_ents = realloc(joymap_ents, sizeof(GamepadMapEnt) * (joymap_size + 64));
    if (!joymap_ents) Die("realloc failure");
  }
  uint16 *p = &joymap_first[button];
  // Insert it as early as possible but before after any entry with more modifiers.
  int cb = CountBits32(modifiers);
  while (*p && cb < CountBits32(joymap_ents[*p - 1].modifiers))
    p = &joymap_ents[*p - 1].next;
  int i = joymap_size++;
  GamepadMapEnt *ent = &joymap_ents[i];
  ent->modifiers = modifiers;
  ent->cmd = cmd;
  ent->next = *p;
  *p = i + 1;
}

int FindCmdForGamepadButton(int button, uint32 modifiers) {
  GamepadMapEnt *ent;
  for(int e = joymap_first[button]; e != 0; e = ent->next) {
    ent = &joymap_ents[e - 1];
    if ((modifiers & ent->modifiers) == ent->modifiers)
      return ent->cmd;
  }
  return 0;
}

static int ParseGamepadButtonName(const char **value) {
  const char *s = *value;
  // Longest substring first
  static const char *const kGamepadKeyNames[] = {
    "Back", "Guide", "Start", "L3", "R3",
    "L1", "R1", "DpadUp", "DpadDown", "DpadLeft", "DpadRight", "L2", "R2",
    "Lb", "Rb", "A", "B", "X", "Y"
  };
  static const uint8 kGamepadKeyIds[] = {
    kGamepadBtn_Back, kGamepadBtn_Guide, kGamepadBtn_Start, kGamepadBtn_L3, kGamepadBtn_R3,
    kGamepadBtn_L1, kGamepadBtn_R1, kGamepadBtn_DpadUp, kGamepadBtn_DpadDown, kGamepadBtn_DpadLeft, kGamepadBtn_DpadRight, kGamepadBtn_L2, kGamepadBtn_R2,
    kGamepadBtn_L1, kGamepadBtn_R1, kGamepadBtn_A, kGamepadBtn_B, kGamepadBtn_X, kGamepadBtn_Y,
  };
  for (size_t i = 0; i != countof(kGamepadKeyNames); i++) {
    const char *r = StringStartsWithNoCase(s, kGamepadKeyNames[i]);
    if (r) {
      *value = r;
      return kGamepadKeyIds[i];
    }
  }
  return kGamepadBtn_Invalid;
}

// Report the unmodified button bound to each of the 12 joypad commands
// (0xff = none).  Esteban's accessor, unchanged.
void GamepadMap_GetControls(uint8 *out) {
  for (int i = 0; i < 12; i++)
    out[i] = 0xff;
  for (int b = 0; b < kGamepadBtn_Count; b++) {
    for (int e = joymap_first[b]; e != 0; e = joymap_ents[e - 1].next) {
      GamepadMapEnt *ent = &joymap_ents[e - 1];
      int cmd = ent->cmd - kKeys_Controls;
      if (ent->modifiers == 0 && (unsigned)cmd < 12 && out[cmd] == 0xff)
        out[cmd] = b;
    }
  }
}

// Rebind the 12 joypad commands, leaving every other gamepad mapping (the
// ALEKS modifier shortcuts included) untouched.  Esteban's accessor.
void GamepadMap_SetControls(const uint8 *btns) {
  for (int b = 0; b < kGamepadBtn_Count; b++) {
    uint16 *p = &joymap_first[b];
    while (*p) {
      GamepadMapEnt *ent = &joymap_ents[*p - 1];
      if (ent->cmd >= kKeys_Controls && ent->cmd <= kKeys_Controls_Last)
        *p = ent->next;
      else
        p = &ent->next;
    }
  }
  for (int i = 0; i < 12; i++)
    if (btns[i] < kGamepadBtn_Count)
      GamepadMap_Add(btns[i], 0, kKeys_Controls + i);
}

static const uint8 kDefaultGamepadCmds[] = {
  kGamepadBtn_DpadUp, kGamepadBtn_DpadDown, kGamepadBtn_DpadLeft, kGamepadBtn_DpadRight, kGamepadBtn_Back, kGamepadBtn_Start,
  kGamepadBtn_B, kGamepadBtn_A, kGamepadBtn_Y, kGamepadBtn_X, kGamepadBtn_L1, kGamepadBtn_R1,
};

/*
 * The factory binding for one joypad command, for repairing an unbound one.
 *
 * A COMMAND MUST NEVER BE LEFT UNBOUND.  Hardware showed why: the rebinding
 * swap could hand a command the value -1, the ini then stored an empty field,
 * the next launch read it back as unbound, and the damage compounded until
 * SNES A, X and L had no button at all and the game could not be played.  An
 * unbound command is now always repaired to this rather than persisted.
 */
uint8 GamepadMap_DefaultForCmd(int cmd) {
  if (cmd < 0 || cmd >= (int)countof(kDefaultGamepadCmds))
    return kGamepadBtn_A;
  return kDefaultGamepadCmds[cmd];
}

static void ParseGamepadArray(char *value, int cmd, int size) {
  char *s;
  int i = 0;
  for (; i < size && (s = NextDelim(&value, ',')) != NULL; i++, cmd += (cmd != 0)) {
    if (*s == 0)
      continue;
    uint32 modifiers = 0;
    const char *ss = s;
    for (;;) {
      int button = ParseGamepadButtonName(&ss);
      if (button == kGamepadBtn_Invalid) BAD: {
        fprintf(stderr, "Unknown gamepad button: '%s'\n", s);
        break;
      }
      while (*ss == ' ' || *ss == '\t') ss++;
      if (*ss == '+') {
        ss++;
        modifiers |= 1 << button;
      } else if (*ss == 0) {
        GamepadMap_Add(button, modifiers, cmd);
        break;
      } else
        goto BAD;
    }
  }
}

// Restore the stock joypad bindings.  Shares kDefaultGamepadCmds with the
// boot path, so "RESET DEFAULTS" can never drift from the real defaults.
void GamepadMap_ResetControls(void) {
  uint8 defaults[12];
  for (int i = 0; i < 12; i++)
    defaults[i] = i < (int)countof(kDefaultGamepadCmds) ? kDefaultGamepadCmds[i] : 0xff;
  GamepadMap_SetControls(defaults);
}

static void RegisterDefaultKeys() {
  for (int i = 1; i < countof(kKeyNameId); i++) {
    if (!has_keynameid[i]) {
      int size = kKeyNameId[i].size, k = kKeyNameId[i].id;
      for (int j = 0; j < size; j++, k++)
        KeyMapHash_Add(kDefaultKbdControls[k], k);
    }
  }
  if (!has_joypad_controls) {
    for (int i = 0; i < countof(kDefaultGamepadCmds); i++)
      GamepadMap_Add(kDefaultGamepadCmds[i], 0, kKeys_Controls + i);
  } else {
    /*
     * A PARTIAL map is repaired command by command.
     *
     * `Controls = DpadUp, DpadDown, DpadLeft, A, R2, Back, B, , Y, , , L3`
     * is a real line off a real card: the empty fields are commands with no
     * button, and this branch used to do nothing about them because the key
     * was present.  The game then booted with SNES A, X and L dead and no way
     * to fix it except editing the file by hand.  Anything still unbound
     * after parsing now gets its factory button back.
     */
    uint8 bound[12];
    GamepadMap_GetControls(bound);
    for (int i = 0; i < (int)countof(kDefaultGamepadCmds); i++) {
      if (bound[i] == 0xff) {
        GamepadMap_Add(kDefaultGamepadCmds[i], 0, kKeys_Controls + i);
        fprintf(stderr, "Joypad command %d was unbound; restored default\n", i);
      }
    }
  }
}

const uint8 kAleksShortcutButtons[] = {
  0 /* OFF */, kGamepadBtn_X, kGamepadBtn_Y, kGamepadBtn_L2, kGamepadBtn_R2,
  kGamepadBtn_L3, kGamepadBtn_R3, kGamepadBtn_Back,
};
const int kAleksShortcutButtonCount =
    (int)(sizeof(kAleksShortcutButtons) / sizeof(kAleksShortcutButtons[0]));

/* ---- gameplay aspect geometry (see config.h) --------------------------- */

int AleksAspect_ExtraSide(int aspect) {
  int extra;
  switch (aspect) {
  /* Byte-for-byte the arithmetic WIDE has always used; it stays 71 whatever
   * else changes around it. */
  case kAleksAspect_Wide:    extra = (224 * 16 / 9 - 256) / 2; break;
  /* 16:9 through the 7:6 PAR: 256+2e = 16*224*6/(9*7) = 341.33 -> e = 42.67.
   * Rounded up to 43 (342 wide) because 342 lands 0.2% wide of 16:9 while 340
   * lands 0.4% narrow -- the smaller error, and it errs toward filling. */
  case kAleksAspect_True169: extra = (16 * 224 * 6 * 2 / (9 * 7) - 256 * 2 + 2) / 4; break;
  /* Same solve at 240 lines: 256+2e = 16*240*6/(9*7) = 365.7 -> e = 54.9 -> 55,
   * giving 366x240 and 1.77917 against 1.77778 -- the closest any of these
   * modes gets to physical 16:9. */
  case kAleksAspect_True169Expanded:
    extra = (16 * 240 * 6 * 2 / (9 * 7) - 256 * 2 + 2) / 4; break;
  default:                   extra = 0; break;
  }
  return extra > kPpuExtraLeftRight ? kPpuExtraLeftRight : extra;
}

/* Only the EXPANDED mode asks the engine for its 240-line frame; every other
 * aspect keeps the original 224 exactly as before. */
bool AleksAspect_Height240(int aspect) {
  return aspect == kAleksAspect_True169Expanded;
}

int AleksAspect_FromExtraSide(int extra) {
  for (int i = 0; i < kAleksAspect_Count; i++)
    if (AleksAspect_ExtraSide(i) == extra) return i;
  return extra > 0 ? kAleksAspect_Wide : kAleksAspect_4x3;
}

const char *AleksAspect_IniValue(int aspect) {
  return aspect == kAleksAspect_Wide ? "16:9" :
         aspect == kAleksAspect_True169 ? "True16:9" :
         aspect == kAleksAspect_True169Expanded ? "True16:9Expanded" : "4:3";
}

/*
 * The aspects the SETTINGS row offers, in cycle order.
 *
 * WIDE is deliberately absent: 398x224 is 2.07:1 after the PAR correction, so
 * it can only ever be shown with a thick letterbox, and TRUE 16:9 supersedes
 * it for every panel this runs on.  The MODE is not removed -- a config that
 * says Wide still parses and still runs 398x224 exactly as before, it simply
 * cannot be reached from the menu.  Cycling out of a legacy Wide lands on the
 * first entry here and there is no way back, which is intended.
 */
const uint8 kAleksAspectCycle[3] = {
  kAleksAspect_4x3, kAleksAspect_True169, kAleksAspect_True169Expanded,
};

int AleksAspect_NextInCycle(int from, int dir) {
  int n = (int)(sizeof kAleksAspectCycle / sizeof kAleksAspectCycle[0]);
  int at = -1;
  for (int i = 0; i < n; i++)
    if (kAleksAspectCycle[i] == from) { at = i; break; }
  if (at < 0)                       /* legacy Wide: step into the cycle */
    return kAleksAspectCycle[dir < 0 ? n - 1 : 0];
  return kAleksAspectCycle[(at + (dir < 0 ? n - 1 : 1)) % n];
}

const char *AleksAspect_Label(int aspect) {
  return aspect == kAleksAspect_Wide ? "WIDE (LEGACY)" :
         aspect == kAleksAspect_True169 ? "TRUE 16:9" :
         aspect == kAleksAspect_True169Expanded ? "TRUE 16:9 EXP" : "4:3 CLASSIC";
}

int AleksAspect_FromIni(const char *value) {
  /* "16:9" is what every existing config on a card already says for WIDE, so
   * it keeps that meaning exactly; "Wide" is accepted as its synonym and the
   * new geometry answers only to its own name. */
  if (StringEqualsNoCase(value, "4:3")) return kAleksAspect_4x3;
  if (StringEqualsNoCase(value, "16:9") || StringEqualsNoCase(value, "Wide"))
    return kAleksAspect_Wide;
  /* Longest first: "True16:9" is a prefix of "True16:9Expanded". */
  if (StringEqualsNoCase(value, "True16:9Expanded") ||
      StringEqualsNoCase(value, "True 16:9 Expanded") ||
      StringEqualsNoCase(value, "TrueWideExpanded"))
    return kAleksAspect_True169Expanded;
  if (StringEqualsNoCase(value, "True16:9") ||
      StringEqualsNoCase(value, "TrueWide") ||
      StringEqualsNoCase(value, "True 16:9"))
    return kAleksAspect_True169;
  return -1;
}

static int GetIniSection(const char *s) {
  if (StringEqualsNoCase(s, "[KeyMap]"))
    return 0;
  if (StringEqualsNoCase(s, "[Graphics]"))
    return 1;
  if (StringEqualsNoCase(s, "[Sound]"))
    return 2;
  if (StringEqualsNoCase(s, "[General]"))
    return 3;
  if (StringEqualsNoCase(s, "[Features]"))
    return 4;
  if (StringEqualsNoCase(s, "[GamepadMap]"))
    return 5;
  return -1;
}

bool ParseBool(const char *value, bool *result) {
  bool rv = false;
  switch (*value++ | 32) {
  case '0': if (*value == 0) break; return false;
  case 'f': if (StringEqualsNoCase(value, "alse")) break; return false;
  case 'n': if (StringEqualsNoCase(value, "o")) break; return false;
  case 'o':
    rv = (*value | 32) == 'n';
    if (StringEqualsNoCase(value, rv ? "n" : "ff")) break;
    return false;
  case '1': rv = true; if (*value == 0) break; return false;
  case 'y': rv = true; if (StringEqualsNoCase(value, "es")) break; return false;
  case 't': rv = true; if (StringEqualsNoCase(value, "rue")) break; return false;
  default: return false;
  }
  if (result) {
    *result = rv;
    return true;
  }
  return rv;
}

static bool ParseBoolBit(const char *value, uint32 *data, uint32 mask) {
  bool tmp;
  if (!ParseBool(value, &tmp))
    return false;
  *data = *data & ~mask | (tmp ? mask : 0);
  return true;
}

static bool HandleIniConfig(int section, const char *key, char *value) {
  if (section == 0) {
    for (int i = 0; i < countof(kKeyNameId); i++) {
      if (StringEqualsNoCase(key, kKeyNameId[i].name)) {
        has_keynameid[i] = true;
        ParseKeyArray(value, kKeyNameId[i].id, kKeyNameId[i].size);
        return true;
      }
    }
  } else if (section == 5) {
    for (int i = 0; i < countof(kKeyNameId); i++) {
      if (StringEqualsNoCase(key, kKeyNameId[i].name)) {
        if (i == 1)
          has_joypad_controls = true;
        ParseGamepadArray(value, kKeyNameId[i].id, kKeyNameId[i].size);
        return true;
      }
    }
  } else if (section == 1) {
    if (StringEqualsNoCase(key, "WindowSize")) {
      char *s;
      if (StringEqualsNoCase(value, "Auto")){
        g_config.window_width  = 0;
        g_config.window_height = 0;
        return true;
      }
      while ((s = NextDelim(&value, 'x')) != NULL) {
        if(g_config.window_width == 0) {
          g_config.window_width = atoi(s);
        } else {
          g_config.window_height = atoi(s);
          return true;
        }
      }
    } else if (StringEqualsNoCase(key, "EnhancedMode7")) {
      return ParseBool(value, &g_config.enhanced_mode7);
    } else if (StringEqualsNoCase(key, "NewRenderer")) {
      return ParseBool(value, &g_config.new_renderer);
    } else if (StringEqualsNoCase(key, "IgnoreAspectRatio")) {
      return ParseBool(value, &g_config.ignore_aspect_ratio);
    } else if (StringEqualsNoCase(key, "Fullscreen")) {
      g_config.fullscreen = (uint8)strtol(value, (char**)NULL, 10);
      return true;
    } else if (StringEqualsNoCase(key, "WindowScale")) {
      g_config.window_scale = (uint8)strtol(value, (char**)NULL, 10);
      return true;
    } else if (StringEqualsNoCase(key, "OutputMethod")) {
      g_config.output_method = StringEqualsNoCase(value, "SDL-Software") ? kOutputMethod_SDLSoftware :
                               StringEqualsNoCase(value, "OpenGL") ? kOutputMethod_OpenGL : 
                               StringEqualsNoCase(value, "OpenGL ES") ? kOutputMethod_OpenGL_ES :
                                                                        kOutputMethod_SDL;
      return true;
    } else if (StringEqualsNoCase(key, "LinearFiltering")) {
      return ParseBool(value, &g_config.linear_filtering);
    } else if (StringEqualsNoCase(key, "NoSpriteLimits")) {
      return ParseBool(value, &g_config.no_sprite_limits);
    } else if (StringEqualsNoCase(key, "LinkGraphics")) {
      g_config.link_graphics = value;
      return true;
    } else if (StringEqualsNoCase(key, "Shader")) {
      g_config.shader = *value ? value : NULL;
      return true;
    } else if (StringEqualsNoCase(key, "DimFlashes")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_DimFlashes);
    }
  } else if (section == 2) {
    if (StringEqualsNoCase(key, "EnableAudio")) {
      return ParseBool(value, &g_config.enable_audio);
    } else if (StringEqualsNoCase(key, "AudioFreq")) {
      g_config.audio_freq = (uint16)strtol(value, (char**)NULL, 10);
      return true;
    } else if (StringEqualsNoCase(key, "AudioChannels")) {
      g_config.audio_channels = (uint8)strtol(value, (char**)NULL, 10);
      return true;
    } else if (StringEqualsNoCase(key, "AudioSamples")) {
      g_config.audio_samples = (uint16)strtol(value, (char**)NULL, 10);
      return true;
    } else if (StringEqualsNoCase(key, "EnableMSU")) {
        if (StringEqualsNoCase(value, "opuz"))
        g_config.enable_msu = kMsuEnabled_Opuz;
      else if (StringEqualsNoCase(value, "deluxe"))
        g_config.enable_msu = kMsuEnabled_MsuDeluxe;
      else if (StringEqualsNoCase(value, "deluxe-opuz"))
        g_config.enable_msu = kMsuEnabled_MsuDeluxe | kMsuEnabled_Opuz;
      else 
        return ParseBool(value, (bool*)&g_config.enable_msu);
      return true;
    } else if (StringEqualsNoCase(key, "MSUPath")) {
      g_config.msu_path = value;
      return true;
    } else if (StringEqualsNoCase(key, "MSUVolume")) {
      g_config.msuvolume = atoi(value);
      return true;
    } else if (StringEqualsNoCase(key, "ResumeMSU")) {
      return ParseBool(value, &g_config.resume_msu);
    }
  } else if (section == 3) {
    if (StringEqualsNoCase(key, "Autosave")) {
      g_config.autosave = (bool)strtol(value, (char**)NULL, 10);
      return true;
    } else if (StringEqualsNoCase(key, "ExtendedAspectRatio")) {
      const char* s;
      int h = 224;
      bool nospr = false, novis = false;
      // todo: make it not depend on the order
      while ((s = NextDelim(&value, ',')) != NULL) {
        if (strcmp(s, "extend_y") == 0)
          h = 240, g_config.extend_y = true;
        else if (strcmp(s, "16:9") == 0)
          g_config.extended_aspect_ratio = (h * 16 / 9 - 256) / 2;
        else if (strcmp(s, "16:10") == 0)
          g_config.extended_aspect_ratio = (h * 16 / 10 - 256) / 2;
        else if (strcmp(s, "18:9") == 0)
          g_config.extended_aspect_ratio = (h * 18 / 9 - 256) / 2;
        else if (strcmp(s, "4:3") == 0)
          g_config.extended_aspect_ratio = 0;
        else if (strcmp(s, "unchanged_sprites") == 0)
          nospr = true;
        else if (strcmp(s, "no_visual_fixes") == 0)
          novis = true;
        else
          return false;
      }
      if (g_config.extended_aspect_ratio && !nospr)
        g_config.features0 |= kFeatures0_ExtendScreen64;
      if (g_config.extended_aspect_ratio && !novis)
        g_config.features0 |= kFeatures0_WidescreenVisualFixes;
      return true;
    } else if (StringEqualsNoCase(key, "DisplayPerfInTitle")) {
      return ParseBool(value, &g_config.display_perf_title);
    } else if (StringEqualsNoCase(key, "DisableFrameDelay")) {
      return ParseBool(value, &g_config.disable_frame_delay);
    } else if (StringEqualsNoCase(key, "AleksDisplayMode")) {
      if (StringEqualsNoCase(value, "Normal")) g_config.aleks_display_mode = 0;
      else if (StringEqualsNoCase(value, "Dual")) g_config.aleks_display_mode = 1;
      else if (StringEqualsNoCase(value, "Flip")) g_config.aleks_display_mode = 2;
      else return false;
      return true;
    } else if (StringEqualsNoCase(key, "AleksCompanionPage")) {
      if (StringEqualsNoCase(value, "Map")) g_config.aleks_companion_page = 0;
      else if (StringEqualsNoCase(value, "Dungeon")) g_config.aleks_companion_page = 1;
      else if (StringEqualsNoCase(value, "Items")) g_config.aleks_companion_page = 2;
      else if (StringEqualsNoCase(value, "Gear")) g_config.aleks_companion_page = 3;
      else return false;
      return true;
    } else if (StringEqualsNoCase(key, "AleksCompanionHud")) {
      return ParseBool(value, &g_config.aleks_companion_hud);
    } else if (StringEqualsNoCase(key, "AleksPixelPerfect")) {
      return ParseBool(value, &g_config.aleks_pixel_perfect);
    } else if (StringEqualsNoCase(key, "AleksGameplayAspect")) {
      /* "16:9" is the string old configs wrote for WIDE and must keep meaning
       * WIDE -- the new geometry gets its own string instead. */
      int aspect = AleksAspect_FromIni(value);
      if (aspect < 0) return false;
      g_config.aleks_gameplay_aspect = (uint8)aspect;
      g_config.extended_aspect_ratio = (uint8)AleksAspect_ExtraSide(aspect);
      if (aspect == kAleksAspect_4x3)
        g_config.features0 &= ~(kFeatures0_ExtendScreen64 | kFeatures0_WidescreenVisualFixes);
      else
        g_config.features0 |= kFeatures0_ExtendScreen64 | kFeatures0_WidescreenVisualFixes;
      return true;
    } else if (!SDL_strncasecmp(key, "AleksShortcut", 13) &&
               key[13] >= '0' && key[13] <= '5' && key[14] == 0) {
      g_config.aleks_shortcut[key[13] - '0'] = (uint8)strtol(value, NULL, 10);
      return true;
    } else if (StringEqualsNoCase(key, "AleksHoldToAdvance")) {
      bool on = false;
      if (!ParseBool(value, &on)) return false;
      g_config.aleks_hold_to_advance = on ? 1 : 0;
      return true;
    } else if (StringEqualsNoCase(key, "AleksRetroAchievements")) {
      bool on = false;
      if (!ParseBool(value, &on)) return false;
      g_config.aleks_ra_enable = on ? 1 : 0;
      return true;
    } else if (StringEqualsNoCase(key, "AleksQuickSlot")) {
      /* Stored 0-based; accept the 1-based number the UI shows. */
      long v = strtol(value, NULL, 10);
      if (v < 1) v = 1;
      if (v > 4) v = 4;
      g_config.aleks_quick_slot = (uint8)(v - 1);
      return true;
    } else if (!SDL_strncasecmp(key, "AleksQuickItemSlot", 18) &&
               key[18] >= '0' && key[18] <= '1' && key[19] == 0) {
      long v = strtol(value, NULL, 10);
      g_config.aleks_quick_item_slot[key[18] - '0'] =
          (uint8)(v >= 1 && v <= 20 ? v : 0);
      return true;
    } else if (!SDL_strncasecmp(key, "AleksQuickItemButton", 20) &&
               key[20] >= '0' && key[20] <= '1' && key[21] == 0) {
      g_config.aleks_quick_item_button[key[20] - '0'] =
          (uint8)strtol(value, NULL, 10);
      return true;
    } else if (StringEqualsNoCase(key, "AleksXItemRing")) {
      return ParseBool(value, &g_config.aleks_x_item_ring);
    } else if (StringEqualsNoCase(key, "AleksStoryGuide")) {
      g_config.aleks_story_guide = StringEqualsNoCase(value, "Off") ? 0 :
                                   StringEqualsNoCase(value, "Hints") ? 2 :
                                   StringEqualsNoCase(value, "Detailed") ? 3 : 1;
      return true;
    } else if (StringEqualsNoCase(key, "AleksWideCamera")) {
      g_config.aleks_wide_camera = StringEqualsNoCase(value, "Fixed") ? 1 : 0;
      return true;
    } else if (StringEqualsNoCase(key, "AleksHudMode")) {
      g_config.aleks_hud_mode = StringEqualsNoCase(value, "On") ? 1 :
                                StringEqualsNoCase(value, "Off") ? 2 : 0;
      return true;
    } else if (StringEqualsNoCase(key, "AleksCompanionLayout")) {
      g_config.aleks_companion_layout = StringEqualsNoCase(value, "Tall") ? 1 : 0;
      return true;
    } else if (StringEqualsNoCase(key, "AleksScreenOrder")) {
      g_config.aleks_screen_order = StringEqualsNoCase(value, "CompanionFirst") ? 1 : 0;
      return true;
    } else if (StringEqualsNoCase(key, "AleksScaling")) {
      if (StringEqualsNoCase(value, "Fit")) g_config.aleks_scaling_mode = 0;
      else if (StringEqualsNoCase(value, "SharpPixel")) g_config.aleks_scaling_mode = 1;
      else if (StringEqualsNoCase(value, "Integer")) g_config.aleks_scaling_mode = 2;
      else if (StringEqualsNoCase(value, "Fill")) g_config.aleks_scaling_mode = 3;
      else return false;
      return true;
    } else if (StringEqualsNoCase(key, "AleksDualGameScale")) { g_config.aleks_dual_game_scale = (uint8)atoi(value); return true;
    } else if (StringEqualsNoCase(key, "AleksDualCompanionScale")) { g_config.aleks_dual_companion_scale = (uint8)atoi(value); return true;
    } else if (StringEqualsNoCase(key, "AleksDualGap")) { g_config.aleks_dual_gap = (uint8)atoi(value); return true;
    } else if (StringEqualsNoCase(key, "AleksFlipGameScale")) { g_config.aleks_flip_game_scale = (uint8)atoi(value); return true;
    } else if (StringEqualsNoCase(key, "AleksFlipCompanionScale")) { g_config.aleks_flip_companion_scale = (uint8)atoi(value); return true;
    } else if (StringEqualsNoCase(key, "AleksFlipGap")) { g_config.aleks_flip_gap = (uint8)atoi(value); return true;
    } else if (StringEqualsNoCase(key, "AleksTouchUI")) return ParseBool(value, &g_config.aleks_touch_ui);
    else if (StringEqualsNoCase(key, "AleksTapToEquip")) return ParseBool(value, &g_config.aleks_tap_equip);
    else if (StringEqualsNoCase(key, "Language")) {
      g_config.language = value;
      return true;
    }
  } else if (section == 4) {
    if (StringEqualsNoCase(key, "ItemSwitchLR")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_SwitchLR);
    } else if (StringEqualsNoCase(key, "ItemSwitchLRLimit")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_SwitchLRLimit);
    } else if (StringEqualsNoCase(key, "TurnWhileDashing")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_TurnWhileDashing);
    } else if (StringEqualsNoCase(key, "MirrorToDarkworld")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_MirrorToDarkworld);
    } else if (StringEqualsNoCase(key, "CollectItemsWithSword")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_CollectItemsWithSword);
    } else if (StringEqualsNoCase(key, "BreakPotsWithSword")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_BreakPotsWithSword);
    } else if (StringEqualsNoCase(key, "DisableLowHealthBeep")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_DisableLowHealthBeep);
    } else if (StringEqualsNoCase(key, "SkipIntroOnKeypress")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_SkipIntroOnKeypress);
    } else if (StringEqualsNoCase(key, "ShowMaxItemsInYellow")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_ShowMaxItemsInYellow);
    } else if (StringEqualsNoCase(key, "MoreActiveBombs")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_MoreActiveBombs);
    } else if (StringEqualsNoCase(key, "CarryMoreRupees")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_CarryMoreRupees);
    } else if (StringEqualsNoCase(key, "MiscBugFixes")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_MiscBugFixes);
    } else if (StringEqualsNoCase(key, "GameChangingBugFixes")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_GameChangingBugFixes);
    } else if (StringEqualsNoCase(key, "CancelBirdTravel")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_CancelBirdTravel);
    }
  }
  return false;
}

static bool ParseOneConfigFile(const char *filename, int depth) {
  char *filedata = (char*)ReadWholeFile(filename, NULL), *p;
  if (!filedata)
    return false;
  
  int section = -2;
  g_config.memory_buffer = filedata;

  for (int lineno = 1; (p = NextLineStripComments(&filedata)) != NULL; lineno++) {
    if (*p == 0)
      continue; // empty line
    if (*p == '[') {
      section = GetIniSection(p);
      if (section < 0)
        fprintf(stderr, "%s:%d: Invalid .ini section %s\n", filename, lineno, p);
    } else if (*p == '!' && SkipPrefix(p + 1, "include ")) {
      char *tt = p + 8;
      char *new_filename = ReplaceFilenameWithNewPath(filename, NextPossiblyQuotedString(&tt));
      if (depth > 10 || !ParseOneConfigFile(new_filename, depth + 1))
        fprintf(stderr, "Warning: Unable to read %s\n", new_filename);
      free(new_filename);
    } else if (section == -2) {
      fprintf(stderr, "%s:%d: Expecting [section]\n", filename, lineno);
    } else {
      char *v = SplitKeyValue(p);
      if (v == NULL) {
        fprintf(stderr, "%s:%d: Expecting 'key=value'\n", filename, lineno);
        continue;
      }
      if (section >= 0 && !HandleIniConfig(section, p, v))
        fprintf(stderr, "%s:%d: Can't parse '%s'\n", filename, lineno, p);
    }
  }
  return true;
}

// THE authoritative defaults.  Every field the game reads is set here, so a
// missing or partial zelda3.ini still produces a correct, playable build --
// that is the whole point, and the clean-boot audio/display bug was the proof
// that leaving fields at zero is not safe.
void Config_SetDefaults(void) {
  // --- graphics -----------------------------------------------------------
  g_config.window_width = 1280;      // the Switch handheld surface
  g_config.window_height = 720;
  g_config.window_scale = 1;
  g_config.fullscreen = 0;
  g_config.new_renderer = true;
  g_config.enhanced_mode7 = true;
  g_config.no_sprite_limits = true;
  g_config.ignore_aspect_ratio = false;
  g_config.linear_filtering = false;
  g_config.output_method = kOutputMethod_SDL;
  g_config.extended_aspect_ratio = 0;  // 4:3; must agree with aleks_gameplay_aspect
  g_config.extend_y = false;

  // --- sound --------------------------------------------------------------
  g_config.enable_audio = true;      // <- zero here was the silent clean boot
  g_config.audio_freq = 44100;
  g_config.audio_channels = 2;
  g_config.audio_samples = 1024;
  g_config.enable_msu = 0;
  g_config.msuvolume = 100;

  // --- gameplay -----------------------------------------------------------
  g_config.autosave = false;         // OFF until it is proven safe on hardware
  g_config.features0 = 0;

  // --- ALEKS --------------------------------------------------------------
  g_config.aleks_display_mode = 0;         // NORMAL
  g_config.aleks_gameplay_aspect = 0;      // 4:3, matching extended_aspect_ratio
  g_config.aleks_wide_camera = 0;          // STANDARD
  g_config.aleks_hud_mode = 0;             // AUTO
  g_config.aleks_companion_layout = 0;     // CLASSIC
  g_config.aleks_screen_order = 0;         // game first
  g_config.aleks_companion_hud = true;
  g_config.aleks_companion_page = 0;       // MAP
  g_config.aleks_touch_ui = true;
  g_config.aleks_tap_equip = true;
  g_config.aleks_x_item_ring = false;
  g_config.aleks_story_guide = 0;          // OFF (no content yet)
  g_config.aleks_scaling_mode = 1;
  // Layout numbers from the final ALEKS TMC defaults.
  g_config.aleks_dual_game_scale = 100;
  g_config.aleks_dual_companion_scale = 100;
  g_config.aleks_dual_gap = 20;
  g_config.aleks_flip_game_scale = 100;
  g_config.aleks_flip_companion_scale = 100;
  g_config.aleks_flip_gap = 16;
  for (int i = 0; i < kAleksShortcut_Count; i++)
    g_config.aleks_shortcut[i] = 0;        // OFF
  g_config.aleks_quick_slot = 0;      // picker slot 1 (backend save2.sav)
  g_config.aleks_ra_enable = 0;       // RetroAchievements: opt-in, never default
  g_config.aleks_hold_to_advance = 0; // vanilla dialogue timing by default
  for (int i = 0; i < 2; i++) {
    g_config.aleks_quick_item_slot[i] = 0; // unassigned
    g_config.aleks_quick_item_button[i] = 0;
  }
}

// Write a complete, commented zelda3.ini FROM the live defaults.  Generated
// rather than shipped as a second file on purpose: there is then exactly one
// place a default can be changed.  Returns false if the file cannot be written
// (which is survivable -- the compiled defaults above already gave us a
// correct configuration).
bool Config_WriteDefaultIni(const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f)
    return false;
  fprintf(f,
    "; Zelda3 ALEKS NX -- created automatically on first launch.\n"
    "; Every value here is this build's compiled default; edit freely.\n"
    "; Deleting this file simply recreates it.\n"
    "\n[General]\n"
    "AleksDisplayMode = %s\n"
    "AleksGameplayAspect = %s\n"
    "AleksWideCamera = %s\n"
    "AleksHudMode = %s\n"
    "AleksCompanionLayout = %s\n"
    "AleksScreenOrder = %s\n"
    "AleksCompanionHud = %s\n"
    "AleksCompanionPage = Map\n"
    "AleksTouchUI = %s\n"
    "AleksTapToEquip = %s\n"
    "AleksXItemRing = %s\n"
    "AleksStoryGuide = Off\n"
    "AleksDualGameScale = %u\n"
    "AleksDualCompanionScale = %u\n"
    "AleksDualGap = %u\n"
    "AleksFlipGameScale = %u\n"
    "AleksFlipCompanionScale = %u\n"
    "AleksFlipGap = %u\n"
    "; Autosave is OFF by default; see HARDWARE-HOTFIX-V1.1.md\n"
    "Autosave = %d\n"
    "ExtendedAspectRatio = %s\n",
    g_config.aleks_display_mode == 1 ? "Dual" : g_config.aleks_display_mode == 2 ? "Flip" : "Normal",
    g_config.aleks_gameplay_aspect ? "16:9" : "4:3",
    g_config.aleks_wide_camera ? "Fixed" : "Standard",
    g_config.aleks_hud_mode == 1 ? "On" : g_config.aleks_hud_mode == 2 ? "Off" : "Auto",
    g_config.aleks_companion_layout ? "Tall" : "Classic",
    g_config.aleks_screen_order ? "CompanionFirst" : "GameFirst",
    g_config.aleks_companion_hud ? "true" : "false",
    g_config.aleks_touch_ui ? "true" : "false",
    g_config.aleks_tap_equip ? "true" : "false",
    g_config.aleks_x_item_ring ? "true" : "false",
    g_config.aleks_dual_game_scale, g_config.aleks_dual_companion_scale, g_config.aleks_dual_gap,
    g_config.aleks_flip_game_scale, g_config.aleks_flip_companion_scale, g_config.aleks_flip_gap,
    g_config.autosave ? 1 : 0,
    g_config.extended_aspect_ratio ? "16:9" : "4:3");

  fprintf(f,
    "\n[Graphics]\n"
    "WindowSize = %dx%d\n"
    "Fullscreen = %u\n"
    "WindowScale = %u\n"
    "NewRenderer = %d\n"
    "EnhancedMode7 = %d\n"
    "IgnoreAspectRatio = %d\n"
    "NoSpriteLimits = %d\n",
    g_config.window_width, g_config.window_height,
    g_config.fullscreen, g_config.window_scale,
    g_config.new_renderer ? 1 : 0, g_config.enhanced_mode7 ? 1 : 0,
    g_config.ignore_aspect_ratio ? 1 : 0, g_config.no_sprite_limits ? 1 : 0);

  fprintf(f,
    "\n[Sound]\n"
    "EnableAudio = %d\n"
    "AudioFreq = %u\n"
    "AudioChannels = %u\n"
    "AudioSamples = %u\n"
    "EnableMSU = %u\n",
    g_config.enable_audio ? 1 : 0, g_config.audio_freq,
    g_config.audio_channels, g_config.audio_samples, g_config.enable_msu);

  fprintf(f,
    "\n[Features]\n"
    "; Quality-of-life options, all off by default.  The companion's\n"
    "; SETTINGS -> GAMEPLAY -> QOL page toggles these at runtime.\n"
    "ItemSwitchLR = 0\n"
    "TurnWhileDashing = 0\n"
    "MirrorToDarkworld = 0\n"
    "CollectItemsWithSword = 0\n"
    "BreakPotsWithSword = 0\n"
    "DisableLowHealthBeep = 0\n"
    "SkipIntroOnKeypress = 0\n"
    "ShowMaxItemsInYellow = 0\n"
    "MoreActiveBombs = 0\n"
    "CarryMoreRupees = 0\n"
    "MiscBugFixes = 0\n"
    "CancelBirdTravel = 0\n"
    "GameChangingBugFixes = 0\n");

  return fclose(f) == 0;
}

void ParseConfigFile(const char *filename) {
  Config_SetDefaults();

  if (filename != NULL || !ParseOneConfigFile("zelda3.user.ini", 0)) {
    if (filename == NULL)
      filename = "zelda3.ini";
    if (!ParseOneConfigFile(filename, 0))
      fprintf(stderr, "Warning: Unable to read config file %s\n", filename);
  }
  RegisterDefaultKeys();
}

bool Config_SaveAleksDisplaySettings(void) {
  static const char *kModes[] = { "Normal", "Dual", "Flip" };
  /* Must cover every value SecondScreenSDL_CycleTab can store, or the page
   * the player left on silently persists as "Map". */
  static const char *kPages[] = { "Map", "Dungeon", "Items", "Gear", "Guide", "Save" };
  #define kPageCount ((int)(sizeof(kPages) / sizeof(kPages[0])))
  FILE *f = fopen("zelda3.ini", "ab");
  if (!f) return false;
  static const char *kScale[] = { "Fit", "SharpPixel", "Integer", "Fill" };
  fprintf(f, "\n[General]\nAleksDisplayMode=%s\nAleksCompanionPage=%s\nAleksCompanionHud=%s\nAleksPixelPerfect=%s\nAleksGameplayAspect=%s\nAleksScaling=%s\nAleksDualGameScale=%u\nAleksDualCompanionScale=%u\nAleksDualGap=%u\nAleksFlipGameScale=%u\nAleksFlipCompanionScale=%u\nAleksFlipGap=%u\nAleksTouchUI=%s\nAleksTapToEquip=%s\n",
          kModes[g_config.aleks_display_mode < 3 ? g_config.aleks_display_mode : 0],
          kPages[g_config.aleks_companion_page < kPageCount ? g_config.aleks_companion_page : 0],
          g_config.aleks_companion_hud ? "true" : "false",
          g_config.aleks_pixel_perfect ? "true" : "false",
          AleksAspect_IniValue(g_config.aleks_gameplay_aspect), kScale[g_config.aleks_scaling_mode < 4 ? g_config.aleks_scaling_mode : 1], g_config.aleks_dual_game_scale, g_config.aleks_dual_companion_scale, g_config.aleks_dual_gap, g_config.aleks_flip_game_scale, g_config.aleks_flip_companion_scale, g_config.aleks_flip_gap, g_config.aleks_touch_ui ? "true" : "false", g_config.aleks_tap_equip ? "true" : "false");
  return fclose(f) == 0;
}
