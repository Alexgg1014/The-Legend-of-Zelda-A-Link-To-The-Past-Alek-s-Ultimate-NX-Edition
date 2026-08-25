# Third-Party Notices

ALEKS Ultimate NX is an integration project. This file records what it is
built from, what was adapted, and on what terms — including where those terms
are not formally declared by the original author.

Attribution here does not alter anyone's copyright, and no license is claimed
on another author's behalf.

---

## Zelda3 engine

ALEKS Ultimate NX is based on [`snesrev/zelda3`](https://github.com/snesrev/zelda3),
distributed under the terms declared by that upstream project (MIT — see
[LICENSE.txt](LICENSE.txt), © 2022 snesrev, © 2021 elzo_d).

This covers the reverse-engineered game engine, PPU, renderer, save handling,
the optional gameplay features exposed here as quality-of-life toggles, and
the widescreen and 240-line rendering paths the aspect modes build on.

No ALEKS authorship is claimed over any of it.

---

## Portable / second-screen lineage

Portable second-screen functionality in ALEKS Ultimate NX was primarily
adapted from [`EstebanPdN/zelda-alttp-3ds`](https://github.com/EstebanPdN/zelda-alttp-3ds),
**with permission from EstebanPdN**, who described the project as open code.

Adapted from that port:

| File | Description |
|---|---|
| `src/second_screen.c` | companion state, art generation, queued engine requests |
| `src/second_screen_sdl.c` | the companion interface: map, dungeon maps, items, gear, settings |
| `src/second_screen_tables.h` | companion tile and layout tables |
| `src/ss_sheets.h`, `src/ss_textures.h` | generated companion UI art and cell tables |
| `src/wide_camera.c`, `src/wide_camera.h` | fixed widescreen camera |
| translation extraction in `src/aleks_lang.c` | ported from `TranslationExtractor.java`: ROM offsets, message walk, dictionary bounds, packed-array layout and ROM compatibility lists |

EstebanPdN's project itself incorporates and extends earlier dual-screen work
from [`samyost1/zelda3-android`](https://github.com/samyost1/zelda3-android),
which is therefore credited as part of the second-screen lineage.

**Scope of permission.** The permission granted by EstebanPdN covers his own
work. It does not relicense, and is not presented as relicensing, original
contributions by samyost1 or by any other author in that lineage.

**License status.** Neither donor repository declares an explicit license.
Both derive from MIT-licensed upstream code, so upstream-derived portions
carry MIT — but the second-screen system is largely new work by its authors,
and no license claim is made over it here. It is included with attribution and
with EstebanPdN's permission, and is not presented as MIT.

No claim is made that original contributions from these donor projects were
authored by ALEKS.

If you are EstebanPdN or samyost1 and would like different wording, different
terms, or your own license statement reflected here, please open an issue.

---

## ALEKS TMC (The Minish Cap, Switch)

Switch platform implementation reference. The architecture this port follows
for its display composition, layout and input transforms, its runtime settings
and shortcut patterns, its crash context and reporting design, and the
RetroAchievements platform integration — network lifecycle, token storage,
unlock queue, badge worker and the single overlay stage the toast draws in.

Adapted here with a Zelda3-specific memory adapter and game identification.

---

## rcheevos

The official RetroAchievements client library, vendored under
`third_party/rcheevos/`.

MIT © RetroAchievements.org — see
[`third_party/rcheevos/LICENSE`](third_party/rcheevos/LICENSE).

---

## Other bundled components

| Component | Location | License |
|---|---|---|
| stb (`stb_image`) | `third_party/stb/` | Public domain / MIT |
| Opus (stripped decoder) | `third_party/opus-1.3.1-stripped/` | BSD |
| gl_core loader | `third_party/gl_core/` | Generated loader, per its own terms |

---

## Toolchain and platform

devkitPro, devkitA64, libnx and SDL2 (switch-sdl2), each under its own
license. None is redistributed in this repository.

---

## Reference saves

`saves/ref/` contains the replay reference saves that ship with upstream
`snesrev/zelda3`, retained as upstream test fixtures. They are not player
saves and are not ALEKS content.

---

## Game data

No ROM, no extracted game assets, no `zelda3_assets.dat`, and no translated
ROM or extracted translation data is included in this repository or in any
release. All such data is generated locally by the user from a copy of the
game they legally own, and remains on their own installation.

*The Legend of Zelda*, *A Link to the Past* and related trademarks belong to
Nintendo. This project is not affiliated with, authorised by or endorsed by
Nintendo.
