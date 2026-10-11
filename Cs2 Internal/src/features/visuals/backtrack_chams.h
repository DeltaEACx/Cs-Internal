#pragma once
#include "chams.h"
namespace visuals::backtrack_chams {
bool Initialize();
void Frame(bool inWorld);
void Reset();
void Shutdown();
bool Generate(std::uintptr_t self, std::uintptr_t scene, std::uintptr_t view,
    std::uintptr_t output, chams::GenerateFn original);
void DrawMenu();
}
