#pragma once
#include "features/combat/aimbot.h"

namespace triggerbot
{
    struct Target
    {
        combat_access::Address pawn{};
        std::uint32_t handle{0xffffffffu};
        float damage{};
        bool valid{};
    };
    struct State
    {
        combat_access::Address pawn{};
        std::uint32_t handle{0xffffffffu}, localHandle{0xffffffffu}, weaponHandle{0xffffffffu};
        ULONGLONG delayStart{}, lastTime{};
        int lastShotTick{-1}, lastTick{-1};
        void Reset() { *this = {}; }
    };
    using SpreadSampler = math::vector2 (*)(const combat_access::Weapon &, unsigned);
    float Hitchance(const combat_access::Context &context, const combat_access::Target &target, int hitbox,
                    const combat_access::Vec3 &angles, SpreadSampler sample = combat_access::Spread);
    Target Select(const combat_access::Context &context, const combat_features::Settings &settings,
                  std::span<const combat_access::Target> players,
                  autowall::Evaluator evaluate = autowall::Evaluate, SpreadSampler sample = combat_access::Spread);
    bool Ready(State &state, const combat_access::Context &context, const Target &target,
               int delayMilliseconds, ULONGLONG now);
}
