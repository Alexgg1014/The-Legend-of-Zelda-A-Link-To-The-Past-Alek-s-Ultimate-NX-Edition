/* aleks_update_manifest.c -- see the header.  Host-testable: no libnx. */
#include "aleks_update_manifest.h"

#include <ctype.h>
#include <string.h>

/* ---- semver ------------------------------------------------------------ */
static int ParseSemver(const char *p, uint32_t out[3]) {
  int part;
  if (!p) return 0;
  if (*p == 'v' || *p == 'V') ++p;
  for (part = 0; part < 3; ++part) {
    uint32_t value = 0; int digits = 0;
    while (isdigit((unsigned char)*p)) {
      if (value > 100000000u) return 0;
      value = value * 10u + (uint32_t)(*p++ - '0');
      digits++;
    }
    if (!digits) return 0;
    out[part] = value;
    if (part != 2 && *p++ != '.') return 0;
  }
  return *p == '\0' || *p == '-' || *p == '+';
}
/* Numeric: "0.10.0" beats "0.9.0", which a string compare gets wrong. A
 * malformed version on either side never claims an update. */
int AleksManifest_IsNewerVersion(const char *candidate, const char *installed) {
  uint32_t a[3], b[3]; int i;
  if (!ParseSemver(candidate, a) || !ParseSemver(installed, b)) return 0;
  for (i = 0; i < 3; ++i)
    if (a[i] != b[i]) return a[i] > b[i];
  return 0;
}

int AleksManifest_IsHttpsUrl(const char *url) {
  return url && strncmp(url, "https://", 8) == 0 && url[8] != '\0';
}

/* ---- manifest ---------------------------------------------------------- */
static const char *SkipWs(const char *p, const char *end) {
  while (p < end && isspace((unsigned char)*p)) ++p;
  return p;
}

/* Bounded reader for the closed manifest schema. Handles JSON string escapes,
 * rejects malformed or duplicated required keys, and never interprets a
 * remote value as a local path. */
static int JsonString(const char **inout, const char *end, char *out, size_t cap) {
  const char *p = SkipWs(*inout, end); size_t n = 0;
  if (p >= end || *p++ != '"' || cap == 0) return 0;
  while (p < end && *p != '"') {
    unsigned char c = (unsigned char)*p++;
    if (c == '\\') {
      if (p >= end) return 0;
      c = (unsigned char)*p++;
      if (c == 'u') return 0;  /* manifest strings are ASCII/UTF-8 literals */
      switch (c) {
        case '"': case '\\': case '/': break;
        case 'b': c = '\b'; break; case 'f': c = '\f'; break;
        case 'n': c = '\n'; break; case 'r': c = '\r'; break;
        case 't': c = '\t'; break; default: return 0;
      }
    }
    if (n + 1 >= cap || c < 0x20) return 0;
    out[n++] = (char)c;
  }
  if (p >= end || *p++ != '"') return 0;
  out[n] = 0;
  *inout = p;
  return 1;
}

static int JsonUInt(const char **inout, const char *end, uint64_t *out) {
  const char *p = SkipWs(*inout, end); uint64_t value = 0; int digits = 0;
  while (p < end && isdigit((unsigned char)*p)) {
    if (value > UINT64_MAX / 10u) return 0;
    value = value * 10u + (uint64_t)(*p++ - '0');
    digits = 1;
  }
  if (!digits) return 0;
  *out = value; *inout = p;
  return 1;
}

static int JsonChangelog(const char **inout, const char *end, AleksManifest *out) {
  const char *p = SkipWs(*inout, end);
  if (p >= end || *p++ != '[') return 0;
  p = SkipWs(p, end);
  while (p < end && *p != ']') {
    if (out->changelogCount >= Z3_UPDATE_CHANGELOG_MAX) {
      /* Skip it, do NOT reject the manifest -- see Manifest.changelog. */
      char discard[160];
      if (!JsonString(&p, end, discard, sizeof discard)) return 0;
    } else {
      if (!JsonString(&p, end, out->changelog[out->changelogCount],
                      sizeof out->changelog[0])) return 0;
      out->changelogCount++;
    }
    p = SkipWs(p, end);
    if (p < end && *p == ',') { p = SkipWs(p + 1, end); continue; }
    if (p >= end || *p != ']') return 0;
  }
  if (p >= end || *p++ != ']') return 0;
  *inout = p;
  return 1;
}

int AleksManifest_Parse(const char *json, size_t len, AleksManifest *out) {
  const char *p, *end; unsigned seen = 0;
  if (!json || !out || len == 0 || len > Z3_UPDATE_MANIFEST_MAX) return 0;
  memset(out, 0, sizeof *out);
  end = json + len;
  p = SkipWs(json, end);
  if (p >= end || *p++ != '{') return 0;
  p = SkipWs(p, end);
  while (p < end && *p != '}') {
    char key[32]; uint64_t number;
    if (!JsonString(&p, end, key, sizeof key)) return 0;
    p = SkipWs(p, end);
    if (p >= end || *p++ != ':') return 0;
    if (!strcmp(key, "schema")) {
      if ((seen & 1u) || !JsonUInt(&p, end, &number) || number > UINT32_MAX) return 0;
      out->schema = (uint32_t)number; seen |= 1u;
    } else if (!strcmp(key, "version")) {
      if ((seen & 2u) || !JsonString(&p, end, out->version, sizeof out->version)) return 0;
      seen |= 2u;
    } else if (!strcmp(key, "channel")) {
      if ((seen & 4u) || !JsonString(&p, end, out->channel, sizeof out->channel)) return 0;
      seen |= 4u;
    } else if (!strcmp(key, "nro_url")) {
      if ((seen & 8u) || !JsonString(&p, end, out->nroUrl, sizeof out->nroUrl)) return 0;
      seen |= 8u;
    } else if (!strcmp(key, "nro_sha256")) {
      if ((seen & 16u) || !JsonString(&p, end, out->nroSha256, sizeof out->nroSha256)) return 0;
      seen |= 16u;
    } else if (!strcmp(key, "nro_size")) {
      if ((seen & 32u) || !JsonUInt(&p, end, &out->nroSize)) return 0;
      seen |= 32u;
    } else if (!strcmp(key, "min_updater_schema")) {
      if ((seen & 64u) || !JsonUInt(&p, end, &number) || number > UINT32_MAX) return 0;
      out->minUpdaterSchema = (uint32_t)number; seen |= 64u;
    } else if (!strcmp(key, "changelog")) {
      if ((seen & 128u) || !JsonChangelog(&p, end, out)) return 0;
      seen |= 128u;
    } else {
      return 0;  /* closed schema: an unknown key is a different format */
    }
    p = SkipWs(p, end);
    if (p < end && *p == ',') { p = SkipWs(p + 1, end); continue; }
    if (p >= end || *p != '}') return 0;
  }
  if (p >= end || *p++ != '}' || SkipWs(p, end) != end) return 0;
  if (seen != 255u || out->schema != Z3_UPDATE_SCHEMA ||
      out->minUpdaterSchema > Z3_UPDATER_SCHEMA || strcmp(out->channel, "stable") ||
      !AleksManifest_IsHttpsUrl(out->nroUrl) || out->nroSize < 4096 || out->nroSize > Z3_UPDATE_NRO_MAX ||
      !ParseSemver(out->version, (uint32_t[3]){0, 0, 0}) || strlen(out->nroSha256) != 64)
    return 0;
  for (unsigned i = 0; i < 64; ++i) {
    if (!isxdigit((unsigned char)out->nroSha256[i])) return 0;
    out->nroSha256[i] = (char)tolower((unsigned char)out->nroSha256[i]);
  }
  return out->changelogCount != 0;
}
