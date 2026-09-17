/* aleks_update_manifest.h -- the pure half of the updater: semver compare
 * and the closed-schema manifest parser.  No libnx, no curl, no filesystem,
 * so it compiles on the host and tools/test_update_manifest.c can prove a
 * manifest is accepted BEFORE it is pushed.  The TMC updater once bricked
 * itself on a manifest its own parser rejected; this file exists so that
 * cannot happen silently again. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define Z3_UPDATE_SCHEMA        1u
#define Z3_UPDATER_SCHEMA       1u
#define Z3_UPDATE_MANIFEST_MAX  (16u * 1024u)
#define Z3_UPDATE_CHANGELOG_MAX 8u
#define Z3_UPDATE_NRO_MAX       (128u * 1024u * 1024u)

typedef struct {
  uint32_t schema;
  uint32_t minUpdaterSchema;
  uint64_t nroSize;
  char version[32];
  char channel[12];
  char nroUrl[512];
  char nroSha256[65];
  /* TMC scar: a manifest with more lines than the cap used to be REJECTED
   * outright, so the release that shipped a fourth changelog line bricked
   * every install's updater -- and nobody could be sent a fix, because the
   * thing that delivers fixes was the thing that was broken (TMC v1.3.2 ->
   * v1.3.3). The parser now stops STORING past the cap instead of failing.
   * A manifest is release notes; it must never be able to brick the updater. */
  char changelog[Z3_UPDATE_CHANGELOG_MAX][160];
  unsigned changelogCount;
} AleksManifest;

/* 1 when the JSON is a valid stable manifest for this updater schema. */
int AleksManifest_Parse(const char *json, size_t len, AleksManifest *out);
/* Numeric semver compare; a malformed version on either side is never newer. */
int AleksManifest_IsNewerVersion(const char *candidate, const char *installed);
int AleksManifest_IsHttpsUrl(const char *url);
