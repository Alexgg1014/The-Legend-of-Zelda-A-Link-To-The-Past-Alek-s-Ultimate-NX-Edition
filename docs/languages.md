# Playing in another language

ALEKS Ultimate NX ships with English. Other languages come from a ROM **you
already own** — nothing translated is distributed with this build, and nothing
is downloaded.

There are two ways in, and which one you need depends on what kind of ROM you
have.

---

## 1. A fan translation of the US ROM — nothing to install

**This covers Spanish, Polish, Portuguese, Dutch, Swedish and English Redux.**
They are patches applied to the US ROM, so the game reads them directly.

Drop the `.sfc`/`.smc` into `switch/Zelda3/languages/` and start the game. It is
picked up automatically, and the result is cached beside it as a `.z3lang` so
later launches are instant.

Then: `ZL + R3` → `SYSTEM` → `LANGUAGE`, pick it, and say yes to the restart.

This works because translation patches keep the US text engine intact, so the
text, the font and the character widths can be lifted straight out of the ROM.

If it doesn't appear, `startup.log` says why. Look for a line starting
`LANGUAGE`.

---

## 2. An official PAL ROM — build a pack on a PC

**This covers French, German, French-Canadian and European English** — the
cartridges Nintendo actually sold in Europe.

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

The tool handles every language the project knows, not just the PAL ones:

| code | language | where it comes from |
|---|---|---|
| `fr` | French | official PAL cartridge |
| `de` | German | official PAL cartridge |
| `fr-c` | French (Canada) | official cartridge |
| `en` | English (Europe) | official PAL cartridge — a different script from the US one |
| `es` `pl` `pt` `nl` `sv` `redux` | Spanish, Polish, Portuguese, Dutch, Swedish, English Redux | fan patches of the US ROM |
| `us` | English | the original US ROM |

The fan-patch rows do not need this tool at all — see route 1 above. They are
listed because building a pack works for them too, which is handy if you would
rather keep a 40 KB pack on the card than a 1 MB ROM.

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
