#pragma once
#include "ppc_runtime.h"
#include <array>

// Registriert die Thread-/Sync-Funktionen von coreinit.
void threads_register(void (*reg)(const char*, const char*, void (*)(PPCContext&)));
// Der Hauptthread nimmt beim Start den globalen Lock.
void threads_main_enter();
// Laesst den aktuellen Gast-Thread schlafen (gibt solange den globalen Lock ab).
void guest_sleep_ns(int64_t ns);
uint32_t threads_specific(int i);
