#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef void (*AssetExtractorLogFn)(const char *message);

void AssetExtractor_SetLogCallback(AssetExtractorLogFn callback);
bool AssetExtractor_AssetsFileIsValid(const char *path);
bool AssetExtractor_ExtractRom(const char *rom_path, const char *bps_path,
                               const char *tmp_path, const char *output_path,
                               char *error, size_t error_size);
