#pragma once

#include <stdbool.h>
#include <stddef.h>

bool SwitchFirstRun_EnsureAssets(char *selected_rom, size_t selected_rom_size,
                                 char *error, size_t error_size);
