#include "features/visuals/detail/surface_square.hpp"
#include "features/visuals/detail/surface_square_projection.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace square = visuals::detail::surface_square;
using square::Vec3;
using square::Corners;

namespace
{
    void Check(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }
    Vec3 Add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    Vec3 Scale(Vec3 a, float b) { return {a.x * b, a.y * b, a.z * b}; }
    double Dot(Vec3 a, Vec3 b) { return double(a.x) * b.x + double(a.y) * b.y + double(a.z) * b.z; }
    Vec3 Cross(Vec3 a, Vec3 b)
    {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }
    double Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
    Vec3 Unit(Vec3 a) { return Scale(a, float(1. / Length(a))); }
    bool Near(double a, double b, double tolerance = 1e-4) { return std::abs(a - b) <= tolerance; }
    bool Same(const Corners& a, const Corners& b)
    {
        for (std::size_t i = 0; i < a.size(); ++i)
            if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) return false;
        return true;
    }

    Corners Verify(Vec3 hit, Vec3 normal, Vec3 aim, float side, double tolerance = 1e-4)
    {
        Corners result;
        Check(square::Build(hit, normal, aim, side, result), "valid surface rejected");
        normal = Unit(normal);
        if (Dot(normal, aim) > 0) normal = Scale(normal, -1);
        Vec3 center{};
        for (std::size_t i = 0; i < result.size(); ++i)
        {
            center = Add(center, Scale(result[i], .25f));
            Check(std::isfinite(result[i].x) && std::isfinite(result[i].y) && std::isfinite(result[i].z),
                  "corner is not finite");
            Check(Near(Dot(Sub(result[i], hit), normal), square::kSurfaceOffset, tolerance),
                  "corner left surface plane or has wrong offset");
            Check(Near(Length(Sub(result[(i + 1) % 4], result[i])), side, tolerance), "wrong side length");
        }
        Check(Length(Sub(center, Add(hit, Scale(normal, square::kSurfaceOffset)))) <= tolerance * 2,
              "square center differs from hit position plus offset");
        const Vec3 edge0 = Sub(result[1], result[0]), edge1 = Sub(result[2], result[1]);
        Check(Near(Dot(edge0, edge1), 0, tolerance * side * 2), "square edges are not orthogonal");
        Check(Near(Length(Sub(result[2], result[0])), side * std::sqrt(2.), tolerance * 2),
              "wrong diagonal length");
        Check(Dot(Cross(edge0, edge1), normal) > 0, "winding faces into the surface");
        Check(Near(Length(Cross(edge0, edge1)), double(side) * side, tolerance * side * 2),
              "wrong square area");
        return result;
    }

    void Surfaces()
    {
        const Vec3 hit{10, 20, 30};
        for (Vec3 normal : {Vec3{1, 0, 0}, Vec3{-1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, -1, 0}})
        {
            const auto corners = Verify(hit, normal, Scale(normal, -1), 8);
            Check(Near(corners[0].z, hit.z - 4) && Near(corners[2].z, hit.z + 4),
                  "wall square is not aligned with world vertical");
        }
        Verify(hit, {0, 0, 1}, {.3f, .4f, -1}, 8);
        Verify(hit, {0, 0, -1}, {.3f, .4f, 1}, 8);
        Verify(hit, {0, 0, 1}, {0, 0, -1}, 8);
        Verify(hit, {0, 0, -1}, {0, 0, 1}, 8);
        Verify(hit, {0, 0, 1}, {1, 0, 0}, 8);
        for (Vec3 normal : {Vec3{.2f, -.4f, 1}, Vec3{1, 2, .3f}, Vec3{-1, -.8f, -.9f}})
            for (float side : {square::kMinSide, 2.f, 8.f, 64.f, square::kMaxSide})
                Verify(hit, normal, Scale(normal, -1), side, 2e-4);
        Verify({10000, -20000, 30000}, {1, 2, 3}, {-1, -2, -3}, 64, .006);
    }

    void StableBasis()
    {
        const Vec3 hit{2, 3, 4};
        const Vec3 aim{.3f, .4f, -1};
        const auto base = Verify(hit, {0, 0, 1}, aim, 8);
        for (Vec3 normal : {Vec3{1e-6f, -1e-6f, 1}, Vec3{-1e-6f, 1e-6f, 1}})
        {
            const auto near = Verify(hit, normal, aim, 8);
            for (std::size_t i = 0; i < near.size(); ++i)
                Check(Length(Sub(near[i], base[i])) < 1e-4, "near-horizontal heading jitters");
        }
        const auto vertical = Verify(hit, {0, 0, 1}, {0, 0, -1}, 8);
        for (Vec3 normal : {Vec3{1e-5f, 0, 1}, Vec3{-1e-5f, 0, 1}, Vec3{0, 1e-5f, 1}})
        {
            const auto near = Verify(hit, normal, {0, 0, -1}, 8);
            for (std::size_t i = 0; i < near.size(); ++i)
                Check(Length(Sub(near[i], vertical[i])) < .001, "vertical floor ray has unstable fallback");
        }
        Corners reversed;
        Check(square::Build(hit, {0, 0, -1}, aim, 8, reversed) && Same(base, reversed),
              "opposite normal does not face incoming aim consistently");
        const auto slope = Verify(hit, {1, -2, .3f}, {-1, 2, -.3f}, 8);
        for (float scale : {.1f, 10.f, 100.f})
        {
            const auto scaled = Verify(hit, Scale({1, -2, .3f}, scale), Scale({-1, 2, -.3f}, scale), 8);
            for (std::size_t i = 0; i < slope.size(); ++i)
                Check(Length(Sub(slope[i], scaled[i])) < 1e-5, "normal or aim magnitude changed geometry");
        }
        Corners repeated;
        Check(square::Build(hit, {1, -2, .3f}, {-1, 2, -.3f}, 8, repeated) && Same(slope, repeated),
              "identical inputs changed corner order");
    }

    void InvalidInputs()
    {
        const Corners sentinel = {Vec3{1, 2, 3}, Vec3{4, 5, 6}, Vec3{7, 8, 9}, Vec3{10, 11, 12}};
        auto reject = [&](Vec3 hit, Vec3 normal, Vec3 aim, float side)
        {
            auto out = sentinel;
            Check(!square::Build(hit, normal, aim, side, out), "invalid input accepted");
            Check(Same(out, sentinel), "failed build changed output");
        };
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        const Vec3 hit{1, 2, 3}, normal{0, 0, 1}, aim{0, 0, -1};
        for (float side : {-1.f, 0.f, square::kMinSide / 2, square::kMaxSide + 1, nan, inf})
            reject(hit, normal, aim, side);
        for (Vec3 bad : {Vec3{}, Vec3{nan, 0, 1}, Vec3{0, inf, 1}, Vec3{0, 0, -inf}, Vec3{1e-20f, 0, 0}})
        {
            reject(hit, bad, aim, 8);
            reject(hit, normal, bad, 8);
        }
        reject({nan, 2, 3}, normal, aim, 8);
        reject({1, inf, 3}, normal, aim, 8);
        reject({std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), 3}, normal, aim, 8);
        reject({0, 0, std::numeric_limits<float>::max()}, normal, aim, 8);
    }

    void Projection()
    {
        const float perspective[4][4] = {
            {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 1, 0}
        };
        const float orthographic[4][4] = {
            {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}
        };
        auto project = [&](const Corners& corners, const float (&matrix)[4][4])
        {
            square::Polygon result;
            Check(square::Project(corners, matrix, 1000, 500, result), "visible polygon rejected");
            Check(result.count >= 3 && result.count <= 9, "wrong clipped polygon count");
            for (std::size_t i = 0; i < result.count; ++i)
            {
                const auto point = result.points[i];
                Check(std::isfinite(point.x) && std::isfinite(point.y) &&
                      point.x >= 0 && point.x <= 1000 && point.y >= 0 && point.y <= 500,
                      "projected point escaped viewport");
            }
            return result;
        };
        auto bounds = [](const square::Polygon& polygon)
        {
            std::array<float, 4> result{1000, 500, 0, 0};
            for (std::size_t i = 0; i < polygon.count; ++i)
            {
                result[0] = std::min(result[0], polygon.points[i].x);
                result[1] = std::min(result[1], polygon.points[i].y);
                result[2] = std::max(result[2], polygon.points[i].x);
                result[3] = std::max(result[3], polygon.points[i].y);
            }
            return result;
        };
        const Corners front = {Vec3{-1, -1, 2}, Vec3{1, -1, 2}, Vec3{1, 1, 2}, Vec3{-1, 1, 2}};
        const auto plain = project(front, perspective);
        Check(plain.count == 4 && Near(plain.points[0].x, 250) && Near(plain.points[0].y, 375) &&
              Near(plain.points[2].x, 750) && Near(plain.points[2].y, 125),
              "perspective division or screen Y is incorrect");

        // This tilted quad crosses the camera/near plane. Its visible part is a
        // finite trapezoid, not four independently projected giant coordinates.
        const Corners near = {Vec3{-.02f, -.004f, -.02f}, Vec3{.02f, -.004f, .06f},
                              Vec3{.02f, .004f, .06f}, Vec3{-.02f, .004f, -.02f}};
        const auto clippedNear = project(near, perspective);
        const auto nearBounds = bounds(clippedNear);
        Check(clippedNear.count == 4 && Near(nearBounds[0], 250, .001) && Near(nearBounds[1], 150, .001) &&
              Near(nearBounds[2], 1000. * 2 / 3, .001) && Near(nearBounds[3], 350, .001),
              "near plane intersection is incorrect");

        const Corners edge = {Vec3{.5f, -.5f, 1}, Vec3{1.5f, -.5f, 1},
                              Vec3{1.5f, .5f, 1}, Vec3{.5f, .5f, 1}};
        const auto clippedEdge = project(edge, perspective);
        const auto edgeBounds = bounds(clippedEdge);
        Check(clippedEdge.count == 4 && Near(edgeBounds[0], 750) && Near(edgeBounds[2], 1000),
              "viewport side intersection is incorrect");
        const Corners huge = {Vec3{-2, -2, 1}, Vec3{2, -2, 1}, Vec3{2, 2, 1}, Vec3{-2, 2, 1}};
        const auto full = project(huge, perspective);
        Check(full.count == 4 && bounds(full) == std::array<float, 4>{0, 0, 1000, 500},
              "four-side clipping did not retain viewport");
        const Corners diamond = {Vec3{0, 1.4f, 1}, Vec3{1.4f, 0, 1}, Vec3{0, -1.4f, 1}, Vec3{-1.4f, 0, 1}};
        Check(project(diamond, perspective).count == 8, "clipped quad did not retain added vertices");

        const Corners negativeDepth = {Vec3{-.5f, -.5f, -5}, Vec3{.5f, -.5f, -5},
                                       Vec3{.5f, .5f, -5}, Vec3{-.5f, .5f, -5}};
        Check(project(negativeDepth, orthographic).count == 4, "projection assumes a native depth convention");
        auto nearBoundary = front;
        for (auto& point : nearBoundary) { point.x *= .001f; point.y *= .001f; point.z = .01f; }
        Check(project(nearBoundary, perspective).count == 4, "exact float near-plane boundary rejected");

        square::Polygon sentinel;
        sentinel.count = 1; sentinel.points[0] = {42, 24};
        auto reject = [&](const Corners& corners, const float (&matrix)[4][4], float width, float height)
        {
            auto out = sentinel;
            Check(!square::Project(corners, matrix, width, height, out), "invalid or hidden polygon accepted");
            Check(out.count == 1 && out.points[0].x == 42 && out.points[0].y == 24,
                  "failed projection changed output");
        };
        auto behind = front;
        for (auto& point : behind) point.z = -1;
        reject(behind, perspective, 1000, 500);
        auto beforeNear = nearBoundary;
        for (auto& point : beforeNear) point.z = .009f;
        reject(beforeNear, perspective, 1000, 500);
        auto offscreen = front;
        for (auto& point : offscreen) point.x += 10;
        reject(offscreen, perspective, 1000, 500);
        const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
        auto invalid = front; invalid[1].x = nan;
        reject(invalid, perspective, 1000, 500);
        invalid = front; invalid[1].y = inf;
        reject(invalid, perspective, 1000, 500);
        for (float value : {0.f, -1.f, nan, inf})
        {
            reject(front, perspective, value, 500);
            reject(front, perspective, 1000, value);
        }
        float invalidMatrix[4][4];
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
            {
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c) invalidMatrix[r][c] = perspective[r][c];
                invalidMatrix[row][column] = nan;
                reject(front, invalidMatrix, 1000, 500);
            }
        const Corners point = {Vec3{0, 0, 1}, Vec3{0, 0, 1}, Vec3{0, 0, 1}, Vec3{0, 0, 1}};
        reject(point, perspective, 1000, 500);
    }
}

int main()
{
    try
    {
        Surfaces();
        StableBasis();
        InvalidInputs();
        Projection();
        std::cout << "Surface square regression: PASS\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Surface square regression: FAIL: " << error.what() << '\n';
        return 1;
    }
}
