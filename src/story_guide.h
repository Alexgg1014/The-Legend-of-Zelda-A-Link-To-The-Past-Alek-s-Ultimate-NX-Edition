#ifndef ALEKS_STORY_GUIDE_H_
#define ALEKS_STORY_GUIDE_H_

/*
 * story_guide.h -- ARCHITECTURE ONLY.
 *
 * The Story Guide will eventually tell the player what to do next based on
 * real Zelda3 progression state.  None of that is written yet, and this pass
 * deliberately does not invent any of it: no progression rules, no hints, no
 * prose.
 *
 * What exists here is the shape the future content pass fills in, chosen so
 * that pass never has to touch the renderer:
 *
 *   - the guide returns STRING KEYS, never text.  StoryGuide_Text() resolves
 *     a key through the string table, which is the single place a translation
 *     will plug into (see MULTILANGUAGE-SWITCH-PLAN.md).
 *   - the renderer (draw_guide in second_screen_sdl.c) knows only the four
 *     key slots below, so adding rules changes no drawing code.
 *   - the detail level is a setting, so OBJECTIVES / HINTS / DETAILED select
 *     which keys a rule is allowed to fill in rather than needing three
 *     different renderers.
 *
 * Rules will be a flat, ordered table of {state predicate -> keys}, evaluated
 * first-match-wins in progression order, with a guaranteed fallback entry.
 * That is why StoryGuide_GetCurrentEntry() cannot fail and never returns
 * empty keys.
 */

#include <stdbool.h>

typedef enum StoryGuideDetail {
  kStoryGuideOff = 0,
  kStoryGuideObjectives,
  kStoryGuideHints,
  kStoryGuideDetailed,
} StoryGuideDetail;

/*
 * A resolved guide entry.  heading_key and objective_key are always set;
 * hint_key and detail_key are NULL when the current detail level does not
 * include them, which is what lets the renderer lay out one, two or three
 * blocks without knowing why.
 */
typedef struct StoryGuideEntry {
  const char *heading_key;
  const char *objective_key;
  const char *hint_key;
  const char *detail_key;
} StoryGuideEntry;

/* Reads live Zelda3 progression state and fills *out.  Never fails. */
void StoryGuide_GetCurrentEntry(StoryGuideEntry *out);

/* Resolve a key to display text in the active language.  Returns the key
 * itself if it is unknown, so a missing string is visible rather than blank. */
const char *StoryGuide_Text(const char *key);

/* Whether the GUIDE page participates in the companion page cycle. */
StoryGuideDetail StoryGuide_Detail(void);
void StoryGuide_SetDetail(StoryGuideDetail detail);
bool StoryGuide_IsEnabled(void);

#endif  /* ALEKS_STORY_GUIDE_H_ */
