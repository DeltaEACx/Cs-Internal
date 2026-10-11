#pragma once
#include "chams.h"

namespace visuals::chams::materials
{
    const char *Name(Material material);
    std::string Source(Material material, bool ignoreDepth);
    bool Initialize();
    void Ensure(Material material, bool ignoreDepth);
    std::uintptr_t Find(Material material, bool ignoreDepth);
    std::uintptr_t CreateCached(const std::string& key, const std::string& source);
    void Reset();
    void Shutdown();
}
