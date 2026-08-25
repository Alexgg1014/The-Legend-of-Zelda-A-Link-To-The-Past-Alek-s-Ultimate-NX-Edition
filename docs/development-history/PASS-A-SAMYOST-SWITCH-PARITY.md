# Pass A — Samyost Switch parity

## Implemented

- The Switch compositor remains the only SDL window and final presenter for NORMAL, DUAL and FLIP.
- Samyost companion actions now run on the Zelda game thread.  Tap-to-equip uses the donor's queued HUD item path rather than SRAM edits.
- Samyost title/cinema module detection now drives a Triforce companion frame through the same DUAL/FLIP texture path.  This includes title, file-select/early states and donor cinema modules.
- The existing donor map, dungeon floor, item atlas, gear, HUD and indoor-exit helpers remain the companion data source.
- The companion settings page now uses the donor-facing widescreen and HUD state helpers; Switch display/scaling remains in the TMC-derived adapter.

## Donors used

- Samyost core: `second_screen.c`, `second_screen_tables.h` and the Linux SDL module-state, cinema and input semantics in `platform/linux/second_screen_sdl.c`.
- Esteban comparison: its `second_screen.c` was checked for platform-independent map/dungeon rendering; no divergent 3DS-only API was imported in this pass.
- ALEKS TMC baseline: existing single-present DUAL/FLIP layout and inverse-FLIP touch mapping in `aleks_compositor.c`.

## Changed files

- `src/aleks_compositor.c` — donor module-state cinema/Triforce path, donor-backed settings controls, guarded tap-to-equip.
- `src/second_screen.c` and `.h` — queued donor actions restored and executed on the game thread; desktop-only remap/CRT calls remain intentionally unavailable on Switch.
- `src/main.c` — executes queued companion actions before `ZeldaRunFrame`.

## Remaining hardware validation

Validate title/file select/cinema, special-area and cave marker fallback, dungeon floors, item taps, and DUAL/FLIP geometry on the physical Switch.  Desktop-only gamepad remapping and CRT options are deliberately left to the existing Switch controls and are not part of Pass A.

## Build

`Zelda3-ALEKS-NX-SAMYOST-PARITY-A.nro` was built successfully with the current devkitPro/libnx toolchain in a temporary short-path build copy, because the inherited Makefile does not support the space in the working folder name.  No ROM, `zelda3_assets.dat`, SRAM or Nintendo asset dump is included.
