/* Host-side check for update_manifest.json and the parser behind it.
 *
 *   cc -I src/platform/switch tools/test_update_manifest.c \
 *      src/platform/switch/aleks_update_manifest.c -o test_update_manifest
 *   ./test_update_manifest update_manifest.json [installed-version]
 *
 * Exit 0 = the console WILL accept this manifest.  Run it before every push
 * of update_manifest.json: the updater is the one thing that cannot be fixed
 * by an update once it is broken. */
#include "aleks_update_manifest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int expect(int cond, const char *what) {
  printf("%s  %s\n", cond ? "ok  " : "FAIL", what);
  return cond ? 0 : 1;
}

static int self_test(void) {
  AleksManifest m;
  int fails = 0;
  const char *good =
    "{\"schema\":1,\"version\":\"1.2.0\",\"channel\":\"stable\","
    "\"nro_url\":\"https://x/y.nro\",\"nro_sha256\":\""
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789ABCDEF\","
    "\"nro_size\":8834342,\"min_updater_schema\":1,"
    "\"changelog\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\",\"j\"]}";
  fails += expect(AleksManifest_Parse(good, strlen(good), &m), "parses a full manifest");
  fails += expect(m.changelogCount == Z3_UPDATE_CHANGELOG_MAX,
                  "changelog over the cap is TRUNCATED, not rejected (TMC v1.3.2 scar)");
  fails += expect(m.nroSha256[63] == 'f', "sha256 is lower-cased");

  const char *http = "{\"schema\":1,\"version\":\"1.2.0\",\"channel\":\"stable\","
    "\"nro_url\":\"http://x/y.nro\",\"nro_sha256\":\""
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
    "\"nro_size\":8834342,\"min_updater_schema\":1,\"changelog\":[\"a\"]}";
  fails += expect(!AleksManifest_Parse(http, strlen(http), &m), "rejects a non-https URL");

  const char *beta = "{\"schema\":1,\"version\":\"1.2.0\",\"channel\":\"beta\","
    "\"nro_url\":\"https://x/y.nro\",\"nro_sha256\":\""
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
    "\"nro_size\":8834342,\"min_updater_schema\":1,\"changelog\":[\"a\"]}";
  fails += expect(!AleksManifest_Parse(beta, strlen(beta), &m), "rejects a non-stable channel");

  const char *future = "{\"schema\":1,\"version\":\"1.2.0\",\"channel\":\"stable\","
    "\"nro_url\":\"https://x/y.nro\",\"nro_sha256\":\""
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
    "\"nro_size\":8834342,\"min_updater_schema\":2,\"changelog\":[\"a\"]}";
  fails += expect(!AleksManifest_Parse(future, strlen(future), &m),
                  "rejects min_updater_schema newer than this updater");

  const char *extra = "{\"schema\":1,\"version\":\"1.2.0\",\"channel\":\"stable\","
    "\"nro_url\":\"https://x/y.nro\",\"nro_sha256\":\""
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
    "\"nro_size\":8834342,\"min_updater_schema\":1,\"changelog\":[\"a\"],\"notes\":\"x\"}";
  fails += expect(!AleksManifest_Parse(extra, strlen(extra), &m),
                  "rejects an unknown key (closed schema -- do not add keys casually)");

  fails += expect(AleksManifest_IsNewerVersion("1.10.0", "1.9.0"), "1.10.0 > 1.9.0 (numeric)");
  fails += expect(!AleksManifest_IsNewerVersion("1.1.1", "1.1.1"), "same version is not newer");
  fails += expect(AleksManifest_IsNewerVersion("v1.2.0", "1.1.1"), "leading v is tolerated");
  fails += expect(!AleksManifest_IsNewerVersion("1.2", "1.1.1"), "two-part version never claims an update");
  return fails;
}

int main(int argc, char **argv) {
  int fails = self_test();
  if (argc >= 2) {
    FILE *f = fopen(argv[1], "rb");
    static char buf[Z3_UPDATE_MANIFEST_MAX + 1];
    size_t n;
    AleksManifest m;
    if (!f) { perror(argv[1]); return 2; }
    n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    fails += expect(n <= Z3_UPDATE_MANIFEST_MAX, "file fits the 16 KB cap");
    if (AleksManifest_Parse(buf, n, &m)) {
      printf("ok    %s: version %s, %llu bytes, %u changelog line(s)\n", argv[1], m.version,
             (unsigned long long)m.nroSize, m.changelogCount);
      printf("      %s\n", m.nroUrl);
      if (argc >= 3)
        printf("%s  installed %s -> %s\n",
               AleksManifest_IsNewerVersion(m.version, argv[2]) ? "ok  " : "note",
               argv[2], AleksManifest_IsNewerVersion(m.version, argv[2]) ?
                 "console will offer this update" : "console will say UP TO DATE");
    } else {
      fails += expect(0, "REJECTED -- the console would show 'Update manifest rejected'");
    }
  }
  printf("%s\n", fails ? "FAILED" : "ALL OK");
  return fails ? 1 : 0;
}
