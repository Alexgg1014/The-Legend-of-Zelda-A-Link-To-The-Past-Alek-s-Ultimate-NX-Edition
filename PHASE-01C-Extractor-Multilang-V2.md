# ALEKS Zelda3 NX — Phase 1C V2

## Delivered baseline

`Zelda3-ALEKS-NX-PHASE1C.nro` is a Switch-first build with a bundled BPS patch in RomFS. It creates `zelda3_assets.dat` locally from a user-supplied SNES ROM on first launch. No ROM, generated asset blob, save, GBA data, or translated donor data is bundled.

## First-run flow

1. The app uses `sdmc:/switch/Zelda3` as its working folder.
2. If `zelda3_assets.dat` is missing or structurally invalid, it shows a controller-only setup screen.
3. Press **A** to scan that folder for `.sfc` and `.smc` files; press **B** to leave cleanly.
4. Choose a candidate with Up/Down and confirm with **A**.
5. The extractor accepts exactly a 1,048,576-byte SNES image, or a 1,049,088-byte image after removing its 512-byte copier header. It applies the bundled BPS in memory, validates the resulting asset structure, writes `zelda3_assets.tmp`, validates it again, then renames it to `zelda3_assets.dat`.
6. A mismatch, invalid output, or write failure remains in the setup UI with a readable error; no original ROM is altered.

`startup.log` records boot stages plus `[EXTRACT 01]` through `[EXTRACT 10]`, including ROM size/header handling, BPS processing, temporary validation, and final rename.

## Asset contract

The generated blob must contain the current build's asset signature, expected asset count, bounded name table, aligned asset offsets, and bounded asset payloads. A file that merely exists is not accepted.

## Language-provider architecture

The active game remains the clean base provider in this phase. The intended selection model is:

```text
LanguageProvider
  CleanBaseProvider       -> generated clean SNES assets
  TranslatedSnesProvider  -> separately verified translated SNES source
  GbaPalProvider          -> separately verified legal GBA-derived source
```

Each provider must expose its language label, source validation rule, message/font assets, and a compatibility manifest. It must not silently overwrite the clean base assets. The current asset map keeps dialogue, dialogue font, and dialogue map together, so making a partial replacement without a runtime overlay would risk incoherent text rendering; provider activation is intentionally deferred until that overlay and per-provider validation exist.

## Research status

The local donor reference is EstebanPdN's `zelda-alttp-3ds`: its first-launch design supplied the BPS extraction model adopted here. Its translated-SNES profile is reference architecture only; it has not been copied into the Switch package. No legally supplied GBA PAL source was present locally, so the GBA provider is documented but not enabled. Any later GBA provider needs a reproducible, user-owned source path and validation record before it can be selectable.

## Test boundary

Phase 1C is limited to first-run extraction and architecture documentation. It does not add widescreen, dual screen, remote control, online update/download, persistent game UI, cloud saves, or generated sample assets.
