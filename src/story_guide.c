/*
 * story_guide.c -- ARCHITECTURE ONLY (see story_guide.h).
 *
 * This pass deliberately ships NO progression rules and NO guidance content.
 * What it ships is the seam: a key-based entry, a string table that a
 * translation can replace wholesale, and a rule table with exactly one entry
 * -- the fallback -- so the future content pass adds rows above it and
 * changes nothing else.
 *
 * The only strings here are the placeholder and the UI headings.  They are in
 * the table rather than inline for the same reason everything else will be:
 * so the localisation pass has one place to look.
 */

#include "story_guide.h"

#include <string.h>

#include "config.h"

/* ---- string table ------------------------------------------------------ *
 * Keys are stable identifiers; the text is what a language file replaces.
 * Deliberately tiny for now -- this is the mechanism, not the content.
 */
typedef struct GuideString {
  const char *key;
  const char *en;
} GuideString;

static const GuideString kGuideStrings[] = {
  { "guide.heading.placeholder", "STORY GUIDE" },
  { "guide.body.placeholder",    "COMING IN A FUTURE UPDATE" },
  { "guide.body.disabled",       "GUIDE IS TURNED OFF" },
};

const char *StoryGuide_Text(const char *key) {
  if (!key)
    return "";
  for (size_t i = 0; i < sizeof(kGuideStrings) / sizeof(kGuideStrings[0]); i++) {
    if (strcmp(kGuideStrings[i].key, key) == 0)
      return kGuideStrings[i].en;
  }
  /* Unknown key: show the key so a missing string is obvious rather than a
   * blank line that looks like a layout bug. */
  return key;
}

/* ---- rules ------------------------------------------------------------- *
 * The future table goes here: an ordered array of {predicate, keys},
 * first match wins, evaluated against live progression state (module,
 * pendants, crystals, dungeon completion, key items).  Today there is one
 * entry and it is the fallback, which is also what guarantees
 * StoryGuide_GetCurrentEntry can never fail or return empty keys.
 */
void StoryGuide_GetCurrentEntry(StoryGuideEntry *out) {
  if (!out)
    return;
  memset(out, 0, sizeof(*out));
  out->heading_key = "guide.heading.placeholder";
  out->objective_key = StoryGuide_IsEnabled() ? "guide.body.placeholder"
                                              : "guide.body.disabled";
  /* hint_key / detail_key stay NULL: no rule has produced them yet, and the
   * renderer simply lays out fewer blocks. */
}

StoryGuideDetail StoryGuide_Detail(void) {
  uint8 v = g_config.aleks_story_guide;
  return v <= kStoryGuideDetailed ? (StoryGuideDetail)v : kStoryGuideObjectives;
}

void StoryGuide_SetDetail(StoryGuideDetail detail) {
  g_config.aleks_story_guide = (uint8)detail;
}

bool StoryGuide_IsEnabled(void) {
  return StoryGuide_Detail() != kStoryGuideOff;
}
