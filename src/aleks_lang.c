/*
 * aleks_lang.c -- see aleks_lang.h.
 *
 * Ported from Zelda3-3DS-Reference/app/src/main/java/com/dishii/zelda3/
 * TranslationExtractor.java.  Every ROM offset, the message walk, the
 * dictionary bounds, the packed-array layout and the SHA-1 accept/reject lists
 * are the donor's; only the language is different and the result is kept out
 * of zelda3_assets.dat.
 */
#include "aleks_lang.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "config.h"
#include "assets.h"
#include "zelda_rtl.h"
#include "aleks_crashctx.h"

#define LANG_DIR "languages"
#define PACK_MAGIC "AZL2"

/* ---- ROM layout of the stock US text engine (donor constants) ---------- */
#define ROM_SIZE          1048576
#define TEXT_BANK1        0xE0000
#define TEXT_BANK2        0x75F40
#define DICT_PTRS         0x74703
#define DICT_PTR_BASE     0xC703
#define BANK_0E           0x70000
#define FONT_GFX          0x70000
#define FONT_GFX_SIZE     0x1000
#define FONT_WIDTHS       0x74ADF
#define FONT_WIDTHS_COUNT 99

/* Extra bytes after each command byte 0x67..0x7F (kText_CommandLengths_US). */
static const uint8 kCmdArgBytes[25] = {
  0,0,0,0,1,1,1,1,0,0,0,0,0,0,0,0,1,1,1,1,0,0,0,0,0,
};

/* Some hacks were dumped without message 4; the US bytes are spliced back so
 * the numbering the game hardcodes stays aligned (donor fixup). */
static const uint8 kUsMessage4[27] = {
  0x7a,0x00,0x34,0x40,0x59,0x6c,0x00,0x41,0x59,0x35,0x40,0x59,0x6c,0x01,
  0x75,0x36,0x40,0x59,0x6c,0x02,0x41,0x59,0x37,0x40,0x59,0x6c,0x03,
};

typedef struct { const char *sha1, *code, *display; } KnownRom;

/* The donor's list, unchanged.  A ROM that is not here still extracts if its
 * layout matches -- it is simply named "mod". */
static const KnownRom kKnownRoms[] = {
  /* Verified locally against the user's own copy: 1 MB unheadered, 397
   * messages, 97 dictionary words, width table 3..8 -- the stock US text
   * layout, so it lifts cleanly.  Not one of the donor's hashes; added here so
   * it names itself instead of showing up as a generic translation. */
  { "785851CECB27123D53777C55E4080C4FD5C23B2D", "es",    "ESPANOL" },
  { "461FCBD700D1332009C0E85A7A136E2A8E4B111E", "es",    "ESPANOL" },
  { "D455AB9E6B24B20D393AEBD17E7610A3FA21D653", "es",    "ESPANOL" },
  { "3C4D605EEFDA1D76F101965138F238476655B11D", "pl",    "POLSKI" },
  { "FA8ADFDBA2697C9A54D583A1284A22AC764C7637", "nl",    "NEDERLANDS" },
  { "43CD3438469B2C3FE879EA2F410B3EF3CB3F1CA4", "sv",    "SVENSKA" },
  { "B2A07A59E64C498BC1B2F28728F9BF4014C8D582", "redux", "ENGLISH REDUX" },
  { "9325C22EB0A2A1F0017157C8B620BC3A605CEDE1", "redux", "ENGLISH REDUX" },
};

/* Identifiable but NOT liftable with the US layout.  These rejections are the
 * donor's and are kept deliberately: forcing one through would produce
 * convincing garbage rather than an honest failure. */
static const KnownRom kUnsupportedRoms[] = {
  { "2E62494967FB0AFDF5DA1635607F9641DF7C6559", NULL, "the German PAL ROM" },
  { "229364A1B92A05167CD38609B1AA98F7041987CC", NULL, "the French PAL ROM" },
  { "C1C6C7F76FFF936C534FF11F87A54162FC0AA100", NULL, "the French-Canadian PAL ROM" },
  { "7C073A222569B9B8E8CA5FCB5DFEC3B5E31DA895", NULL, "the European PAL ROM" },
  { "D0D09ED41F9C373FE6AFDCCAFBF0DA8C88D3D90D", NULL, "the Portuguese translation" },
};

/* ---- SHA-1 (needed only to honour the donor's accept/reject lists) ------ */
typedef struct { uint32 h[5]; uint64 len; uint8 buf[64]; size_t n; } Sha1;

static uint32 rol32(uint32 v, int s) { return (v << s) | (v >> (32 - s)); }

static void sha1_block(Sha1 *c, const uint8 *p) {
  uint32 w[80], a, b, d, e, f, k, t;
  int i;
  for (i = 0; i < 16; i++)
    w[i] = ((uint32)p[i*4] << 24) | ((uint32)p[i*4+1] << 16) |
           ((uint32)p[i*4+2] << 8) | p[i*4+3];
  for (; i < 80; i++)
    w[i] = rol32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
  a = c->h[0]; b = c->h[1]; { uint32 cc = c->h[2]; d = c->h[3]; e = c->h[4];
  for (i = 0; i < 80; i++) {
    if (i < 20)      { f = (b & cc) | (~b & d);           k = 0x5A827999; }
    else if (i < 40) { f = b ^ cc ^ d;                    k = 0x6ED9EBA1; }
    else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
    else             { f = b ^ cc ^ d;                    k = 0xCA62C1D6; }
    t = rol32(a, 5) + f + e + k + w[i];
    e = d; d = cc; cc = rol32(b, 30); b = a; a = t;
  }
  c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; }
}

static void sha1_hex(const uint8 *data, size_t len, char out[41]) {
  Sha1 c;
  size_t i;
  uint8 tail[128];
  size_t tn = 0;
  uint64 bits = (uint64)len * 8;
  c.h[0]=0x67452301; c.h[1]=0xEFCDAB89; c.h[2]=0x98BADCFE;
  c.h[3]=0x10325476; c.h[4]=0xC3D2E1F0; c.len=0; c.n=0;
  for (i = 0; i + 64 <= len; i += 64)
    sha1_block(&c, data + i);
  memcpy(tail, data + i, len - i);
  tn = len - i;
  tail[tn++] = 0x80;
  while ((tn % 64) != 56) tail[tn++] = 0;
  for (int b = 7; b >= 0; b--) tail[tn++] = (uint8)(bits >> (b * 8));
  for (i = 0; i < tn; i += 64)
    sha1_block(&c, tail + i);
  for (i = 0; i < 5; i++)
    sprintf(out + i * 8, "%08X", c.h[i]);
  out[40] = 0;
}

/* ---- growable byte buffer ---------------------------------------------- */
typedef struct { uint8 *p; size_t n, cap; } Buf;

static bool buf_add(Buf *b, const void *data, size_t n) {
  if (b->n + n > b->cap) {
    size_t cap = b->cap ? b->cap * 2 : 4096;
    while (cap < b->n + n) cap *= 2;
    uint8 *np = (uint8 *)realloc(b->p, cap);
    if (!np) return false;
    b->p = np; b->cap = cap;
  }
  memcpy(b->p + b->n, data, n);
  b->n += n;
  return true;
}
static void buf_free(Buf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

/* An array of blobs, for pack/unpack. */
typedef struct { Buf *e; int n, cap; } Arr;

static bool arr_add(Arr *a, const void *p, size_t n) {
  if (a->n == a->cap) {
    int cap = a->cap ? a->cap * 2 : 64;
    Buf *ne = (Buf *)realloc(a->e, sizeof(Buf) * cap);
    if (!ne) return false;
    a->e = ne; a->cap = cap;
  }
  memset(&a->e[a->n], 0, sizeof(Buf));
  if (n && !buf_add(&a->e[a->n], p, n)) return false;
  a->n++;
  return true;
}
static void arr_free(Arr *a) {
  for (int i = 0; i < a->n; i++) buf_free(&a->e[i]);
  free(a->e);
  a->e = NULL; a->n = a->cap = 0;
}

static uint16 rd16(const uint8 *p) { return (uint16)(p[0] | (p[1] << 8)); }
static uint32 rd32(const uint8 *p) {
  return (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) | ((uint32)p[3] << 24);
}
static void wr16(uint8 *p, uint16 v) { p[0] = (uint8)v; p[1] = (uint8)(v >> 8); }
static void wr32(uint8 *p, uint32 v) {
  p[0]=(uint8)v; p[1]=(uint8)(v>>8); p[2]=(uint8)(v>>16); p[3]=(uint8)(v>>24);
}

/*
 * Packed array, the layout FindIndexInMemblk reads (util.c): n-1 offsets, the
 * entries back to back, then a 16-bit trailer holding n-1 (plus 8192 when the
 * offsets had to widen to 32-bit).
 */
static bool pack_arr(const Arr *a, Buf *out) {
  size_t data = 0, cum = 0, i;
  bool wide;
  int width;
  uint8 *o;
  size_t total;

  if (a->n <= 0) return false;
  for (i = 0; i < (size_t)a->n; i++) data += a->e[i].n;
  wide = (data - a->e[a->n - 1].n) >= 65536 || a->n > 8192;
  width = wide ? 4 : 2;
  total = (size_t)(a->n - 1) * width + data + 2;
  o = (uint8 *)calloc(1, total);
  if (!o) return false;
  for (i = 0; i < (size_t)a->n; i++) {
    if (i != 0) {
      if (wide) wr32(o + (i - 1) * 4, (uint32)cum);
      else      wr16(o + (i - 1) * 2, (uint16)cum);
    }
    if (a->e[i].n)
      memcpy(o + (size_t)(a->n - 1) * width + cum, a->e[i].p, a->e[i].n);
    cum += a->e[i].n;
  }
  wr16(o + total - 2, (uint16)((a->n - 1) + (wide ? 8192 : 0)));
  out->p = o; out->n = total; out->cap = total;
  return true;
}

static bool unpack_arr(const uint8 *blk, size_t len, Arr *out) {
  size_t end, base, prev = 0;
  int mx, width = 2;
  memset(out, 0, sizeof *out);
  if (len < 2) return false;
  end = len - 2;
  mx = rd16(blk + end);
  if (mx >= 8192) { mx -= 8192; width = 4; }
  base = (size_t)mx * width;
  if (base > end) return false;
  for (int i = 0; i <= mx; i++) {
    size_t next = (i == mx) ? end - base
                : (width == 2 ? rd16(blk + i * 2) : rd32(blk + i * 4));
    if (next < prev || base + next > end) { arr_free(out); return false; }
    if (!arr_add(out, blk + base + prev, next - prev)) { arr_free(out); return false; }
    prev = next;
  }
  return true;
}

/* ---- extraction (donor logic) ------------------------------------------ */
static bool extract_messages(const uint8 *rom, Arr *out) {
  Buf cur = {0};
  int p = TEXT_BANK1, bank_switches = 0;
  memset(out, 0, sizeof *out);
  for (long guard = 0; ; guard++) {
    if (guard > 0x20000 || p >= ROM_SIZE || out->n > 1000) goto fail;
    {
      int c = rom[p];
      int len, i;
      if (c == 0xFF) break;
      if (c == 0x80) {
        if (++bank_switches > 1) goto fail;
        p = TEXT_BANK2;
        continue;
      }
      len = (c >= 0x67 && c < 0x80) ? 1 + kCmdArgBytes[c - 0x67] : 1;
      if (p + len > ROM_SIZE) goto fail;
      for (i = 0; i < len; i++)
        if (!buf_add(&cur, rom + p + i, 1)) goto fail;
      p += len;
      if (c == 0x7F) {
        cur.n--;                       /* the terminator is not stored */
        if (!arr_add(out, cur.p, cur.n)) goto fail;
        cur.n = 0;
      }
    }
  }
  buf_free(&cur);
  if (out->n < 300) { arr_free(out); return false; }
  return true;
fail:
  buf_free(&cur);
  arr_free(out);
  return false;
}

static bool extract_dictionary(const uint8 *rom, Arr *out) {
  int first = rd16(rom + DICT_PTRS);
  int gap = first - DICT_PTR_BASE;
  int count, i;
  memset(out, 0, sizeof *out);
  if (gap <= 0 || (gap & 1) || gap / 2 < 50 || gap / 2 > 200) return false;
  count = gap / 2 - 1;                 /* the last pointer only marks the end */
  for (i = 0; i < count; i++) {
    int start = rd16(rom + DICT_PTRS + i * 2);
    int end   = rd16(rom + DICT_PTRS + i * 2 + 2);
    if (start < 0x8000 || end < start || end - start > 256 ||
        BANK_0E + end - 0x8000 > ROM_SIZE) { arr_free(out); return false; }
    if (!arr_add(out, rom + BANK_0E + start - 0x8000, (size_t)(end - start))) {
      arr_free(out); return false;
    }
  }
  return true;
}

/* dialogueBlk = pack([pack(dictionary), pack(messages)]) */
static bool build_blobs(const uint8 *rom, Buf *dialogue, Buf *font) {
  Arr msgs, dict, two;
  Buf pmsgs = {0}, pdict = {0};
  bool ok = false;

  if (!extract_messages(rom, &msgs)) return false;
  if (msgs.n == 396) {
    /* Insert the missing message 4 by rebuilding: cheap, happens once. */
    Arr fixed;
    memset(&fixed, 0, sizeof fixed);
    for (int i = 0; i < msgs.n; i++) {
      if (i == 4 && !arr_add(&fixed, kUsMessage4, sizeof kUsMessage4)) goto done_msgs;
      if (!arr_add(&fixed, msgs.e[i].p, msgs.e[i].n)) goto done_msgs;
    }
    arr_free(&msgs);
    msgs = fixed;
  }
  if (!extract_dictionary(rom, &dict)) goto done_msgs;

  if (!pack_arr(&dict, &pdict) || !pack_arr(&msgs, &pmsgs)) goto done_dict;
  memset(&two, 0, sizeof two);
  if (!arr_add(&two, pdict.p, pdict.n) || !arr_add(&two, pmsgs.p, pmsgs.n)) {
    arr_free(&two); goto done_dict;
  }
  if (!pack_arr(&two, dialogue)) { arr_free(&two); goto done_dict; }
  arr_free(&two);

  memset(&two, 0, sizeof two);
  if (!arr_add(&two, rom + FONT_GFX, FONT_GFX_SIZE) ||
      !arr_add(&two, rom + FONT_WIDTHS, FONT_WIDTHS_COUNT)) {
    arr_free(&two); buf_free(dialogue); goto done_dict;
  }
  ok = pack_arr(&two, font);
  arr_free(&two);
  if (!ok) buf_free(dialogue);

done_dict:
  buf_free(&pdict); buf_free(&pmsgs); arr_free(&dict);
done_msgs:
  arr_free(&msgs);
  return ok;
}

/* ---- registration into the live asset arrays --------------------------- */
typedef struct { char code[16]; char display[24]; } LangEntry;

#define MAX_LANGS 8
static LangEntry g_langs[MAX_LANGS];
static int g_lang_count;        /* extracted ones; the UI adds English at 0 */

/* Blobs handed to the asset arrays.  Never freed: they are live asset data
 * for the rest of the process, exactly like the mapped .dat. */
static bool register_language(const char *code, const char *display,
                              const Buf *dialogue, const Buf *font) {
  Arr dlg, fnt, map, two;
  Buf pdlg = {0}, pfnt = {0}, pmap = {0}, conf_blk = {0};
  uint8 conf[3];
  bool ok = false;
  int idx;

  if (g_lang_count >= MAX_LANGS) return false;
  if (!g_asset_ptrs[94] || !g_asset_ptrs[95] || !g_asset_ptrs[96]) return false;

  if (!unpack_arr(g_asset_ptrs[94], g_asset_sizes[94], &dlg)) return false;
  if (!unpack_arr(g_asset_ptrs[95], g_asset_sizes[95], &fnt)) { arr_free(&dlg); return false; }
  if (!unpack_arr(g_asset_ptrs[96], g_asset_sizes[96], &map)) {
    arr_free(&dlg); arr_free(&fnt); return false;
  }

  idx = dlg.n;                      /* the new language's index in both arrays */
  conf[0] = (uint8)idx; conf[1] = (uint8)idx;
  /* flag 2 = "text no longer matches the US ROM", which is what disables the
   * emulator-compare path.  Flag 1 (PAL command encoding) is never emitted. */
  conf[2] = 2;

  if (!arr_add(&dlg, dialogue->p, dialogue->n)) goto done;
  if (!arr_add(&fnt, font->p, font->n)) goto done;

  memset(&two, 0, sizeof two);
  if (!arr_add(&two, code, strlen(code)) || !arr_add(&two, conf, 3)) {
    arr_free(&two); goto done;
  }
  if (!pack_arr(&two, &conf_blk)) { arr_free(&two); goto done; }
  arr_free(&two);
  if (!arr_add(&map, conf_blk.p, conf_blk.n)) goto done;

  if (!pack_arr(&dlg, &pdlg) || !pack_arr(&fnt, &pfnt) || !pack_arr(&map, &pmap))
    goto done;

  /* Repoint.  The base .dat is untouched on disk and in memory; only these
   * three array pointers now refer to the rebuilt copies. */
  g_asset_ptrs[94] = pdlg.p; g_asset_sizes[94] = (uint32)pdlg.n;
  g_asset_ptrs[95] = pfnt.p; g_asset_sizes[95] = (uint32)pfnt.n;
  g_asset_ptrs[96] = pmap.p; g_asset_sizes[96] = (uint32)pmap.n;
  pdlg.p = pfnt.p = pmap.p = NULL;   /* ownership moved to the asset table */

  snprintf(g_langs[g_lang_count].code, sizeof g_langs[0].code, "%s", code);
  snprintf(g_langs[g_lang_count].display, sizeof g_langs[0].display, "%s", display);
  g_lang_count++;
  ok = true;

done:
  buf_free(&conf_blk); buf_free(&pdlg); buf_free(&pfnt); buf_free(&pmap);
  arr_free(&dlg); arr_free(&fnt); arr_free(&map);
  return ok;
}

/* ---- cached pack ------------------------------------------------------- */
/* Layout: "AZL1", u32 dialogue size, u32 font size, then the two blobs.  Small
 * (~40 KB) and derived entirely from the user's own ROM. */
static bool pack_write(const char *key, const char *code, const char *display,
                       const Buf *dialogue, const Buf *font) {
  char tmp[160], dst[160];
  FILE *f;
  uint8 hdr[52];
  bool ok;
  snprintf(tmp, sizeof tmp, LANG_DIR "/%s.z3lang.tmp", key);
  snprintf(dst, sizeof dst, LANG_DIR "/%s.z3lang", key);
  f = fopen(tmp, "wb");
  if (!f) return false;
  memset(hdr, 0, sizeof hdr);
  memcpy(hdr, PACK_MAGIC, 4);
  wr32(hdr + 4, (uint32)dialogue->n);
  wr32(hdr + 8, (uint32)font->n);
  snprintf((char *)hdr + 12, 16, "%s", code);
  snprintf((char *)hdr + 28, 24, "%s", display);
  ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr &&
       fwrite(dialogue->p, 1, dialogue->n, f) == dialogue->n &&
       fwrite(font->p, 1, font->n, f) == font->n;
  if (fflush(f) != 0) ok = false;
  if (fclose(f) != 0) ok = false;
  /* Only replace a good pack with another good pack. */
  if (ok) { remove(dst); ok = rename(tmp, dst) == 0; }
  if (!ok) remove(tmp);
  return ok;
}

static bool pack_read(const char *key, char *code, size_t code_cap,
                      char *display, size_t disp_cap,
                      Buf *dialogue, Buf *font) {
  char path[160];
  FILE *f;
  uint8 hdr[52];
  uint32 dn, fn;
  bool ok = false;
  snprintf(path, sizeof path, LANG_DIR "/%s.z3lang", key);
  f = fopen(path, "rb");
  if (!f) return false;
  if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr ||
      memcmp(hdr, PACK_MAGIC, 4) != 0) goto done;
  dn = rd32(hdr + 4); fn = rd32(hdr + 8);
  if (dn == 0 || fn == 0 || dn > (8u << 20) || fn > (1u << 20)) goto done;
  hdr[27] = 0; hdr[51] = 0;                 /* the strings are fixed fields */
  snprintf(code, code_cap, "%s", (const char *)hdr + 12);
  snprintf(display, disp_cap, "%s", (const char *)hdr + 28);
  if (!code[0]) goto done;
  dialogue->p = (uint8 *)malloc(dn); font->p = (uint8 *)malloc(fn);
  if (!dialogue->p || !font->p) goto done;
  if (fread(dialogue->p, 1, dn, f) != dn || fread(font->p, 1, fn, f) != fn) goto done;
  dialogue->n = dialogue->cap = dn;
  font->n = font->cap = fn;
  ok = true;
done:
  fclose(f);
  if (!ok) { buf_free(dialogue); buf_free(font); }
  return ok;
}

/* ---- one language ROM -------------------------------------------------- */

/* A safe cache/config key from the filename: "fr-hack.sfc" -> "fr-hack". */
static void key_from_filename(const char *filename, char *out, size_t cap) {
  size_t n = 0;
  for (const char *p = filename; *p && *p != '.' && n + 1 < cap; p++) {
    char c = *p;
    bool okc = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_';
    out[n++] = okc ? c : '_';
  }
  out[n] = 0;
  if (!out[0]) snprintf(out, cap, "lang");
}

static void upper_copy(const char *in, char *out, size_t cap) {
  size_t n = 0;
  for (; in[n] && n + 1 < cap; n++)
    out[n] = (in[n] >= 'a' && in[n] <= 'z') ? (char)(in[n] - 'a' + 'A') : in[n];
  out[n] = 0;
}

static void try_rom(const char *filename) {
  char path[192], sha[41], key[16], code[16], display[24];
  FILE *f;
  long size;
  uint8 *rom = NULL;
  size_t off = 0;
  Buf dialogue = {0}, font = {0};

  key_from_filename(filename, key, sizeof key);

  /* A cached pack means this file has already been read once; it also carries
   * its own name, so nothing has to be re-derived from the ROM. */
  if (pack_read(key, code, sizeof code, display, sizeof display, &dialogue, &font)) {
    if (register_language(code, display, &dialogue, &font))
      StartupLog("LANGUAGE PACK: %s loaded from cache", code);
    buf_free(&dialogue); buf_free(&font);
    return;
  }

  snprintf(path, sizeof path, LANG_DIR "/%s", filename);
  f = fopen(path, "rb");
  if (!f) return;
  fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
  /* Unheadered 1 MB, or the same with a 512-byte copier header. */
  if (size != ROM_SIZE && size != ROM_SIZE + 512) {
    fclose(f);
    StartupLog("LANGUAGE SCAN: %s skipped (size %ld)", filename, size);
    return;
  }
  rom = (uint8 *)malloc((size_t)size);
  if (!rom || fread(rom, 1, (size_t)size, f) != (size_t)size) {
    fclose(f); free(rom); return;
  }
  fclose(f);
  if (size == ROM_SIZE + 512) off = 512;    /* strip the header */

  /* Unknown ROMs are named after their file, so two of them cannot collide
   * and the ini value stays something the user can recognise. */
  snprintf(code, sizeof code, "%s", key);
  upper_copy(key, display, sizeof display);

  sha1_hex(rom + off, ROM_SIZE, sha);
  for (size_t i = 0; i < sizeof kUnsupportedRoms / sizeof kUnsupportedRoms[0]; i++) {
    if (strcmp(sha, kUnsupportedRoms[i].sha1) == 0) {
      StartupLog("LANGUAGE EXTRACT: unsupported ROM (%s)", kUnsupportedRoms[i].display);
      free(rom);
      return;
    }
  }
  for (size_t i = 0; i < sizeof kKnownRoms / sizeof kKnownRoms[0]; i++) {
    if (strcmp(sha, kKnownRoms[i].sha1) == 0) {
      snprintf(code, sizeof code, "%s", kKnownRoms[i].code);
      snprintf(display, sizeof display, "%s", kKnownRoms[i].display);
      break;
    }
  }
  StartupLog("LANGUAGE SCAN: %s -> %s", filename, display);

  if (!build_blobs(rom + off, &dialogue, &font)) {
    StartupLog("LANGUAGE EXTRACT: no usable text in %s", filename);
    free(rom);
    return;
  }
  free(rom);                                 /* never kept resident */

  if (register_language(code, display, &dialogue, &font)) {
    pack_write(key, code, display, &dialogue, &font);   /* best effort cache */
    StartupLog("LANGUAGE EXTRACT: success (%s)", code);
  } else {
    StartupLog("LANGUAGE EXTRACT: could not register %s", code);
  }
  buf_free(&dialogue);
  buf_free(&font);
}

void AleksLang_Init(void) {
  DIR *dir;
  struct dirent *ent;

  /*
   * SCAN THE DIRECTORY, don't guess filenames.
   *
   * Any hack of the US ROM that keeps the stock text engine works -- the font
   * and width table come out of the ROM itself, so no per-language table is
   * needed here.  Looking for a fixed list of names would have silently
   * ignored every translation nobody thought to hardcode, which is most of
   * them.  A ROM whose layout does not match still fails structurally and is
   * skipped, so scanning widely costs nothing but a rejected file.
   */
  dir = opendir(LANG_DIR);
  if (!dir) return;                      /* no languages/ at all: English only */
  while ((ent = readdir(dir)) != NULL) {
    const char *dot = strrchr(ent->d_name, '.');
    if (!dot) continue;
    if (strcasecmp(dot, ".sfc") != 0 && strcasecmp(dot, ".smc") != 0) continue;
    if (g_lang_count >= MAX_LANGS) break;
    try_rom(ent->d_name);
  }
  closedir(dir);

  if (g_lang_count)
    StartupLog("LANGUAGE: %d extra language(s) available", g_lang_count);
}

int AleksLang_Count(void) { return g_lang_count + 1; }

const char *AleksLang_CodeAt(int i) {
  if (i <= 0 || i > g_lang_count) return NULL;      /* 0 = built-in English */
  return g_langs[i - 1].code;
}

const char *AleksLang_DisplayAt(int i) {
  if (i <= 0 || i > g_lang_count) return "ENGLISH";
  return g_langs[i - 1].display;
}

int AleksLang_CurrentIndex(void) {
  if (!g_config.language || !g_config.language[0]) return 0;
  for (int i = 0; i < g_lang_count; i++)
    if (strcmp(g_langs[i].code, g_config.language) == 0) return i + 1;
  /* Configured but not available: the caller falls back to English. */
  StartupLog("LANGUAGE: requested '%s' but no pack is available; using English",
             g_config.language);
  return 0;
}
