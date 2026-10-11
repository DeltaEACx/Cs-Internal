#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <span>

namespace hvh_math
{
    struct Vec3 { float x{}, y{}, z{}; };
    struct Vec2 { float x{}, y{}; };
    struct Candidate
    {
        std::uintptr_t pawn{};
        std::uint32_t handle{0xffffffffu};
        int team{}, health{};
        // valid means the caller validated this pawn's complete entity handle.
        bool valid{}, dormant{}, immune{}, hittable{};
        Vec3 point{};
        float damage{};
        int hitbox{-1};
    };
    struct View
    {
        std::uintptr_t localPawn{};
        std::uint32_t localHandle{0xffffffffu};
        int localTeam{};
        Vec3 eye{}, angles{};
        float range{}, maxFov{180}, minDamage{1};
        bool allowBehind{};
        // If supplied, this row-major world-to-clip matrix also rejects points
        // outside the viewport. Ranking then uses distance from screen center.
        const float (*matrix)[4]{};
        float width{}, height{};
    };
    struct Selection
    {
        std::size_t index{};
        Vec3 angles{};
        float angularDistance{}, distance{}, damage{};
        double score{};
    };

    namespace detail
    {
        inline constexpr double kRadians = std::numbers::pi / 180.;
        inline constexpr double kDegrees = 180. / std::numbers::pi;
        struct DVec3
        {
            double x{}, y{}, z{};
            DVec3 operator+(DVec3 b) const noexcept { return {x + b.x, y + b.y, z + b.z}; }
            DVec3 operator-(DVec3 b) const noexcept { return {x - b.x, y - b.y, z - b.z}; }
            DVec3 operator*(double b) const noexcept { return {x * b, y * b, z * b}; }
        };
        inline bool Finite(Vec3 v) noexcept
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }
        inline DVec3 Convert(Vec3 v) noexcept { return {v.x, v.y, v.z}; }
        inline double Dot(DVec3 a, DVec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
        inline bool Normalize(DVec3& value) noexcept
        {
            const double squared = Dot(value, value);
            if (!std::isfinite(squared) || squared <= 1e-16) return false;
            value = value * (1. / std::sqrt(squared));
            return true;
        }
        inline DVec3 Forward(Vec3 angles) noexcept
        {
            const double pitch = std::remainder(double(angles.x), 360.) * kRadians;
            const double yaw = std::remainder(double(angles.y), 360.) * kRadians;
            return {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), -std::sin(pitch)};
        }
        inline bool Handle(std::uint32_t handle) noexcept
        {
            const auto index = handle & 0x7fffu;
            return handle != 0xffffffffu && index > 0 && index < 0x7fffu;
        }
    }

    // Set requireHittable=false to sort geometry-qualified points before native
    // penetration evaluation. Select always requires a fresh successful hit.
    inline std::optional<Selection> ProjectCandidate(const View& view, const Candidate& candidate,
                                                     std::size_t index, bool requireHittable = true) noexcept
    {
        using namespace detail;
        if (!view.localPawn || !Handle(view.localHandle) || (view.localTeam != 2 && view.localTeam != 3) ||
            !Finite(view.eye) || !Finite(view.angles) || !std::isfinite(view.range) ||
            view.range <= 0 || view.range > 32768 || !std::isfinite(view.maxFov) ||
            view.maxFov < 0 || view.maxFov > 180 || !std::isfinite(view.minDamage) || view.minDamage < 0 ||
            !candidate.valid || !candidate.pawn || candidate.pawn == view.localPawn ||
            !Handle(candidate.handle) || candidate.handle == view.localHandle || candidate.health <= 0 ||
            (candidate.team != 2 && candidate.team != 3) || candidate.team == view.localTeam ||
            candidate.dormant || candidate.immune || !Finite(candidate.point)) return {};
        if (requireHittable && (!candidate.hittable || !std::isfinite(candidate.damage) ||
                               candidate.damage <= 0 || candidate.damage < view.minDamage)) return {};

        DVec3 direction = Convert(candidate.point) - Convert(view.eye);
        const double distance = std::sqrt(Dot(direction, direction));
        if (!std::isfinite(distance) || distance <= 1e-4 || distance > view.range || !Normalize(direction)) return {};
        const double cosine = std::clamp(Dot(Forward(view.angles), direction), -1., 1.);
        // A third-person camera can see points behind the player's eye. When
        // supplied, its projection decides visibility; input angles are only
        // the fallback when no visible-camera matrix exists.
        if (!view.matrix && !view.allowBehind && cosine <= 0) return {};
        const double angularDistance = std::acos(cosine) * kDegrees;
        if (angularDistance > view.maxFov) return {};

        double score = angularDistance;
        if (view.matrix)
        {
            if (!std::isfinite(view.width) || !std::isfinite(view.height) || view.width <= 0 || view.height <= 0) return {};
            for (int row = 0; row < 4; ++row)
                for (int column = 0; column < 4; ++column)
                    if (!std::isfinite(view.matrix[row][column])) return {};
            const auto transform = [&](int row)
            {
                return double(view.matrix[row][0]) * candidate.point.x + double(view.matrix[row][1]) * candidate.point.y +
                       double(view.matrix[row][2]) * candidate.point.z + view.matrix[row][3];
            };
            const double w = transform(3);
            if (w < static_cast<double>(.01f)) return {};
            const double x = transform(0) / w, y = transform(1) / w;
            if (!std::isfinite(x) || !std::isfinite(y) || x < -1 || x > 1 || y < -1 || y > 1) return {};
            const double dx = x * view.width * .5, dy = y * view.height * .5;
            score = dx * dx + dy * dy;
        }
        const DVec3 delta = Convert(candidate.point) - Convert(view.eye);
        const Vec3 angles{
            static_cast<float>(std::atan2(-delta.z, std::hypot(delta.x, delta.y)) * kDegrees),
            static_cast<float>(std::atan2(delta.y, delta.x) * kDegrees), 0
        };
        return Selection{index, angles, static_cast<float>(angularDistance), static_cast<float>(distance),
                         requireHittable ? candidate.damage : 0.f, score};
    }

    inline bool Closer(const Selection& a, const Selection& b) noexcept
    {
        if (a.score != b.score) return a.score < b.score;
        if (a.angularDistance != b.angularDistance) return a.angularDistance < b.angularDistance;
        if (a.damage != b.damage) return a.damage > b.damage;
        if (a.distance != b.distance) return a.distance < b.distance;
        return a.index < b.index;
    }

    inline std::optional<Selection> Select(const View& view, std::span<const Candidate> candidates) noexcept
    {
        std::optional<Selection> best;
        for (std::size_t i = 0; i < candidates.size(); ++i)
            if (const auto selection = ProjectCandidate(view, candidates[i], i); selection && (!best || Closer(*selection, *best)))
                best = selection;
        return best;
    }

    // Spread seeds describe the physical angle after native recoil is added.
    // With recoil correction disabled, keep that recoil in the desired shot;
    // it is still removed from the command after solving spread so the server
    // reconstructs exactly the physical angle used by the seed search.
    inline std::optional<Vec3> ShotAim(Vec3 desired, Vec3 punch, bool recoilCorrection) noexcept
    {
        if (!detail::Finite(desired) || !detail::Finite(punch)) return {};
        const double pitch = double(desired.x) + (recoilCorrection ? 0. : double(punch.x));
        if (std::abs(pitch) > std::numeric_limits<float>::max()) return {};
        const Vec3 physical{
            static_cast<float>(pitch),
            static_cast<float>(std::remainder(double(desired.y) + (recoilCorrection ? 0. : double(punch.y)), 360.)),
            desired.z
        };
        return detail::Finite(physical) ? std::optional<Vec3>{physical} : std::nullopt;
    }

    inline std::optional<Vec3> RemovePunch(Vec3 physicalShot, Vec3 punch) noexcept
    {
        if (!detail::Finite(physicalShot) || !detail::Finite(punch)) return {};
        const double pitch = double(physicalShot.x) - punch.x;
        if (std::abs(pitch) > 89.) return {};
        const Vec3 command{
            static_cast<float>(pitch),
            static_cast<float>(std::remainder(double(physicalShot.y) - punch.y, 360.)),
            physicalShot.z
        };
        if (!detail::Finite(command) || std::abs(command.x) > 89.f) return {};
        return command;
    }

    struct ShotBasis { detail::DVec3 forward, left, up; };
    inline std::optional<ShotBasis> MakeShotBasis(Vec3 angles) noexcept
    {
        using namespace detail;
        if (!Finite(angles)) return {};
        const double pitch = std::remainder(double(angles.x), 360.) * kRadians;
        const double yaw = std::remainder(double(angles.y), 360.) * kRadians;
        const double roll = std::remainder(double(angles.z), 360.) * kRadians;
        const double sp = std::sin(pitch), cp = std::cos(pitch), sy = std::sin(yaw), cy = std::cos(yaw);
        const double sr = std::sin(roll), cr = std::cos(roll);
        const DVec3 left{-sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp};
        const DVec3 up{cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp};
        return ShotBasis{Forward(angles),left,up};
    }
    // Source uses forward + left * spread.x + up * spread.y, including roll.
    // A confidence scan reuses this immutable basis for all spread seeds.
    inline std::optional<Vec3> DirectionWithBasis(const ShotBasis& basis, Vec2 spread) noexcept
    {
        using namespace detail;
        if (!std::isfinite(spread.x) || !std::isfinite(spread.y)) return {};
        DVec3 direction = basis.forward + basis.left * spread.x + basis.up * spread.y;
        if (!Normalize(direction)) return {};
        return Vec3{static_cast<float>(direction.x), static_cast<float>(direction.y), static_cast<float>(direction.z)};
    }
    inline std::optional<Vec3> ShotDirection(Vec3 angles, Vec2 spread) noexcept
    {
        const auto basis=MakeShotBasis(angles);
        return basis?DirectionWithBasis(*basis,spread):std::nullopt;
    }

    inline std::optional<Vec3> CorrectSpread(Vec3 desiredAim, Vec2 spread) noexcept
    {
        using namespace detail;
        if (!Finite(desiredAim) || desiredAim.x < -89 || desiredAim.x > 89 ||
            !std::isfinite(spread.x) || !std::isfinite(spread.y)) return {};
        const double pitch = desiredAim.x + std::atan(std::hypot(double(spread.x), double(spread.y))) * kDegrees;
        // Clamping a solved angle changes the shot direction and its native seed.
        if (pitch < -89 || pitch > 89) return {};
        const Vec3 corrected{static_cast<float>(pitch), static_cast<float>(std::remainder(double(desiredAim.y), 360.)),
                             static_cast<float>(-std::atan2(double(spread.x), double(spread.y)) * kDegrees)};
        const auto shot = ShotDirection(corrected, spread);
        if (!shot) return {};
        DVec3 actual = Convert(*shot);
        if (!Normalize(actual) || Dot(actual, Forward(desiredAim)) < 1. - 1e-10) return {};
        return corrected;
    }

    // Try only seed-consistent fixed points, without assumptions about native
    // seed quantization. Each mismatch follows the seed of the corrected angle;
    // cycles, missing native data, and exhaustion return no solution. At most
    // attempts samples and attempts+1 seed calls are made. The caller can impose
    // a shared native-call budget through these callbacks.
    template<class Seed, class Sample>
    std::optional<Vec3> FindSpreadCorrectionIterative(Vec3 desiredAim, int tick, Seed&& seed, Sample&& sample,
                                                      int attempts = 8)
    {
        if (!detail::Finite(desiredAim) || desiredAim.x < -89 || desiredAim.x > 89 ||
            tick < 0 || attempts <= 0 || attempts > 32) return {};
        desiredAim.y = static_cast<float>(std::remainder(double(desiredAim.y), 360.));
        auto candidateSeed = seed(desiredAim, tick);
        if (!candidateSeed) return {};
        std::uint32_t visited[32]{};
        int visitedCount{};
        for (int i = 0; i < attempts; ++i)
        {
            for (int previous = 0; previous < visitedCount; ++previous)
                if (visited[previous] == *candidateSeed) return {};
            visited[visitedCount++] = *candidateSeed;
            const auto spread = sample(*candidateSeed);
            if (!spread) return {};
            const auto corrected = CorrectSpread(desiredAim, *spread);
            if (!corrected) return {};
            const auto correctedSeed = seed(*corrected, tick);
            if (!correctedSeed) return {};
            if (*correctedSeed == *candidateSeed) return corrected;
            candidateSeed = correctedSeed;
        }
        return {};
    }

    // Native callbacks supply the angle/tick seed and sample for that seed. The
    // native sampler owns its seed+1 convention and safe per-pellet output arrays.
    // The final angle must reproduce the sampled seed; a zero angle is valid.
    template<class Seed, class Sample>
    std::optional<Vec3> FindSpreadCorrection(Vec3 desiredAim, int tick, Seed&& seed, Sample&& sample,
                                             int attempts = 720)
    {
        if (!detail::Finite(desiredAim) || desiredAim.x < -89 || desiredAim.x > 89 ||
            tick < 0 || attempts <= 0 || attempts > 720) return {};
        desiredAim.y = static_cast<float>(std::remainder(double(desiredAim.y), 360.));
        for (int i = 0; i < attempts; ++i)
        {
            const auto candidateSeed = seed(Vec3{static_cast<float>(i) * .5f, desiredAim.y, 0}, tick);
            if (!candidateSeed) continue;
            const auto spread = sample(*candidateSeed);
            if (!spread) continue;
            const auto corrected = CorrectSpread(desiredAim, *spread);
            if (!corrected) continue;
            const auto correctedSeed = seed(*corrected, tick);
            if (correctedSeed && *correctedSeed == *candidateSeed) return corrected;
        }
        return {};
    }
}
