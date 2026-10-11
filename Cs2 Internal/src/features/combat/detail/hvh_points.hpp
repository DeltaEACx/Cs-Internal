#pragma once
#include "game/combat_access.h"

namespace hvh_points
{
    enum class Selection { Head = 0, HeadAndBody = 1, Body = 2, All = 3 };
    enum class Region { Unknown, Head, Neck, Body, Limb };

    struct Point
    {
        math::vector3 position{};
        int hitbox{-1};
        bool fallback{};
        Region region{Region::Unknown};
    };

    // Convert model-local hitbox extents to the same once-scaled geometry used
    // by recorded poses. Bone positions are already in world space. The caller
    // keeps the source box and pose untouched and reuses this copy for both aim
    // points and ray intersections; already-scaled records must not call this.
    inline std::optional<combat_access::Hitbox> ScaleGeometry(const combat_access::Hitbox& hitbox,
                                                             float scale) noexcept
    {
        const auto finite = [](const math::vector3& value) {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        };
        if (!std::isfinite(scale) || scale <= 0.f || scale > 100.f ||
            !finite(hitbox.mins) || !finite(hitbox.maxs) ||
            !std::isfinite(hitbox.radius) || hitbox.radius < 0.f) return {};
        auto result = hitbox;
        result.mins = hitbox.mins * scale;
        result.maxs = hitbox.maxs * scale;
        result.radius = hitbox.radius * scale;
        return finite(result.mins) && finite(result.maxs) && std::isfinite(result.radius) ?
            std::optional{result} : std::nullopt;
    }

    inline bool Accept(const Point& point, Selection selection = Selection::Head) noexcept
    {
        switch (selection)
        {
        case Selection::Head: return point.region == Region::Head;
        case Selection::HeadAndBody: return point.region == Region::Head || point.region == Region::Body;
        case Selection::Body: return point.region == Region::Body;
        case Selection::All: return true;
        default: return false;
        }
    }

    namespace detail
    {
        // These are hitbox ordinals (the same grouping as Hitgroup), never
        // native model bone indices or the ESP anatomy-slot numbers.
        inline Region NativeRegion(int hitbox, int group) noexcept
        {
            // Custom models may reorder hitboxes; declared hitgroups retain
            // their anatomy even when a head is not ordinal0.
            if (group == 1) return Region::Head;
            if (group == 8) return Region::Neck;
            if (group == 2 || group == 3) return Region::Body;
            if (group >= 4 && group <= 7) return Region::Limb;
            if (group != -1) return Region::Unknown;
            if (hitbox == 0) return Region::Head;
            if (hitbox == 1) return Region::Neck;
            if (hitbox >= 2 && hitbox <= 6) return Region::Body;
            if (hitbox >= 7 && hitbox <= 18) return Region::Limb;
            return Region::Unknown;
        }
        inline bool ValidPosition(const math::vector3& position) noexcept
        {
            if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) ||
                std::abs(position.x) > 1e7f || std::abs(position.y) > 1e7f || std::abs(position.z) > 1e7f) return false;
            const double squared = double(position.x) * position.x + double(position.y) * position.y + double(position.z) * position.z;
            return squared >= .001;
        }
        inline void Append(std::vector<Point>& points, const math::vector3& position, int hitbox, bool fallback, Region region)
        {
            if (!ValidPosition(position)) return;
            for (const auto& point : points)
            {
                // Coincident head/body centers must survive region filtering.
                if (point.region != region) continue;
                const double dx = double(position.x) - point.position.x;
                const double dy = double(position.y) - point.position.y;
                const double dz = double(position.z) - point.position.z;
                if (dx * dx + dy * dy + dz * dz <= .0001) return;
            }
            points.push_back({position, hitbox, fallback, region});
        }
    }

    // Native hitbox indices address the model's actual bones, not the remapped
    // visual anatomy slots. Missing geometry uses only existing world-space
    // anatomy points; a missing head can recover independently of the torso.
    // Native tracing still confirms the pawn.
    template<class NativeBone, class Skeleton>
    std::vector<Point> Collect(const combat_access::Target& target, NativeBone&& nativeBone, Skeleton&& skeleton,
                               float pointScale = 0.f)
    {
        std::vector<Point> points;
        const bool multipoints = std::isfinite(pointScale) && pointScale > 0.f && pointScale <= 1.f;
        points.reserve(multipoints ? 140 : 21);
        if (target.hitboxes.count > 0 && target.hitboxes.count <= static_cast<int>(target.hitboxes.entries.size()))
        {
            for (const auto& hitbox : target.hitboxes)
            {
                if (hitbox.bone < 0 || hitbox.bone >= 1024 || !std::isfinite(hitbox.radius) || hitbox.radius < 0 ||
                    !std::isfinite(hitbox.mins.x) || !std::isfinite(hitbox.mins.y) || !std::isfinite(hitbox.mins.z) ||
                    !std::isfinite(hitbox.maxs.x) || !std::isfinite(hitbox.maxs.y) || !std::isfinite(hitbox.maxs.z)) continue;
                const auto bone = nativeBone(static_cast<unsigned>(hitbox.bone));
                if (!detail::ValidPosition(bone.position)) continue;
                const auto& rotation = bone.rotation;
                const double norm = double(rotation.x) * rotation.x + double(rotation.y) * rotation.y +
                                    double(rotation.z) * rotation.z + double(rotation.w) * rotation.w;
                if (!hitbox.translation_only && (!std::isfinite(norm) || std::abs(norm - 1.) > .1)) continue;
                const math::vector3 midpoint{hitbox.mins.x * .5f + hitbox.maxs.x * .5f,
                                             hitbox.mins.y * .5f + hitbox.maxs.y * .5f,
                                             hitbox.mins.z * .5f + hitbox.maxs.z * .5f};
                const auto point = bone.position + (hitbox.translation_only ? midpoint : rotation.rotate_vector(midpoint));
                const auto region = detail::NativeRegion(hitbox.index, hitbox.group);
                detail::Append(points, point, hitbox.index, false, region);
                if (multipoints) {
                    // A sphere around the capsule segment's midpoint lies
                    // inside the capsule. For a box, keep each sample within
                    // its local half extents. Apply the same bone-space
                    // transform as the center; metadata-free fallbacks cannot
                    // invent a collision radius and receive no extra samples.
                    const math::vector3 reach = hitbox.radius > .001f ?
                        math::vector3{hitbox.radius * pointScale, hitbox.radius * pointScale, hitbox.radius * pointScale} :
                        (hitbox.maxs - hitbox.mins) * (.5f * pointScale);
                    const std::array<math::vector3, 6> offsets{{
                        {reach.x, 0, 0}, {-reach.x, 0, 0},
                        {0, reach.y, 0}, {0, -reach.y, 0},
                        {0, 0, reach.z}, {0, 0, -reach.z}
                    }};
                    for (const auto& offset : offsets) {
                        const auto local = midpoint + offset;
                        const auto position = bone.position + (hitbox.translation_only ? local : rotation.rotate_vector(local));
                        detail::Append(points, position, hitbox.index, false, region);
                    }
                }
            }
        }
        if (!points.empty()) {
            for (const auto& point : points) if (point.region == Region::Head) return points;
            // Usable torso/limb metadata must not suppress the real named head
            // when a custom head hitbox is absent or not yet pose-readable.
            const auto pose = skeleton();
            detail::Append(points, pose[cstypes::bone_ids::head].position, -1, true, Region::Head);
            return points;
        }

        // These are the same verified anatomy slots that ESP uses. Missing model
        // bones remain zero in the snapshot and are discarded, without invented
        // offsets, collision radii, or assumed native skeleton ordering.
        struct Anatomy { unsigned slot; Region region; };
        constexpr std::array<Anatomy, 21> slots{{
            {cstypes::bone_ids::head, Region::Head}, {cstypes::bone_ids::neck, Region::Neck},
            {cstypes::bone_ids::spine_4, Region::Body}, {cstypes::bone_ids::spine_3, Region::Body},
            {cstypes::bone_ids::spine_2, Region::Body}, {cstypes::bone_ids::spine_1, Region::Body},
            {cstypes::bone_ids::pelvis, Region::Body},
            {cstypes::bone_ids::left_clavicle, Region::Limb}, {cstypes::bone_ids::left_shoulder, Region::Limb},
            {cstypes::bone_ids::left_elbow, Region::Limb}, {cstypes::bone_ids::left_hand, Region::Limb},
            {cstypes::bone_ids::right_clavicle, Region::Limb}, {cstypes::bone_ids::right_shoulder, Region::Limb},
            {cstypes::bone_ids::right_elbow, Region::Limb}, {cstypes::bone_ids::right_hand, Region::Limb},
            {cstypes::bone_ids::left_hip, Region::Limb}, {cstypes::bone_ids::left_knee, Region::Limb}, {cstypes::bone_ids::left_foot, Region::Limb},
            {cstypes::bone_ids::right_hip, Region::Limb}, {cstypes::bone_ids::right_knee, Region::Limb}, {cstypes::bone_ids::right_foot, Region::Limb}
        }};
        const auto pose = skeleton();
        for (const auto [slot, region] : slots) detail::Append(points, pose[slot].position, -1, true, region);
        return points;
    }
}
