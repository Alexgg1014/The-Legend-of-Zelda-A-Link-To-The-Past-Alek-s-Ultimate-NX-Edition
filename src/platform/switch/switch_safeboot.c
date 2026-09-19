/* switch_safeboot.c -- the way out of a renderer you cannot see Settings in.
 *
 * With OutputMethod=OpenGL the game presents straight to the window and the
 * companion (where SETTINGS lives) is never drawn, so once a player is there
 * nothing in the game can bring them back.  Holding ZL+R3 -- the same combo
 * that opens SETTINGS -- while the game boots forces the SDL renderer and
 * writes it to the ini.  Read with libnx directly because this runs before
 * SDL_Init; SDL configures the pads again afterwards and does not mind. */
#include <switch.h>
#include <stdbool.h>

bool SwitchSafeBoot_HeldZLR3(void) {
  PadState pad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);
  padUpdate(&pad);
  u64 b = padGetButtons(&pad);
  return (b & HidNpadButton_ZL) && (b & HidNpadButton_StickR);
}
