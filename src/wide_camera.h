#ifndef ZELDA3_WIDE_CAMERA_H_
#define ZELDA3_WIDE_CAMERA_H_

/* True on the three module-14 screens that are drawn in their own
 * 256-pixel map space rather than in gameplay camera space: the dungeon
 * map (submodule 3), the world map (7) and the flute destination map (10).
 * Upstream zelda-alttp-3ds v3.0. */
int WideCamera_IsMapMenu(int module, int submodule);

int WideCamera_Unwrap16(int value, int reference);
int WideCamera_ClampToBounds(int logical_x, int left_bound,
                             int right_bound, int margin);
int WideCamera_FindDungeonTransitionEnd(int start_x, int direction,
                                        int target);
int WideCamera_InterpolateTransition(int logical_x, int start_logical_x,
                                     int start_visual_x, int end_offset,
                                     int direction, int distance);

#endif  // ZELDA3_WIDE_CAMERA_H_
