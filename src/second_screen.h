#ifndef ZELDA3_SECOND_SCREEN_H_
#define ZELDA3_SECOND_SCREEN_H_

#include <stdbool.h>
#include "types.h"

// The core is compiled now, but no display path consumes it in Phase 3A.
#ifndef ALEKS_SECOND_SCREEN_CORE
#define ALEKS_SECOND_SCREEN_CORE 1
#endif

typedef enum AleksDisplayMode {
  ALEKS_DISPLAY_NORMAL,
  ALEKS_DISPLAY_DUAL,
  ALEKS_DISPLAY_FLIP,
} AleksDisplayMode;

typedef struct SecondScreenState {
  int module;
  int area;
  int dungeon;
  int link_x;
  int link_y;
  int equipped_slot;
  int rupees;
  int bombs;
  int arrows;
  bool indoors;
  bool dark_world;
} SecondScreenState;

void SecondScreen_Init(void);
bool SecondScreen_IsInitialized(void);
void SecondScreen_GetState(SecondScreenState *state);

int SS_GetLinkX(void);
int SS_GetLinkY(void);
int SS_GetArea(void);
int SS_GetModule(void);
bool SS_IsIndoors(void);
int SS_GetEquippedSlot(void);
int SS_GetEquippedSlotX(void);
int SS_GetDungeon(void);
void SS_ReadSram(uint8 *out, int n);
void SS_ReadDungFlags(uint8 *out, int n);
bool SS_GetIndoorExit(int *out);
bool SS_RenderIconSheet(uint32 *pixels);
bool SS_RenderGlyphSheet(uint32 *pixels);
bool SS_RenderLetterSheet(uint32 *pixels);
bool SS_RenderWorldMap(uint32 *pixels, bool dark);
bool SS_RenderLinkFace(uint32 *pixels, int chunk);
bool SS_RenderLinkFaceArmor(uint32 *pixels, int chunk);
int SS_GetLinkFacing(void);
void SS_RequestSaveState(int slot);
void SS_RequestLoadState(int slot);
bool SS_TakeThumbnail(uint32 *out);
void SS_SetAutosave(bool on);
/* Armed-capture state, written by the input layer in main.c and consumed by
 * the companion: -1 idle, -2 armed and waiting, >= 0 a captured button. */
extern volatile int g_ss_capture_button;
void SS_ArmButtonCapture(bool arm);
int SS_GetCapturedButton(void);
void SS_GetGamepadControls(int *out12);
void SS_SetGamepadControls(const int *in12);
int SS_GetDungeonLayout(int palace, uint8 *out, int cap);
bool SS_RenderDungeonFloor(int palace, int floor, uint32 *pixels);
bool SS_RenderMapIcons(int palace, uint32 *pixels);
/* Queue an inventory-grid slot (1..20) to become the equipped Y item on the
 * game thread.  Validates ownership itself and returns false if it refused,
 * so callers never need their own copy of the inventory rules. */
bool SS_EquipSlot(int slot);
bool SS_CanEquipSlot(int slot);
/* Quick Item: USE the item in this 1..20 slot now, without changing the item
 * equipped to Y.  Returns false when the slot is invalid or not owned. */
bool SS_RequestUseItemSlot(int slot);
void SS_AssignSlotX(int slot);
void SS_SetWidescreen(bool on);
void SS_SetWideEdgeMode(int mode);
int SS_GetWideEdgeMode(void);
void SS_Set3DSDisplayMode(int mode);
void SS_Set3DSWideEdgeMode(int mode);
void SS_RequestMemoryDump(const char *dump_dir);
void SS_RequestRestart(void);
unsigned SS_GetFeatures(void);
void SS_SetFeature(unsigned mask, bool on);
bool SS_IsWidescreen(void);
/* Tri-state aspect (kAleksAspect_*), queued and applied on the game thread
 * exactly like SS_SetWidescreen, which is now a wrapper over it. */
void SS_SetAspect(int aspect);
int SS_GetAspect(void);
void SS_SetHudHidden(bool hide);
bool SS_IsHudHidden(void);
void SecondScreen_RunFrameHook(void);
void SecondScreen_CaptureFrameHook(const uint8 *px, int pitch, int width, int height);

// Future single-output compositor API. No implementation renders in Phase 3A.
void AleksSecondScreen_Update(void);
const uint32 *AleksSecondScreen_GetPixels(int *width, int *height);

#endif  // ZELDA3_SECOND_SCREEN_H_
