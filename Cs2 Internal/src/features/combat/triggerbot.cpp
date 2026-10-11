#include "features/combat/triggerbot.h"

namespace triggerbot
{
    float Hitchance(const combat_access::Context &context, const combat_access::Target &target, int index,
                    const combat_access::Vec3 &angles, SpreadSampler sample)
    {
        if (!context.valid || !context.accuracyValid || !sample || !combat_access::Finite(context.eye) ||
            !combat_access::Finite(angles) || !std::isfinite(context.weapon.range) || context.weapon.range <= 0 ||
            target.hitboxes.count <= 0 || target.hitboxes.count > static_cast<int>(target.hitboxes.entries.size())) return 0;
        const combat_access::Hitbox *hitbox = nullptr;
        for (const auto &entry : target.hitboxes) if (entry.index == index) { hitbox = &entry; break; }
        if (!hitbox || hitbox->bone < 0 || hitbox->bone >= static_cast<int>(target.bones.size())) return 0;
        combat_access::Vec3 forward, left, up;
        math::helpers::angle_vectors_left(angles, &forward, &left, &up);
        unsigned hits = 0;
        constexpr unsigned samples = 128;
        for (unsigned seed = 0; seed < samples; ++seed)
        {
            const auto spread = sample(context.weapon, seed);
            if (!std::isfinite(spread.x) || !std::isfinite(spread.y)) continue;
            const auto direction = (forward + left * spread.x + up * spread.y).normalized() * context.weapon.range;
            float fraction = 1;
            if (combat_access::RayHitbox(context.eye, direction, *hitbox, target.bones[hitbox->bone], fraction)) ++hits;
        }
        return static_cast<float>(hits) / samples;
    }
    Target Select(const combat_access::Context &context, const combat_features::Settings &settings,
                  std::span<const combat_access::Target> players, autowall::Evaluator evaluate, SpreadSampler sample)
    {
        Target best;
        if (!context.valid || !context.canShoot || !settings.triggerbot || !evaluate ||
            (settings.hitchance > 0 && !context.accuracyValid) ||
            !combat_access::Finite(context.eye) || !std::isfinite(context.weapon.range) || context.weapon.range <= 0) return best;
        const auto corrected = context.angles + context.punch;
        if (!combat_access::Finite(corrected)) return best;
        combat_access::Vec3 forward;
        math::helpers::angle_vectors_left(corrected, &forward);
        const auto delta = forward * context.weapon.range;
        float closest = 1;
        for (const auto &player : players)
        {
            if (!player.pawn || player.pawn == context.local.pawn || player.health <= 0 ||
                player.team == context.local.team || (player.team != 2 && player.team != 3) ||
                player.hitboxes.count <= 0 || player.hitboxes.count > static_cast<int>(player.hitboxes.entries.size())) continue;
            for (const auto &hitbox : player.hitboxes)
            {
                if (hitbox.bone < 0 || hitbox.bone >= static_cast<int>(player.bones.size()) ||
                    (settings.headOnly && combat_access::Hitgroup(hitbox.index) != 1)) continue;
                float fraction = 1;
                if (!combat_access::RayHitbox(context.eye, delta, hitbox, player.bones[hitbox.bone], fraction) || fraction >= closest) continue;
                autowall::Result hit;
                const auto point = context.eye + delta * std::min(1.f, fraction + .0001f);
                if (!evaluate(context, player, point, hit) || !std::isfinite(hit.damage) || hit.damage <= 0 ||
                    (hit.penetrated && (!settings.autowall || hit.damage < settings.minimumDamage)) ||
                    (settings.headOnly && hit.hitgroup != 1) ||
                    (settings.hitchance > 0 &&
                     Hitchance(context, player, hit.hitbox, corrected, sample) * 100 < settings.hitchance)) continue;
                best = {player.pawn, player.handle, hit.damage, true};
                closest = fraction;
            }
        }
        return best;
    }
    bool Ready(State &state, const combat_access::Context &context, const Target &target,
               int delayMilliseconds, ULONGLONG now)
    {
        if (!context.valid || !target.valid || !context.canShoot)
        {
            state.Reset();
            return false;
        }
        if (state.localHandle != context.local.handle || state.weaponHandle != context.weapon.handle ||
            context.tick < state.lastTick || now < state.lastTime) state.Reset();
        state.localHandle = context.local.handle;
        state.weaponHandle = context.weapon.handle;
        state.lastTick = context.tick;
        state.lastTime = now;
        if (target.pawn != state.pawn || target.handle != state.handle)
        {
            state.pawn = target.pawn;
            state.handle = target.handle;
            state.delayStart = now;
        }
        return now - state.delayStart >= static_cast<ULONGLONG>(std::max(0, delayMilliseconds)) &&
               (state.lastShotTick < 0 || context.tick - state.lastShotTick >= 2);
    }
}
