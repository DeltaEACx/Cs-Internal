#include "features/combat/aimbot.h"

namespace aimbot
{
    Target Select(const combat_access::Context &context, const combat_features::Settings &settings,
                  std::span<const combat_access::Target> players, autowall::Evaluator evaluate)
    {
        Target best;
        if (!context.valid || !settings.aimbot || !evaluate || !combat_access::Finite(context.eye) ||
            !combat_access::Finite(context.angles) || !std::isfinite(settings.fov) || settings.fov < 0) return best;
        auto detection = context.angles;
        if (settings.recoilControl) detection += context.punch;
        if (!combat_access::Finite(detection)) return best;
        detection.y = std::remainder(detection.y, 360.f);
        for (const auto &player : players)
        {
            if (!player.pawn || player.pawn == context.local.pawn || player.health <= 0 ||
                player.team == context.local.team || (player.team != 2 && player.team != 3) ||
                player.hitboxes.count <= 0 || player.hitboxes.count > static_cast<int>(player.hitboxes.entries.size())) continue;
            float damage = -1, playerFov = 999;
            combat_access::Vec3 angle;
            bool found = false;
            int selectedHitbox = -1;
            for (const auto &hitbox : player.hitboxes)
            {
                const auto group = combat_access::Hitgroup(hitbox.index);
                const auto option = group == 1 ? 0 : (group == 2 || group == 8) ? 1 : group == 3 ? 2 :
                    (group == 4 || group == 5) ? 3 : (group == 6 || group == 7) ? 4 : -1;
                if (option < 0 || !settings.hitboxes[option] || hitbox.bone < 0 ||
                    hitbox.bone >= static_cast<int>(player.bones.size()) ||
                    !combat_access::Finite(hitbox.mins) || !combat_access::Finite(hitbox.maxs) ||
                    !std::isfinite(hitbox.radius) || hitbox.radius < 0) continue;
                const auto &bone = player.bones[hitbox.bone];
                if (!combat_access::Finite(bone.position) || !std::isfinite(bone.rotation.x) ||
                    !std::isfinite(bone.rotation.y) || !std::isfinite(bone.rotation.z) ||
                    !std::isfinite(bone.rotation.w)) continue;
                const auto midpoint = (hitbox.mins + hitbox.maxs) * .5f;
                const auto point = bone.position + (hitbox.translation_only ? midpoint : bone.rotation.rotate_vector(midpoint));
                if (!combat_access::Finite(point) || (point - context.eye).length_sqr() < .001f) continue;
                const auto aim = math::helpers::calculate_angle(context.eye, point);
                const auto fov = math::helpers::angle_distance(detection, aim);
                if (!std::isfinite(fov) || fov > settings.fov) continue;
                autowall::Result hit;
                if (!evaluate(context, player, point, hit) || !std::isfinite(hit.damage) || hit.damage <= 0 ||
                    (hit.penetrated && (!settings.autowall || hit.damage < settings.minimumDamage))) continue;
                if (hit.damage > damage || (hit.damage == damage && fov < playerFov))
                {
                    damage = hit.damage;
                    playerFov = fov;
                    angle = aim;
                    selectedHitbox = hit.hitbox;
                    found = true;
                }
            }
            if (!found) continue;
            const bool lethal = damage >= player.health;
            const bool bestLethal = best.valid && best.damage >= best.health;
            // Spectral prioritizes lethal targets, then damage and angular distance.
            if (best.valid && !lethal && bestLethal) continue;
            const float score = 1000 + damage * 100 + 180 - playerFov;
            if (!best.valid || (lethal && !bestLethal) || (lethal == bestLethal && score > best.score))
                best = {player.pawn, player.handle, angle, damage, playerFov, score, player.health, true, selectedHitbox};
        }
        return best;
    }
    bool ShouldAutoFire(const combat_access::Context &context, const combat_features::Settings &settings,
                        const Target &target, bool activationHeld)
    {
        const unsigned entityIndex = target.handle & 0x7fff;
        // Select already checks hitboxes, FOV, enemy identity and penetration damage.
        // Automatic silent shots use that fresh result independently of trigger settings.
        return settings.aimbot && settings.silentAim && activationHeld && context.valid && context.canShoot &&
            context.local.pawn && target.valid && target.pawn && target.pawn != context.local.pawn &&
            target.handle != 0xffffffffu && target.handle != context.local.handle &&
            entityIndex > 0 && entityIndex < 0x7fff && target.health > 0 &&
            combat_access::Finite(target.angles) && std::isfinite(target.damage) && target.damage > 0;
    }

    combat_access::Vec3 Smooth(const combat_access::Vec3 &view, const combat_access::Vec3 &aim, int amount)
    {
        if (!combat_access::Finite(view) || !combat_access::Finite(aim)) return view;
        auto delta = aim - view;
        delta.x = std::remainder(delta.x, 360.f);
        delta.y = std::remainder(delta.y, 360.f);
        if (amount > 0)
        {
            const auto length = std::sqrt(delta.x * delta.x + delta.y * delta.y);
            const auto t = std::clamp(length / 10.f, 0.f, 1.f);
            const auto remaining = 1 - t;
            // Retain Spectral's cubic smoothing curve.
            const auto ease = 3 * remaining * remaining * t * .10f + 3 * remaining * t * t * .90f + t * t * t;
            delta *= (.3f + ease * .7f) / static_cast<float>(std::max(1, amount));
        }
        auto result = view + delta;
        result.x = std::clamp(result.x, -89.f, 89.f);
        result.y = std::remainder(result.y, 360.f);
        result.z = 0;
        return result;
    }
    combat_access::Vec3 Angles(const combat_access::Context &context, const combat_features::Settings &settings,
                              const Target &target)
    {
        if (!context.valid || !target.valid || !combat_access::Finite(target.angles)) return {};
        auto angles = target.angles;
        if (settings.recoilControl)
        {
            if (!combat_access::Finite(context.punch) || !std::isfinite(settings.recoilStrength)) return {};
            angles -= context.punch * (std::clamp(settings.recoilStrength, 0.f, 150.f) / 100.f);
        }
        angles.x = std::clamp(angles.x, -89.f, 89.f);
        angles.y = std::remainder(angles.y, 360.f);
        angles.z = 0;
        return combat_access::Finite(angles) ? angles : combat_access::Vec3{};
    }
    combat_access::Vec3 Delta(const combat_access::Context &context, const combat_features::Settings &settings,
                             const Target &target)
    {
        if (!context.valid || !target.valid || !combat_access::Finite(context.angles) ||
            !combat_access::Finite(target.angles) || (settings.recoilControl &&
            (!combat_access::Finite(context.punch) || !std::isfinite(settings.recoilStrength)))) return {};
        auto angles = target.angles;
        if (settings.recoilControl) angles -= context.punch * (std::clamp(settings.recoilStrength, 0.f, 150.f) / 100.f);
        auto delta = Smooth(context.angles, angles, settings.smooth) - context.angles;
        delta.y = std::remainder(delta.y, 360.f);
        const auto quantum = context.sensitivity * .022f * context.fovAdjust;
        if (std::isfinite(quantum) && quantum > .000001f)
        {
            delta.x = std::round(delta.x / quantum) * quantum;
            delta.y = std::round(delta.y / quantum) * quantum;
        }
        delta.z = 0;
        return combat_access::Finite(delta) ? delta : combat_access::Vec3{};
    }
}
