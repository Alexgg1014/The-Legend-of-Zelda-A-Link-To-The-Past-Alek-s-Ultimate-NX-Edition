/* Native offline Story Guide. It owns no save data: the current objective is
 * derived from Zelda3's canonical progression fields every time it is read. */
#include "story_guide.h"

#include <stddef.h>
#include <string.h>

#include "config.h"
#include "variables.h"

typedef struct GuideRule {
  const char *heading, *objective, *hint, *detail;
} GuideRule;

static const GuideRule kRules[] = {
  {"THE RESCUE", "RESCUE ZELDA AND ESCAPE THE CASTLE",
   "A SECRET PASSAGE CONNECTS THE CASTLE TO THE SANCTUARY.",
   "FOLLOW THE SEWERS NORTH. USE THE THRONE ROOM ORNAMENT TO OPEN THE PASSAGE."},
  {"THE FIRST PENDANT", "VISIT SAHASRAHLA AND CLEAR THE EASTERN PALACE",
   "THE ELDER HIDES NEAR THE EASTERN PALACE.",
   "GO EAST FROM HYRULE CASTLE. SPEAK TO SAHASRAHLA, THEN ENTER THE EASTERN PALACE."},
  {"THE SECOND PENDANT", "FIND THE BOOK OF MUDORA AND CLEAR THE DESERT PALACE",
   "THE LIBRARY HOLDS A BOOK THAT TRANSLATES THE DESERT SCRIPT.",
   "DASH INTO THE LIBRARY SHELF TO DROP THE BOOK. READ THE TABLET AT THE DESERT PALACE."},
  {"THE THIRD PENDANT", "CLIMB DEATH MOUNTAIN AND CLEAR THE TOWER OF HERA",
   "AN OLD MAN IN THE MOUNTAIN CAVES KNOWS THE WAY UP.",
   "ESCORT THE OLD MAN, USE HIS MIRROR, THEN REACH THE TOWER ON THE SUMMIT."},
  {"THE MASTER SWORD", "CLAIM THE MASTER SWORD IN THE LOST WOODS",
   "THE THREE PENDANTS PROVE YOU ARE READY.",
   "ENTER THE LOST WOODS FROM KAKARIKO AND FIND THE TRUE PEDESTAL IN THE NORTHWEST GROVE."},
  {"HYRULE CASTLE TOWER", "BREAK THE CASTLE BARRIER AND DEFEAT AGAHNIM",
   "THE MASTER SWORD CAN CUT THE MAGIC SEAL.",
   "RETURN TO HYRULE CASTLE, CUT THE ELECTRIC BARRIER AND CLIMB THE CENTRAL TOWER."},
  {"THE SEVEN MAIDENS", "RESCUE THE REMAINING MAIDENS IN THE DARK WORLD",
   "YOUR MAP MARKS EVERY CRYSTAL YOU HAVE NOT YET CLAIMED.",
   "CLEAR THE NUMBERED DARK WORLD DUNGEONS. EARLIER PALACE ITEMS OPEN THE LATER ONES."},
  {"GANONS TOWER", "ENTER GANONS TOWER AND FINISH THE FINAL BATTLE",
   "ALL SEVEN CRYSTALS OPEN THE TOWER ON DEATH MOUNTAIN.",
   "CLEAR THE TOWER, DEFEAT AGAHNIM AGAIN, THEN FOLLOW GANON TO THE PYRAMID."},
  {"HYRULE IS SAVED", "THE MAIN QUEST IS COMPLETE",
   "OPTIONAL HEART PIECES AND UPGRADES MAY STILL REMAIN.",
   "CONTINUE EXPLORING OR START ANOTHER FILE WHEN YOU ARE READY."},
};

/* These are deliberately broad, provable milestones. A coarse correct chapter
 * is more useful than a room-level instruction inferred from an ambiguous bit. */
static int StoryGuide_Resolve(void) {
  if (main_module_index == 25) return 8;
  if ((link_has_crystals & 0x7f) == 0x7f) return 7;
  if (link_has_crystals & 0x7f) return 6;
  if (link_sword_type >= 2 || sram_progress_indicator >= 3) return 5;
  if ((link_which_pendants & 7) == 7) return 4;
  if (!(link_which_pendants & 1)) return sram_progress_indicator < 2 ? 0 : 1;
  if (!(link_which_pendants & 2)) return 2;
  if (!(link_which_pendants & 4)) return 3;
  return 4;
}

/* Entries currently use their display text as stable keys. The resolver API
 * stays key based, so a guide language table can replace this lookup later. */
const char *StoryGuide_Text(const char *key) { return key ? key : ""; }

void StoryGuide_GetCurrentEntry(StoryGuideEntry *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  const GuideRule *r = &kRules[StoryGuide_Resolve()];
  out->heading_key = r->heading;
  out->objective_key = r->objective;
  StoryGuideDetail level = StoryGuide_Detail();
  if (level >= kStoryGuideHints) out->hint_key = r->hint;
  if (level >= kStoryGuideDetailed) out->detail_key = r->detail;
}

StoryGuideDetail StoryGuide_Detail(void) {
  uint8 v = g_config.aleks_story_guide;
  return v <= kStoryGuideDetailed ? (StoryGuideDetail)v : kStoryGuideObjectives;
}
void StoryGuide_SetDetail(StoryGuideDetail level) { g_config.aleks_story_guide = (uint8)level; }
bool StoryGuide_IsEnabled(void) { return StoryGuide_Detail() != kStoryGuideOff; }
