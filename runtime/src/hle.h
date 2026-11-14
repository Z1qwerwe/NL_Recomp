// High-Level-Emulation der Wii-U-Systembibliotheken (coreinit, gx2, ...).
#pragma once
#include "ppc_runtime.h"

void hle_init(PPCContext& c);
void hle_report();
bool hle_is_fake_addr(uint32_t addr);
void hle_call_fake(PPCContext& c, uint32_t addr);
uint32_t hle_alloc(uint32_t size, uint32_t align);
