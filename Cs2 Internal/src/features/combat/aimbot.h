#pragma once
#include "features/combat/combat.h"
#include "features/combat/autowall.h"

namespace aimbot
{
    struct Target
    {
        combat_access::Address pawn{};
        std::uint32_t handle{0xffffffffu};
        combat_access::Vec3 angles{};
        float damage{}, fov{}, score{};
        int health{};
        bool valid{};
        int hitbox{-1};
    };
    Target Select(const combat_access::Context &context, const combat_features::Settings &settings,
                  std::span<const combat_access::Target> players, autowall::Evaluator evaluate = autowall::Evaluate);
    bool ShouldAutoFire(const combat_access::Context &context, const combat_features::Settings &settings,
                        const Target &target, bool activationHeld);
    combat_access::Vec3 Smooth(const combat_access::Vec3 &view, const combat_access::Vec3 &aim, int amount);
    combat_access::Vec3 Angles(const combat_access::Context &context, const combat_features::Settings &settings,
                              const Target &target);
    combat_access::Vec3 Delta(const combat_access::Context &context, const combat_features::Settings &settings,
                             const Target &target);
}
