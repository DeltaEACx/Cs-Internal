#pragma once
#include "surface_square.hpp"
#include <algorithm>
#include <cstddef>

namespace visuals::detail::surface_square
{
    struct Point
    {
        float x{}, y{};
    };
    struct Polygon
    {
        std::array<Point, 12> points{};
        std::size_t count{};
    };

    namespace projection_detail
    {
        inline constexpr double kNearW = static_cast<double>(0.01f);
        struct ClipPoint { double x{}, y{}, w{}; };
        inline double Distance(ClipPoint point, int plane) noexcept
        {
            switch (plane)
            {
            case 0: return point.w - kNearW;
            case 1: return point.x + point.w;
            case 2: return point.w - point.x;
            case 3: return point.y + point.w;
            default: return point.w - point.y;
            }
        }
        inline bool Same(ClipPoint a, ClipPoint b) noexcept
        {
            return a.x == b.x && a.y == b.y && a.w == b.w;
        }
    }

    // Matrix rows 0/1/3 produce clip x/y/w. Clip before division, using only
    // the near-w plane and viewport sides; the engine's depth convention is unused.
    // A quad needs at most nine vertices after these five convex clipping planes.
    // The output remains unchanged if no drawable polygon survives.
    inline bool Project(const Corners& corners, const float (&matrix)[4][4],
                        float width, float height, Polygon& out) noexcept
    {
        using namespace projection_detail;
        if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) return false;
        for (const auto& row : matrix)
            for (float value : row)
                if (!std::isfinite(value)) return false;

        std::array<ClipPoint, 12> current{}, next{};
        std::size_t count = corners.size();
        for (std::size_t i = 0; i < corners.size(); ++i)
        {
            const auto point = corners[i];
            if (!math_detail::Finite(point)) return false;
            auto transform = [&](int row)
            {
                return double(matrix[row][0]) * point.x + double(matrix[row][1]) * point.y +
                       double(matrix[row][2]) * point.z + matrix[row][3];
            };
            current[i] = {transform(0), transform(1), transform(3)};
        }

        for (int plane = 0; plane < 5; ++plane)
        {
            if (count < 3) return false;
            std::size_t nextCount = 0;
            auto append = [&](ClipPoint point)
            {
                if (nextCount && Same(next[nextCount - 1], point)) return true;
                if (nextCount == next.size()) return false;
                next[nextCount++] = point;
                return true;
            };
            ClipPoint previous = current[count - 1];
            double previousDistance = Distance(previous, plane);
            bool previousInside = previousDistance >= 0;
            for (std::size_t i = 0; i < count; ++i)
            {
                const ClipPoint point = current[i];
                const double distance = Distance(point, plane);
                const bool inside = distance >= 0;
                if (inside != previousInside)
                {
                    const double amount = previousDistance / (previousDistance - distance);
                    ClipPoint intersection{
                        previous.x * (1. - amount) + point.x * amount,
                        previous.y * (1. - amount) + point.y * amount,
                        previous.w * (1. - amount) + point.w * amount
                    };
                    // Put the intersection exactly on its clipping plane.
                    // This also avoids a near-plane rounding error at w=0.01.
                    switch (plane)
                    {
                    case 0: intersection.w = kNearW; break;
                    case 1: intersection.x = -intersection.w; break;
                    case 2: intersection.x = intersection.w; break;
                    case 3: intersection.y = -intersection.w; break;
                    default: intersection.y = intersection.w; break;
                    }
                    if (!append(intersection)) return false;
                }
                if (inside && !append(point)) return false;
                previous = point;
                previousDistance = distance;
                previousInside = inside;
            }
            if (nextCount > 1 && Same(next[0], next[nextCount - 1])) --nextCount;
            current = next;
            count = nextCount;
        }
        if (count < 3) return false;

        Polygon candidate;
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto point = current[i];
            if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
                !std::isfinite(point.w) || point.w < kNearW) return false;
            const double x = std::clamp(point.x / point.w, -1., 1.);
            const double y = std::clamp(point.y / point.w, -1., 1.);
            const Point screen{static_cast<float>((x + 1.) * .5 * width),
                               static_cast<float>((1. - y) * .5 * height)};
            if (!std::isfinite(screen.x) || !std::isfinite(screen.y)) return false;
            if (candidate.count && candidate.points[candidate.count - 1].x == screen.x &&
                                   candidate.points[candidate.count - 1].y == screen.y) continue;
            candidate.points[candidate.count++] = screen;
        }
        if (candidate.count > 1 && candidate.points[0].x == candidate.points[candidate.count - 1].x &&
                                  candidate.points[0].y == candidate.points[candidate.count - 1].y)
            --candidate.count;
        if (candidate.count < 3) return false;
        double area = 0;
        const auto origin = candidate.points[0];
        for (std::size_t i = 1; i + 1 < candidate.count; ++i)
        {
            const auto a = candidate.points[i], b = candidate.points[i + 1];
            area += (double(a.x) - origin.x) * (double(b.y) - origin.y) -
                    (double(a.y) - origin.y) * (double(b.x) - origin.x);
        }
        if (!std::isfinite(area) || area == 0) return false;
        out = candidate;
        return true;
    }
}
