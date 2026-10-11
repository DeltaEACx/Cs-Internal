#pragma once
#include "game/combat_access.h"

namespace autowall
{
    struct Result { float damage{}; int hitbox{-1}, hitgroup{}; bool penetrated{}; };
    enum class ProbeReason : std::uint8_t
    {
        Unavailable, InvalidContext, InvalidDirection, TraceSetupFailed, InvalidTrace,
        BulletSimulationFailed, InvalidContact, Clear, NoPenetration,
        DamageExhausted, Stopped, NoPenetrationPower, Penetrated
    };
    struct ProbeResult
    {
        // Raw damage from the first penetrated material span, before target scaling.
        float damage{};
        bool hitSurface{}, penetrated{};
        // Distinguishes clear space from an unavailable or malformed native trace.
        bool valid{};
        ProbeReason reason{ProbeReason::Unavailable};
        int contactCount{}, contactIndex{-1};
        std::uint8_t flags{};
        float enterFraction{}, exitFraction{}, contactDamage{};
    };
    using Evaluator = bool (*)(const combat_access::Context &, const combat_access::Target &,
                               const combat_access::Vec3 &, Result &);
    bool Evaluate(const combat_access::Context &context, const combat_access::Target &target,
                  const combat_access::Vec3 &point, Result &result);
    // Test material contacts along the visible crosshair, without a target pawn.
    bool Probe(const combat_access::Context &context, const combat_access::Vec3 &direction,
               ProbeResult &result);
    // Release native-owned vector storage after combat callbacks have stopped.
    bool Reset();
    const char *ProbeReasonText(ProbeReason reason);
    float ScaleDamage(float damage, int group, int armor, bool helmet, int team,
                      float armorRatio, float headMultiplier, const combat_access::DamageScales &scales);
}
