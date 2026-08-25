# Phase 6B — Companion UI V2

The compositor is unchanged.  The companion now renders in the Esteban 3DS
native 320x240 coordinate system, then the existing TMC-derived layout scales
the finished texture for DUAL and FLIP.

The item-atlas defect was fixed by using the 16x16 source rectangles from
Esteban's `icons.json`, rather than treating an offset inside the 160x128
sheet as a new sheet.  Map is contextual: it shows the donor overworld while
outdoors and the real dungeon floor inside a dungeon.  Tabs are Gear, Map,
Items, and Settings; Dungeon is no longer an exposed empty-state page.
