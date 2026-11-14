#pragma once
#include "ppc_runtime.h"

void heap_register(void (*reg)(const char*, const char*, void (*)(PPCContext&)));
