#pragma once
#include "game/combat_access.h"
#include "features/combat/detail/hvh_points.hpp"
#include <memory>

namespace backtrack {
struct Settings { bool enabled{true}; float windowMs{100.f}; };
struct Record {
    std::uint64_t epoch{}, generation{};
    game_access::Address list{}, pawn{}, scene{}, binding{}, model{};
    std::uint32_t handle{0xffffffffu}, localHandle{0xffffffffu};
    int team{};
    float simulationTime{};
    math::vector3 origin{};
    std::array<float, 8> transform{};
    std::vector<systems::bones::data> bones;
    combat_access::Target target;
    std::vector<hvh_points::Point> points;
};
using Snapshot = std::shared_ptr<const Record>;
Settings Get();
void Set(Settings);
void Frame(bool inWorld);
void Reset();
bool Current(const Record&);
std::vector<Snapshot> Records(game_access::Address pawn, std::uint32_t handle);
std::vector<Snapshot> Oldest();
std::optional<combat_bindings::BacktrackTiming> Timing(const Record&);
std::optional<math::vector3> Intersection(const Record&, const math::vector3& eye, const math::vector3& point);
void DrawMenu();
}
