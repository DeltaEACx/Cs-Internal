#pragma once
#include "game/autowall_probe.h"
#include "chams.h"

namespace visuals::penetrable_props {
void BeginMap(std::uint64_t epoch,std::uintptr_t list);
void Remember(const autowall_probe::Result& result);
void Clear();
std::size_t Count();
// Checks at most 64 entity slots and issues at most one native query. Returns
// true exactly when a native query consumed the caller's per-frame budget.
bool Discover(const autowall_probe::Frame& profile,float maximumDistance,float minimumDamage);
// Called only for model scene descriptors, whose full owner handle is audited.
bool Generate(std::uintptr_t self,std::uintptr_t scene,std::uintptr_t view,
              std::uintptr_t output,chams::GenerateFn original,color tint);
}
