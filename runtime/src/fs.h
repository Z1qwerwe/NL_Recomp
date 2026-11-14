#pragma once
#include "ppc_runtime.h"

// Liest NL_CONTENT_DIRS / NL_SAVE_DIR aus der Umgebung.
void fs_init_paths();
void fs_register(void (*reg)(const char*, const char*, void (*)(PPCContext&)));
