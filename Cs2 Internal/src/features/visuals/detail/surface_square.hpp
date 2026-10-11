#pragma once
#include <array>
#include <cmath>

namespace visuals::detail::surface_square
{
    struct Vec3
    {
        float x{}, y{}, z{};
    };

    using Corners = std::array<Vec3, 4>;
    inline constexpr float kMinSide = 0.25f;
    inline constexpr float kMaxSide = 128.f;
    inline constexpr float kSurfaceOffset = 0.05f;

    namespace math_detail
    {
        struct DVec3
        {
            double x{}, y{}, z{};
            DVec3 operator+(DVec3 rhs) const noexcept { return {x + rhs.x, y + rhs.y, z + rhs.z}; }
            DVec3 operator-(DVec3 rhs) const noexcept { return {x - rhs.x, y - rhs.y, z - rhs.z}; }
            DVec3 operator*(double value) const noexcept { return {x * value, y * value, z * value}; }
        };

        inline DVec3 Convert(Vec3 value) noexcept { return {value.x, value.y, value.z}; }
        inline bool Finite(Vec3 value) noexcept
        {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }
        inline double Dot(DVec3 a, DVec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
        inline DVec3 Cross(DVec3 a, DVec3 b) noexcept
        {
            return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
        }
        inline bool Normalize(DVec3& value) noexcept
        {
            const double squared = Dot(value, value);
            if (!std::isfinite(squared) || squared <= 1e-12) return false;
            value = value * (1. / std::sqrt(squared));
            return true;
        }
    }

    // Corners wind counterclockwise around the normal facing the incoming ray.
    // Wall edges follow world vertical; nearly horizontal planes follow the ray
    // heading. A nearly vertical ray uses world Y to avoid a noisy floor heading.
    // The output remains unchanged on failure. Projection/clipping is the caller's job.
    inline bool Build(Vec3 hit, Vec3 normal, Vec3 incomingAim,
                      float sideLength, Corners& out) noexcept
    {
        using namespace math_detail;
        if (!Finite(hit) || !Finite(normal) || !Finite(incomingAim) ||
            !std::isfinite(sideLength) || sideLength < kMinSide || sideLength > kMaxSide) return false;

        DVec3 n = Convert(normal), aim = Convert(incomingAim);
        if (!Normalize(n) || !Normalize(aim)) return false;
        if (Dot(n, aim) > 0.) n = n * -1.;

        DVec3 up;
        if (std::abs(n.z) < 0.98)
        {
            up = DVec3{0., 0., 1.} - n * n.z;
        }
        else
        {
            up = aim - n * Dot(aim, n);
            if (Dot(up, up) < 1e-4)
                up = DVec3{0., 1., 0.} - n * n.y;
        }
        if (!Normalize(up)) return false;
        DVec3 right = Cross(up, n);
        if (!Normalize(right)) return false;
        // Reconstruct up so rounding in a nearly parallel projection cannot
        // make the two square edges slightly nonorthogonal.
        up = Cross(n, right);

        const DVec3 origin = Convert(hit);
        const DVec3 center = origin + n * kSurfaceOffset;
        const double half = static_cast<double>(sideLength) * .5;
        const std::array<DVec3, 4> points = {
            center - right * half - up * half,
            center + right * half - up * half,
            center + right * half + up * half,
            center - right * half + up * half
        };
        Corners candidate;
        for (std::size_t i = 0; i < points.size(); ++i)
        {
            candidate[i] = {static_cast<float>(points[i].x), static_cast<float>(points[i].y),
                            static_cast<float>(points[i].z)};
            if (!Finite(candidate[i])) return false;
            // Reject coordinates whose float precision erases the offset.
            const double offset = Dot(Convert(candidate[i]) - origin, n);
            if (offset < kSurfaceOffset * .5 || offset > kSurfaceOffset * 1.5) return false;
        }
        for (std::size_t i = 0; i < candidate.size(); ++i)
        {
            const DVec3 edge = Convert(candidate[(i + 1) % candidate.size()]) - Convert(candidate[i]);
            const double squared = Dot(edge, edge);
            const double expected = static_cast<double>(sideLength) * sideLength;
            // Normal map coordinates have ample precision. Extremely large
            // finite coordinates can collapse a small square after float conversion.
            if (squared < expected * .96 || squared > expected * 1.04) return false;
        }
        out = candidate;
        return true;
    }
}
