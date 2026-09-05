#!/usr/bin/env python3
"""
make_language_pack.py -- build a .z3lang language pack for ALEKS Ultimate NX.

WHY THIS EXISTS
---------------
The Switch build extracts translations on-device, but only from ROMs that keep
the stock US text layout -- which is what fan translation patches of the US ROM
do.  Official PAL releases (French, German, French-Canadian) use the EU command
encoding and different offsets, so the on-device extractor honestly refuses
them.  The engine can already RENDER EU text: messaging.c's Text_DecodeCmd has
a complete EU branch, selected by bit 0 of the dialogue flags.  The only
missing piece was a way to produce the data.

This tool is that piece.  It runs on a PC, reads YOUR OWN ROM, and writes a
small .z3lang pack you drop in switch/Zelda3/languages/.  Nothing copyrighted
is distributed by the project: the pack is derived entirely from a file you
already own, exactly like the on-device extractor.

All the hard knowledge -- alphabets, dictionaries, command tables and the PAL
ROM addresses -- is upstream's, in assets/text_compression.py.  This tool only
re-packs it into the format aleks_lang.c reads.

USAGE
    python tools/make_language_pack.py <rom.sfc>
    python tools/make_language_pack.py <rom.sfc> --code fr --name FRANCAIS
    python tools/make_language_pack.py --self-test <us-rom.sfc>

Then copy the resulting <code>.z3lang to switch/Zelda3/languages/ and pick the
language in SETTINGS -> SYSTEM -> LANGUAGE.
"""

import argparse
import os
import struct
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ASSETS = os.path.join(os.path.dirname(_HERE), 'assets')
sys.path.insert(0, _ASSETS)

import util                 # noqa: E402
import text_compression as tc   # noqa: E402

# ---------------------------------------------------------------------------
# Font locations, lifted from assets/sprite_sheets.py kFontTypes so this tool
# does not drag in that module's image dependencies.
#   code : (gfx_addr, glyph_count, (width_table_addr, width_count))
# ---------------------------------------------------------------------------
kFontTypes = {
    'us':   (0x8e8000, 256, (0x8ECADF, 99)),
    'de':   (0x0CC6E8, 256, (0x8CDECF, 112)),
    'fr':   (0x0CC6E8, 256, (0x8CDEAF, 112)),
    'fr-c': (0x0CD078, 256, (0x8CE83F, 112)),
    'en':   (0x8E8000, 256, (0x8ECAFF, 102)),
    'es':   (0x8e8000, 256, (0x8ECADF, 99)),
    'pl':   (0x8e8000, 256, (0x8ECADF, 99)),
    'pt':   (0x8e8000, 256, (0x8ECADF, 121)),
    'redux': (0x8e8000, 256, (0x8ECADF, 99)),
    'nl':   (0x8e8000, 256, (0x8ECADF, 99)),
    'sv':   (0x8e8000, 256, (0x8ECADF, 99)),
}

# Display names for the SETTINGS row.  Anything not listed falls back to the
# uppercased language code, which is what the on-device extractor does too.
kDisplayNames = {
    'us': 'ENGLISH', 'en': 'ENGLISH', 'redux': 'ENGLISH REDUX',
    'de': 'DEUTSCH', 'fr': 'FRANCAIS', 'fr-c': 'FRANCAIS CA',
    'es': 'ESPANOL', 'pl': 'POLSKI', 'pt': 'PORTUGUES',
    'nl': 'NEDERLANDS', 'sv': 'SVENSKA',
}

PACK_MAGIC = b'AZL3'
LANG_FLAG_EU_TEXT = 1

# The message the PAL scripts are missing; upstream splices it back in so the
# numbering the game hardcodes stays aligned.
kMissingMessage4 = ("[Speed 00]0- [Number 00]. 1- [Number 01][2]"
                    "2- [Number 02]. 3- [Number 03]")


def load_rom_any(path):
    """Load a ROM from any path.

    upstream's util resolves what it is given against its own default ROM
    directory, and under the MSYS Python a Windows-style "C:/..." is not
    absolute, so the join mangles it.  Pointing that default at the ROM's own
    directory and passing the bare filename works everywhere and leaves
    upstream's code alone.
    """
    path = os.path.abspath(path)
    if not os.path.isfile(path):
        raise SystemExit('No such ROM: %s' % path)
    saved = util.DEFAULT_ROM_DIRECTORY
    try:
        util.DEFAULT_ROM_DIRECTORY = os.path.dirname(path)
        return util.load_rom(os.path.basename(path), True)
    finally:
        util.DEFAULT_ROM_DIRECTORY = saved


def pack_arr(elements):
    """Mirror of pack_arr() in src/aleks_lang.c.

    Layout: (n-1) cumulative offsets, then the concatenated element data, then
    a 2-byte trailer holding (n-1), with 8192 added when the offsets are 32-bit.
    """
    if not elements:
        raise ValueError('cannot pack an empty array')
    n = len(elements)
    data = sum(len(e) for e in elements)
    wide = (data - len(elements[-1])) >= 65536 or n > 8192
    width = 4 if wide else 2
    out = bytearray((n - 1) * width + data + 2)
    cum = 0
    for i, e in enumerate(elements):
        if i != 0:
            off = (i - 1) * width
            if wide:
                struct.pack_into('<I', out, off, cum)
            else:
                struct.pack_into('<H', out, off, cum)
        base = (n - 1) * width + cum
        out[base:base + len(e)] = e
        cum += len(e)
    struct.pack_into('<H', out, len(out) - 2, (n - 1) + (8192 if wide else 0))
    return bytes(out)


def extract_messages(get_byte, info):
    """Walk the script exactly the way src/aleks_lang.c does.

    Returns the RAW ROM bytes of each message, terminator stripped.  For US
    that is already what the engine wants; for EU it is not -- see
    build_messages().
    """
    msgs = []
    cur = bytearray()
    p, rom_idx = info.rom_addrs[0], 1
    guard = 0
    while True:
        guard += 1
        if guard > 0x20000 or len(msgs) > 1000:
            raise ValueError('script walk did not terminate sanely')
        c = get_byte(p)
        if c == info.FINISH:
            break
        if c == info.SWITCH_BANK:
            if rom_idx >= len(info.rom_addrs):
                raise ValueError('more bank switches than known banks')
            p = info.rom_addrs[rom_idx]
            rom_idx += 1
            continue
        if info.COMMAND_START <= c < info.SWITCH_BANK:
            ln = info.command_lengths[c - info.COMMAND_START]
        else:
            ln = 1
        for i in range(ln):
            cur.append(get_byte(p + i))
        p += ln
        if c == 0x7F:                      # EndMessage, in both encodings
            cur.pop()                      # the terminator is not stored
            msgs.append(bytes(cur))
            cur = bytearray()
    if len(msgs) < 300:
        raise ValueError('only %d messages found; wrong ROM or addresses'
                         % len(msgs))
    return msgs


def build_messages(rom, lang):
    """Produce the message bytes the ENGINE wants, which is not what a PAL ROM
    holds.

    THE TRAP.  A PAL ROM does not store text in the form the engine decodes.
    Upstream's pipeline re-encodes it into what text_compression.py calls the
    "new format": commands move from 0x70.. up to 0x80..0x87, freeing the low
    bytes for the larger European alphabet, and the dictionary base differs
    between encoding (0x88) and decoding (0x90).  messaging.c's EU branch of
    Text_DecodeCmd is written against THAT form -- it treats every byte below
    0x7f as a letter.  Handing it raw PAL bytes would decode commands as
    letters and dictionary tokens shifted by eight: convincing garbage, which
    is the exact failure this feature exists to avoid.

    So the text is decoded out of the ROM and re-encoded through upstream's own
    compressor.  For US the two forms are identical (org encoder, dictionary
    base 0x88 both ways), which is what --self-test proves.
    """
    texts = [t for t, _src in tc.decode_strings_generic(rom.get_byte, lang)]
    if len(texts) == 396:
        # PAL scripts are one message short; splice it back so every later
        # message keeps the index the game hardcodes.
        texts.insert(4, kMissingMessage4)
        print('  spliced the missing message 4 (PAL script)')
    return [bytes(m) for m in tc.compress_strings(texts, lang)]


def build_pack(rom_path, code=None, name=None, lang=None):
    rom = load_rom_any(rom_path)
    lang = lang or rom.language
    if lang not in tc.kLanguages:
        raise SystemExit('Unsupported language %r. Known: %s'
                         % (lang, ', '.join(sorted(tc.kLanguages))))
    if lang not in kFontTypes:
        raise SystemExit('No font location known for %r' % lang)

    info = tc.kLanguages[lang]
    is_eu = isinstance(info, tc.LangEU)

    msgs = build_messages(rom, lang)
    print('  messages:   %d' % len(msgs))

    dictionary = [bytes(d) for d in tc.encode_dictionary(lang)]
    print('  dictionary: %d entries' % len(dictionary))

    gfx_addr, glyphs, (w_addr, w_count) = kFontTypes[lang]
    font_gfx = bytes(rom.get_bytes(gfx_addr, glyphs * 16))
    font_widths = bytes(rom.get_bytes(w_addr, w_count))
    print('  font:       %d bytes of tiles, %d widths' %
          (len(font_gfx), len(font_widths)))

    dialogue = pack_arr([pack_arr(dictionary), pack_arr(msgs)])
    font = pack_arr([font_gfx, font_widths])

    code = code or lang.replace('-', '_')
    name = name or kDisplayNames.get(lang, code.upper())
    flags = LANG_FLAG_EU_TEXT if is_eu else 0

    hdr = bytearray(52)
    hdr[0:4] = PACK_MAGIC
    struct.pack_into('<I', hdr, 4, len(dialogue))
    struct.pack_into('<I', hdr, 8, len(font))
    hdr[12:12 + min(15, len(code))] = code.encode('ascii')[:15]
    hdr[28:28 + min(19, len(name))] = name.encode('ascii', 'replace')[:19]
    hdr[48] = flags
    return code, name, is_eu, bytes(hdr) + dialogue + font


def self_test(rom_path):
    """Prove the walker against the C extractor's own reference data.

    Two independent checks on a US ROM, where aleks_lang.c's hardcoded offsets
    and this generic walker must agree:

      1. the message walk here vs. upstream's decode_strings_generic(), which
         reaches the same bytes by a completely different route;
      2. encode_dictionary() vs. the dictionary pointer table read out of the
         ROM the way extract_dictionary() in aleks_lang.c reads it.
    """
    rom = load_rom_any(rom_path)
    if rom.language != 'us':
        raise SystemExit('--self-test needs the US ROM; this one is %r'
                         % rom.language)
    info = tc.kLanguages['us']

    raw = extract_messages(rom.get_byte, info)
    built = build_messages(rom, 'us')
    if len(raw) != len(built):
        raise SystemExit('FAIL: %d walked vs %d rebuilt' % (len(raw), len(built)))
    bad = [i for i, (a, b) in enumerate(zip(raw, built)) if a != b]
    if bad:
        raise SystemExit('FAIL: message %d differs' % bad[0])
    print('PASS  %d messages: decode+re-encode reproduces the ROM exactly'
          % len(raw))

    # aleks_lang.c: DICT_PTRS 0x74703, DICT_PTR_BASE 0xC703, BANK_0E 0x70000
    raw = rom.ROM
    ptrs, base, bank = 0x74703, 0xC703, 0x70000
    first = raw[ptrs] | (raw[ptrs + 1] << 8)
    count = (first - base) // 2 - 1
    from_rom = []
    for i in range(count):
        s = raw[ptrs + i * 2] | (raw[ptrs + i * 2 + 1] << 8)
        e = raw[ptrs + i * 2 + 2] | (raw[ptrs + i * 2 + 3] << 8)
        from_rom.append(bytes(raw[bank + s - 0x8000: bank + e - 0x8000]))
    from_tables = [bytes(d) for d in tc.encode_dictionary('us')]
    if from_rom != from_tables:
        n = min(len(from_rom), len(from_tables))
        i = next((k for k in range(n) if from_rom[k] != from_tables[k]), n)
        raise SystemExit('FAIL: dictionary entry %d differs (%d vs %d entries)'
                         % (i, len(from_rom), len(from_tables)))
    print('PASS  %d dictionary entries match the ROM table' % len(from_rom))
    print('\nThe walker and the tables agree with the on-device extractor.')


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('rom', help='your own .sfc/.smc ROM')
    ap.add_argument('--lang', help='override the detected language code')
    ap.add_argument('--code', help='ini/config code to store (default: the language)')
    ap.add_argument('--name', help='name shown in SETTINGS (default: a sensible one)')
    ap.add_argument('-o', '--out', help='output path (default: <code>.z3lang)')
    ap.add_argument('--self-test', action='store_true',
                    help='verify against the US ROM instead of building')
    a = ap.parse_args()

    if a.self_test:
        self_test(a.rom)
        return

    code, name, is_eu, blob = build_pack(a.rom, a.code, a.name, a.lang)
    out = a.out or (code + '.z3lang')
    with open(out, 'wb') as f:
        f.write(blob)
    print('\nWrote %s (%d bytes)' % (out, len(blob)))
    print('  code    %s' % code)
    print('  shown as %s' % name)
    print('  text     %s' % ('EU encoding' if is_eu else 'US encoding'))
    print('\nCopy it to switch/Zelda3/languages/ and pick it in')
    print('SETTINGS -> SYSTEM -> LANGUAGE (the game restarts to apply it).')


if __name__ == '__main__':
    main()
