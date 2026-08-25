#include "switch_first_run.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <switch.h>

#include "asset_extractor.h"

enum { kMaxRoms = 16, kPathSize = 256 };

static bool HasRomExtension(const char *name) {
  const char *dot = strrchr(name, '.');
  return dot && (!strcasecmp(dot, ".sfc") || !strcasecmp(dot, ".smc"));
}

static int ScanRoms(char roms[kMaxRoms][kPathSize]) {
  DIR *dir = opendir(".");
  if (!dir) return 0;
  int count = 0;
  struct dirent *entry;
  while ((entry = readdir(dir)) && count < kMaxRoms) {
    if (HasRomExtension(entry->d_name)) {
      snprintf(roms[count], kPathSize, "%s", entry->d_name);
      count++;
    }
  }
  closedir(dir);
  return count;
}

static u64 WaitForChoice(int *choice, int count) {
  PadState pad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);
  for (;;) {
    padUpdate(&pad);
    u64 down = padGetButtonsDown(&pad);
    if ((down & HidNpadButton_Up) && *choice > 0) (*choice)--;
    if ((down & HidNpadButton_Down) && *choice + 1 < count) (*choice)++;
    if (down & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_Plus)) return down;
    consoleUpdate(NULL);
  }
}

bool SwitchFirstRun_EnsureAssets(char *selected_rom, size_t selected_rom_size,
                                 char *error, size_t error_size) {
  if (AssetExtractor_AssetsFileIsValid("zelda3_assets.dat")) return true;
  consoleInit(NULL);
  printf("ALEKS Zelda3 NX\n\nGame assets not found.\n\n[A] Create game assets\n[B] Exit\n");
  int ignored = 0;
  if (!(WaitForChoice(&ignored, 1) & HidNpadButton_A)) {
    snprintf(error, error_size, "Asset setup cancelled"); consoleExit(NULL); return false;
  }
  char roms[kMaxRoms][kPathSize];
  int count = ScanRoms(roms);
  if (!count) {
    snprintf(error, error_size, "No .sfc or .smc ROM found in Zelda3 folder"); consoleExit(NULL); return false;
  }
  int choice = 0;
select_rom:
  for (;;) {
    consoleClear();
    printf("SELECT SNES ROM\n\n");
    for (int i = 0; i < count; i++) printf("%c %s\n", i == choice ? '>' : ' ', roms[i]);
    printf("\n[A] Select  [B] Exit\n");
    u64 down = WaitForChoice(&choice, count);
    if (down & HidNpadButton_B) { snprintf(error, error_size, "Asset setup cancelled"); consoleExit(NULL); return false; }
    if (down & HidNpadButton_A) break;
  }
  snprintf(selected_rom, selected_rom_size, "%s", roms[choice]);
  consoleClear();
  printf("Validating ROM...\nGenerating assets...\n");
  bool ok = AssetExtractor_ExtractRom(selected_rom, "romfs:/zelda3_assets.bps",
                                      "zelda3_assets.tmp", "zelda3_assets.dat", error, error_size);
  if (ok) { printf("Done. Starting game...\n"); consoleUpdate(NULL); svcSleepThread(600000000); }
  else {
    printf("\nFAILED: %s\n\n[A] Choose another ROM  [B] Exit\n", error);
    consoleUpdate(NULL);
    if (WaitForChoice(&choice, 1) & HidNpadButton_A)
      goto select_rom;
  }
  consoleExit(NULL);
  return ok;
}
