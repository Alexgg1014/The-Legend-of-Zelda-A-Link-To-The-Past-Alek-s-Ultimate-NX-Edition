# ALEKS Display System V1.1

The validated TMC-style single-renderer compositor remains the presentation
boundary: NORMAL keeps Zelda's original logical path, while DUAL and FLIP use
physical output coordinates and make one final present per frame.

DUAL uses a 1280x720 design layout: integer-scaled gameplay on the left and a
400x600 companion on the right.  The shared companion provides Map (including
Light/Dark World and indoor exit fallback), Dungeon (donor map/visited-room
data/current-room indicator), Items (live inventory block), Gear (live SRAM
progression values), HUD, and page tabs.

Controls: ZR+L3 Normal/Dual, ZL+ZR pages, ZL+R3 Settings.  Settings uses the
D-pad, persists `AleksDisplayMode`, `AleksCompanionPage`,
`AleksCompanionHud`, and `AleksPixelPerfect` in `zelda3.ini`.
`AleksGameplayAspect` selects 4:3 or 16:9 on the next launch.

FLIP composes the same game texture and companion texture on one 720x1280
target and rotates the completed canvas 270 degrees.  Touch and story-guide
work are deliberately deferred.
