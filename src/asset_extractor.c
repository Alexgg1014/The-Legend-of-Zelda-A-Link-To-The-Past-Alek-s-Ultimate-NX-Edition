#include "asset_extractor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "assets.h"
#include "util.h"

static AssetExtractorLogFn g_log_callback;

void AssetExtractor_SetLogCallback(AssetExtractorLogFn callback) {
  g_log_callback = callback;
}

static void Logf(const char *format, ...) {
  if (!g_log_callback)
    return;
  char message[384];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  g_log_callback(message);
}

static void SetError(char *error, size_t error_size, const char *message) {
  if (error_size) {
    snprintf(error, error_size, "%s", message);
    error[error_size - 1] = 0;
  }
}

static uint32 ReadU32LE(const uint8 *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32)p[3] << 24);
}

static bool AssetsBlobIsValid(const uint8 *data, size_t size) {
  static const uint8 signature[] = { kAssets_Sig };
  if (!data || size < 88 || memcmp(data, signature, sizeof(signature)) != 0 ||
      ReadU32LE(data + 80) != kNumberOfAssets)
    return false;
  uint32 names_size = ReadU32LE(data + 84);
  size_t offset = 88 + kNumberOfAssets * 4 + names_size;
  if (offset > size)
    return false;
  for (int i = 0; i < kNumberOfAssets; i++) {
    uint32 asset_size = ReadU32LE(data + 88 + i * 4);
    offset = (offset + 3) & ~(size_t)3;
    if (asset_size > size - offset)
      return false;
    offset += asset_size;
  }
  /* Assets 0/1/2 are the SPC sound banks (intro / indoor / ending).  A blob
   * that indexes cleanly but carries an empty bank produces a game that looks
   * fine and is silent -- a failure worth naming rather than shipping. */
  for (int i = 0; i < 3; i++) {
    if (ReadU32LE(data + 88 + i * 4) == 0) {
      Logf("ASSET VALIDATION FAILED: SOUND BANK %d MISSING / EMPTY", i);
      return false;
    }
  }
  Logf("SOUND BANK 0 SIZE: %u", ReadU32LE(data + 88));
  Logf("SOUND BANK 1 SIZE: %u", ReadU32LE(data + 92));
  Logf("SOUND BANK 2 SIZE: %u", ReadU32LE(data + 96));
  return true;
}

bool AssetExtractor_AssetsFileIsValid(const char *path) {
  size_t size = 0;
  uint8 *data = ReadWholeFile(path, &size);
  bool valid = AssetsBlobIsValid(data, size);
  free(data);
  return valid;
}

bool AssetExtractor_ExtractRom(const char *rom_path, const char *bps_path,
                               const char *tmp_path, const char *output_path,
                               char *error, size_t error_size) {
  size_t rom_size = 0, bps_size = 0, asset_size = 0;
  uint8 *rom_file = ReadWholeFile(rom_path, &rom_size);
  if (!rom_file) { SetError(error, error_size, "Unable to read ROM"); return false; }
  Logf("[EXTRACT 03] candidate selected: %s", rom_path);
  const uint8 *rom = rom_file;
  if (rom_size == 1049088) {
    rom += 512;
    rom_size -= 512;
    Logf("[EXTRACT 04] ROM validated: 1049088 bytes, 512-byte copier header removed");
  } else {
    Logf("[EXTRACT 04] ROM validation: %zu bytes, no copier header", rom_size);
  }
  if (rom_size != 1048576) {
    free(rom_file); SetError(error, error_size, "Unsupported ROM size"); return false;
  }
  Logf("[EXTRACT 04] ROM accepted: canonical 1048576-byte image");
  uint8 *bps = ReadWholeFile(bps_path, &bps_size);
  if (!bps) { free(rom_file); SetError(error, error_size, "Unable to read bundled BPS patch"); return false; }
  Logf("[EXTRACT 05] BPS patch begin: %s", bps_path);
  uint8 *assets = ApplyBps(rom, rom_size, bps, bps_size, &asset_size);
  free(bps);
  free(rom_file);
  if (!assets) { SetError(error, error_size, "Unsupported or wrong ROM"); return false; }
  Logf("[EXTRACT 06] BPS completed: %zu-byte output", asset_size);
  if (!AssetsBlobIsValid(assets, asset_size)) {
    free(assets); SetError(error, error_size, "Generated asset validation failed"); return false;
  }
  FILE *out = fopen(tmp_path, "wb");
  if (!out) { free(assets); SetError(error, error_size, "Unable to write temporary asset file"); return false; }
  size_t bytes_written = fwrite(assets, 1, asset_size, out);
  int flush_result = fflush(out);
  int close_result = fclose(out);
  bool written = bytes_written == asset_size && flush_result == 0 && close_result == 0;
  free(assets);
  if (!written || !AssetExtractor_AssetsFileIsValid(tmp_path)) {
    remove(tmp_path); SetError(error, error_size, "Temporary asset write or validation failed"); return false;
  }
  Logf("[EXTRACT 07] temporary output written: %s", tmp_path);
  Logf("[EXTRACT 08] temporary output validated");
  if (rename(tmp_path, output_path) != 0) {
    remove(tmp_path); SetError(error, error_size, "Unable to finalize asset file"); return false;
  }
  Logf("[EXTRACT 09] final rename complete: %s", output_path);
  return true;
}
