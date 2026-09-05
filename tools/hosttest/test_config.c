/*
 * test_config.c -- run the real config parser on the host.
 *
 * WHY: public issue #6 was "my display settings reset every launch", and it was
 * reproducible from a config file alone -- no console, no controller, no play
 * session.  It still took a user report to find, because nothing could execute
 * config.c outside a Switch.  This fixes that: it builds the actual
 * src/config.c and feeds it real ini files.
 *
 * ONE SCENARIO PER PROCESS.  ParseConfigFile() is startup code and keeps global
 * state -- the key map hash grows on every call and is never reset -- so
 * calling it repeatedly in one process is not something the engine supports,
 * and a harness that did it hung.  run.sh therefore runs each scenario in its
 * own process, which is also what makes a failure isolatable.
 *
 * Build and run with tools/hosttest/run.sh.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

static int g_fail;

#define CHECK(cond, ...)                                                        do {                                                                            if (!(cond)) {                                                                  printf("  FAIL  " __VA_ARGS__);                                               printf("\n        (%s, line %d)\n", #cond, __LINE__);                         g_fail++;                                                                   }                                                                           } while (0)

static void parse(const char *text) {
  FILE *f = fopen("zelda3.ini", "wb");
  if (!f) { printf("cannot write zelda3.ini\n"); exit(2); }
  fwrite(text, 1, strlen(text), f);
  fclose(f);
  ParseConfigFile("zelda3.ini");
}

/* Everything the ALEKS aspect touches must agree after a parse: the renderer
 * reads extended_aspect_ratio and extend_y while the UI reads the enum.  Them
 * disagreeing IS the bug. */
static void expect_aspect(const char *what, int aspect) {
  CHECK(g_config.aleks_gameplay_aspect == aspect,
        "%s: aspect is %d, expected %d", what,
        g_config.aleks_gameplay_aspect, aspect);
  CHECK(g_config.extended_aspect_ratio == AleksAspect_ExtraSide(aspect),
        "%s: extra_side is %d, expected %d", what,
        g_config.extended_aspect_ratio, AleksAspect_ExtraSide(aspect));
  CHECK(g_config.extend_y == AleksAspect_Height240(aspect),
        "%s: extend_y is %d, expected %d", what,
        (int)g_config.extend_y, (int)AleksAspect_Height240(aspect));
}

int main(int argc, char **argv) {
  int n = argc > 1 ? atoi(argv[1]) : 0;
  char buf[160];
  setbuf(stdout, NULL);

  switch (n) {
  case 0:
    /* THE REGRESSION, from the reporter's own file: the ALEKS key says True
     * 16:9 Expanded and upstream's key -- written last by the generated config
     * -- says 4:3.  The last one used to win, so every launch reverted it. */
    printf("stale ExtendedAspectRatio must not override the ALEKS aspect\n");
    parse("[General]\nAleksGameplayAspect = True16:9Expanded\n"
          "AleksWideCamera = Fixed\nAutosave = 0\nExtendedAspectRatio = 4:3\n");
    expect_aspect("expanded-vs-stale-4:3", kAleksAspect_True169Expanded);
    break;

  case 1:
    printf("order must not matter when the ALEKS key comes last\n");
    parse("[General]\nExtendedAspectRatio = 4:3\n"
          "AleksGameplayAspect = True16:9\n");
    expect_aspect("aleks-key-last", kAleksAspect_True169);
    break;

  case 2:
    /* A config with only upstream's key belongs to somebody else: leave the
     * value exactly as written and only derive the enum for the UI. */
    printf("an upstream-only config is not rewritten\n");
    parse("[General]\nExtendedAspectRatio = 16:9\n");
    CHECK(g_config.extended_aspect_ratio == (224 * 16 / 9 - 256) / 2,
          "upstream 16:9 became %d", g_config.extended_aspect_ratio);
    CHECK(g_config.aleks_gameplay_aspect ==
              AleksAspect_FromExtraSide(g_config.extended_aspect_ratio),
          "enum was not derived from the upstream key");
    break;

  case 3:
    printf("square pixels persist\n");
    parse("[General]\nAleksSquarePixels = true\n");
    CHECK(g_config.aleks_square_pixels, "AleksSquarePixels = true did not stick");
    break;

  case 4:
    printf("defaults: 4:3, square pixels off\n");
    parse("[General]\n");
    CHECK(!g_config.aleks_square_pixels, "square pixels must default to off");
    expect_aspect("defaults", kAleksAspect_4x3);
    break;

  default:
    /* Every aspect must survive a round trip through its own ini string. */
    if (n - 5 >= kAleksAspect_Count) { printf("no scenario %d\n", n); return 2; }
    {
      int a = n - 5;
      printf("round trip: %s\n", AleksAspect_Label(a));
      snprintf(buf, sizeof buf, "[General]\nAleksGameplayAspect = %s\n",
               AleksAspect_IniValue(a));
      parse(buf);
      expect_aspect(AleksAspect_Label(a), a);
    }
    break;
  }

  remove("zelda3.ini");
  return g_fail ? 1 : 0;
}
