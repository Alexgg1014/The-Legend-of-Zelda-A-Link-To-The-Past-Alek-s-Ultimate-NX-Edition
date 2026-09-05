# Playing in another language

ALEKS Ultimate NX ships with English. Other languages come from a ROM **you
already own** — nothing translated is distributed with this build, and nothing
is downloaded.

There are two ways in, and which one you need depends on what kind of ROM you
have.

---

## 1. A fan translation of the US ROM — nothing to install

Drop the `.sfc`/`.smc` into `switch/Zelda3/languages/` and start the game. It is
picked up automatically, and the result is cached beside it as a `.z3lang` so
later launches are instant.

Then: `ZL + R3` → `SYSTEM` → `LANGUAGE`, pick it, and say yes to the restart.

This works because translation patches keep the US text engine intact, so the
text, the font and the character widths can be lifted straight out of the ROM.

If it doesn't appear, `startup.log` says why. Look for a line starting
`LANGUAGE`.

---

## 2. An official PAL ROM (French, German, Spanish…) — build a pack on a PC

Official European releases use a different text command encoding and store
everything at different addresses, so the on-device extractor refuses them
rather than producing convincing garbage.

The game can still *display* them — the engine has a complete EU text decoder.
It just can't do the extraction itself. So you do that once, on a PC:

```bash
python tools/make_language_pack.py "your-rom.sfc"
```

It prints what it found and writes `fr.z3lang` (or `de.z3lang`, etc.). Copy that
file into `switch/Zelda3/languages/` — **the ROM itself does not go on the SD
card** — and pick the language in `SETTINGS → SYSTEM → LANGUAGE`.

You need Python 3. Run it from the repository root; it uses the tables in
`assets/`.

Useful flags:

| flag | what it does |
|---|---|
| `--lang fr-c` | force a language when the ROM isn't auto-detected |
| `--name FRANCAIS` | change the name shown in SETTINGS |
| `-o path.z3lang` | write somewhere else |
| `--self-test rom` | verify the tool against a US ROM |

Supported: `de`, `fr`, `fr-c`, `es`, `pl`, `pt`, `nl`, `sv`, `en`, `redux`, `us`.

---

## Where things live

```
switch/Zelda3/languages/
    fr.z3lang          a pack built on a PC, or cached from a ROM below
    my-translation.smc a fan translation of the US ROM
```

Up to 8 languages, plus the built-in English. If a ROM and a pack claim the same
language, the ROM wins — it's the one you can see.

---

## Why it's done this way

Everything is derived from your own ROM, on your own machine, and stays there.
The project ships no game data, which is also why there are no ready-made
language packs to download: making one requires a ROM, and that has to be yours.
