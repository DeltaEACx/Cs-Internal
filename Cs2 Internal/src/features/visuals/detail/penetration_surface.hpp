#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>

namespace visuals::detail::penetration_surface
{
    struct Vec2 { float x{}, y{}; };
    struct Vec3 { float x{}, y{}, z{}; };
    struct Camera { float matrix[4][4]{}; Vec3 origin{}; };
    struct Key
    {
        std::uint64_t generation{}, localHandle{}, weaponHandle{}, ballistics{};
        bool operator==(const Key&) const noexcept = default;
    };
    struct Frame { Key key{}; Camera camera{}; Vec3 eye{}; std::uint64_t nowMs{}; };
    struct Identity
    {
        // Opaque native physics tokens, never dereferenced by this policy.
        // A material pointer alone is not a surface identity.
        std::uint64_t object{}, shape{};
        std::uint32_t ownerHandle{};
        std::uint8_t kind{};
        bool valid{};
        bool operator==(const Identity&) const noexcept = default;
    };
    enum class State : std::uint8_t { Unknown, Clear, Blocked, Pass };
    struct Sample
    {
        State state{State::Unknown};
        Identity identity{};
        Vec3 position{}, normal{};
        Vec2 ndc{};
        Frame frame{};
    };
    struct Limits
    {
        std::uint64_t ttlMs{150};
        float maxCameraShift{.25f}, maxEyeShift{.25f}, maxNdcError{.005f};
        float minNormalDot{.985f}, maxPlaneDistance{1.f}, maxEdge{512.f}, offset{.05f};
    };
    struct Patch
    {
        // TL, TR, BR, BL, center. Draw the four triangles
        // {points[i], points[(i+1)%4], points[4]} with the current view matrix.
        std::array<Vec3, 5> points{};
        Vec3 normal{};
    };

    namespace policy_detail
    {
        struct D3
        {
            double x{}, y{}, z{};
            D3 operator+(D3 v) const noexcept { return {x + v.x, y + v.y, z + v.z}; }
            D3 operator-(D3 v) const noexcept { return {x - v.x, y - v.y, z - v.z}; }
            D3 operator*(double v) const noexcept { return {x * v, y * v, z * v}; }
        };
        inline D3 Convert(Vec3 v) noexcept { return {v.x, v.y, v.z}; }
        inline Vec3 Convert(D3 v) noexcept
        { return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)}; }
        inline double Dot(D3 a, D3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
        inline D3 Cross(D3 a, D3 b) noexcept
        { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
        inline bool World(Vec3 v) noexcept
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
                   std::abs(v.x) <= 1e7f && std::abs(v.y) <= 1e7f && std::abs(v.z) <= 1e7f;
        }
        inline bool Ndc(Vec2 v) noexcept
        { return std::isfinite(v.x) && std::isfinite(v.y) && std::abs(v.x) <= 1 && std::abs(v.y) <= 1; }
        inline bool Normalize(D3& v) noexcept
        {
            const double squared = Dot(v, v);
            if (!std::isfinite(squared) || squared <= 1e-16) return false;
            v = v * (1. / std::sqrt(squared));
            return true;
        }
        inline bool Near(Vec2 a, Vec2 b, double tolerance) noexcept
        { return std::hypot(double(a.x) - b.x, double(a.y) - b.y) <= tolerance; }
        struct Basis { D3 a, b, c, bc, ca, ab, origin; double det{}; };
        inline std::optional<Basis> ReadBasis(const float (&m)[4][4]) noexcept
        {
            for (const auto& row : m)
                for (float v : row)
                    if (!std::isfinite(v)) return std::nullopt;
            Basis q;
            q.a = {m[0][0], m[0][1], m[0][2]};
            q.b = {m[1][0], m[1][1], m[1][2]};
            q.c = {m[3][0], m[3][1], m[3][2]};
            q.bc = Cross(q.b, q.c); q.ca = Cross(q.c, q.a); q.ab = Cross(q.a, q.b);
            q.det = Dot(q.a, q.bc);
            const double scale = std::sqrt(Dot(q.a, q.a) * Dot(q.b, q.b) * Dot(q.c, q.c));
            if (!std::isfinite(scale) || scale <= 0 || std::abs(q.det) <= scale * 1e-9)
                return std::nullopt;
            q.origin = (q.bc * -m[0][3] + q.ca * -m[1][3] + q.ab * -m[3][3]) * (1. / q.det);
            if (!World(Convert(q.origin))) return std::nullopt;
            return q;
        }
        inline bool MatchesOrigin(const Camera& camera, const Basis& basis) noexcept
        {
            if (!World(camera.origin)) return false;
            // Camera.origin is a convenience copy, not independent input.
            // Comparing the rounded result also works at large map coordinates.
            const Vec3 expected = Convert(basis.origin);
            return expected.x == camera.origin.x && expected.y == camera.origin.y &&
                   expected.z == camera.origin.z;
        }
        inline bool Valid(const Camera& camera) noexcept
        {
            const auto basis = ReadBasis(camera.matrix);
            return basis && MatchesOrigin(camera, *basis);
        }
        inline std::optional<Vec3> Direction(const Basis& basis, Vec2 ndc) noexcept
        {
            D3 ray = (basis.bc * ndc.x + basis.ca * ndc.y + basis.ab) * (1. / basis.det);
            if (!Normalize(ray) || Dot(ray, basis.c) <= 0) return std::nullopt;
            return Convert(ray);
        }
        inline std::optional<Vec2> Project(const Camera& camera, Vec3 world) noexcept
        {
            const auto row = [&](int i)
            {
                return double(camera.matrix[i][0]) * world.x + double(camera.matrix[i][1]) * world.y +
                       double(camera.matrix[i][2]) * world.z + camera.matrix[i][3];
            };
            const double x = row(0), y = row(1), w = row(3);
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || w < .01) return std::nullopt;
            const Vec2 ndc{static_cast<float>(x / w), static_cast<float>(y / w)};
            if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y)) return std::nullopt;
            return ndc;
        }
        inline bool Valid(const Limits& q) noexcept
        {
            return q.ttlMs > 0 && q.ttlMs <= 1000 &&
                   std::isfinite(q.maxCameraShift) && q.maxCameraShift >= 0 && q.maxCameraShift <= 16 &&
                   std::isfinite(q.maxEyeShift) && q.maxEyeShift >= 0 && q.maxEyeShift <= 16 &&
                   std::isfinite(q.maxNdcError) && q.maxNdcError >= 0 && q.maxNdcError <= .05f &&
                   std::isfinite(q.minNormalDot) && q.minNormalDot >= .9f && q.minNormalDot <= 1 &&
                   std::isfinite(q.maxPlaneDistance) && q.maxPlaneDistance >= 0 && q.maxPlaneDistance <= 16 &&
                   std::isfinite(q.maxEdge) && q.maxEdge > 0 && q.maxEdge <= 4096 &&
                   std::isfinite(q.offset) && q.offset >= 0 && q.offset <= 1;
        }
    }

    // Perspective origin and rays use only clip x/y/w (rows 0/1/3).
    // No reversed-Z/depth convention or separately sampled view angle is assumed.
    inline std::optional<Camera> MakeCamera(const float (&matrix)[4][4]) noexcept
    {
        const auto basis = policy_detail::ReadBasis(matrix);
        if (!basis) return std::nullopt;
        Camera camera;
        for (std::size_t row = 0; row < 4; ++row)
            std::copy(std::begin(matrix[row]), std::end(matrix[row]), camera.matrix[row]);
        camera.origin = policy_detail::Convert(basis->origin);
        return camera;
    }
    inline std::optional<Vec3> Direction(const Camera& camera, Vec2 ndc) noexcept
    {
        using namespace policy_detail;
        const auto basis = ReadBasis(camera.matrix);
        if (!Ndc(ndc) || !basis || !MatchesOrigin(camera, *basis)) return std::nullopt;
        return policy_detail::Direction(*basis, ndc);
    }
    inline std::optional<Vec2> Project(const Camera& camera, Vec3 world) noexcept
    {
        using namespace policy_detail;
        if (!World(world) || !Valid(camera)) return std::nullopt;
        return policy_detail::Project(camera, world);
    }

    // A cached hit must still lie on the same current screen ray and shot origin.
    // Generation/serial/stat changes invalidate the sample before geometry tests.
    inline bool Fresh(const Sample& sample, const Frame& current, const Limits& limits = {}) noexcept
    {
        using namespace policy_detail;
        if (!Valid(limits) || !(sample.frame.key == current.key) ||
            sample.frame.nowMs > current.nowMs || current.nowMs - sample.frame.nowMs > limits.ttlMs ||
            !Ndc(sample.ndc) || !World(sample.position) || !World(sample.frame.eye) || !World(current.eye)) return false;
        const auto oldBasis = ReadBasis(sample.frame.camera.matrix), newBasis = ReadBasis(current.camera.matrix);
        if (!oldBasis || !newBasis || !MatchesOrigin(sample.frame.camera, *oldBasis) ||
            !MatchesOrigin(current.camera, *newBasis)) return false;
        const auto within = [](Vec3 a, Vec3 b, double maximum)
        { const D3 d = Convert(a) - Convert(b); return Dot(d, d) <= maximum * maximum; };
        if (!within(sample.frame.eye, current.eye, limits.maxEyeShift) ||
            !within(sample.frame.camera.origin, current.camera.origin, limits.maxCameraShift)) return false;
        const auto oldRay = policy_detail::Direction(*oldBasis, sample.ndc), newRay = policy_detail::Direction(*newBasis, sample.ndc);
        if (!oldRay || !newRay || !within(*oldRay, *newRay, limits.maxNdcError)) return false;
        const auto oldNdc = policy_detail::Project(sample.frame.camera, sample.position), nowNdc = policy_detail::Project(current.camera, sample.position);
        return oldNdc && nowNdc && Near(*oldNdc, sample.ndc, limits.maxNdcError) &&
               Near(*nowNdc, sample.ndc, limits.maxNdcError);
    }

    // Only an adjacent grid cell is joined. A passing center is mandatory;
    // blocked/unknown centers never borrow four thin passing corner hits.
    // The result is sampled local coverage, never a whole-entity/mesh verdict.
    inline std::optional<Patch> BuildCell(const std::array<Sample, 4>& corners, const Sample& center,
                                          const Frame& current, const Limits& limits = {}) noexcept
    {
        using namespace policy_detail;
        if (!Valid(limits) || center.state != State::Pass || !center.identity.valid ||
            !(center.identity.object || center.identity.shape) || !Fresh(center, current, limits)) return std::nullopt;
        constexpr double gridEpsilon = 1e-5;
        const auto& a = corners[0].ndc; const auto& b = corners[1].ndc;
        const auto& c = corners[2].ndc; const auto& d = corners[3].ndc;
        if (!(a.x < b.x && a.y > d.y) || std::abs(double(a.y) - b.y) > gridEpsilon ||
            std::abs(double(b.x) - c.x) > gridEpsilon || std::abs(double(c.y) - d.y) > gridEpsilon ||
            std::abs(double(d.x) - a.x) > gridEpsilon ||
            !Near(center.ndc, {(a.x + b.x) * .5f, (a.y + d.y) * .5f}, gridEpsilon)) return std::nullopt;

        const auto facing = [&](const Sample& s) -> std::optional<D3>
        {
            D3 n = Convert(s.normal);
            if (!Normalize(n)) return std::nullopt;
            if (Dot(n, Convert(s.position) - Convert(current.camera.origin)) > 0) n = n * -1.;
            return n;
        };
        const auto normal = facing(center);
        if (!normal) return std::nullopt;
        const D3 mid = Convert(center.position);
        std::array<D3, 4> points{};
        for (std::size_t i = 0; i < corners.size(); ++i)
        {
            const auto& sample = corners[i];
            if (sample.state != State::Pass || !(sample.identity == center.identity) ||
                !Fresh(sample, current, limits)) return std::nullopt;
            const auto n = facing(sample);
            points[i] = Convert(sample.position);
            const D3 delta = points[i] - mid;
            if (!n || Dot(*n, *normal) < limits.minNormalDot ||
                std::abs(Dot(delta, *normal)) > limits.maxPlaneDistance ||
                std::abs(Dot(delta, *n)) > limits.maxPlaneDistance ||
                Dot(delta, delta) > double(limits.maxEdge) * limits.maxEdge) return std::nullopt;
        }
        double winding{};
        for (std::size_t i = 0; i < points.size(); ++i)
        {
            const D3 edge = points[(i + 1) % 4] - points[i];
            const D3 next = points[(i + 2) % 4] - points[(i + 1) % 4];
            const double lengthSquared = Dot(edge, edge);
            const double centerSide = Dot(Cross(edge, mid - points[i]), *normal);
            if (i == 0) winding = centerSide > 0 ? 1. : -1.;
            // Reject folds, concavity, a center outside the quad, and vanishing area.
            // TL/TR/BR/BL world winding changes with camera handedness. Retain
            // its screen order and camera-facing normal, requiring consistency
            // rather than assuming the x/y/w basis has positive determinant.
            if (lengthSquared <= 1e-10 || lengthSquared > double(limits.maxEdge) * limits.maxEdge ||
                centerSide * winding <= 1e-8 ||
                Dot(Cross(edge, next), *normal) * winding <= 1e-8) return std::nullopt;
            // Pairwise plane consistency prevents accumulated normal differences.
            for (std::size_t j = i + 1; j < points.size(); ++j)
                if (std::abs(Dot(points[j] - points[i], *normal)) > limits.maxPlaneDistance) return std::nullopt;
        }

        Patch patch;
        patch.normal = Convert(*normal);
        const auto shifted = [&](D3 v) -> std::optional<Vec3>
        {
            const Vec3 point = Convert(v + *normal * limits.offset);
            if (!World(point)) return std::nullopt;
            const double actualOffset = Dot(Convert(point) - v, *normal);
            if (limits.offset > 0 && (actualOffset < limits.offset * .5 || actualOffset > limits.offset * 1.5))
                return std::nullopt;
            return point;
        };
        for (std::size_t i = 0; i < 5; ++i)
        {
            const auto point = shifted(i < 4 ? points[i] : mid);
            if (!point) return std::nullopt;
            patch.points[i] = *point;
        }
        return patch;
    }

    // Fixed grid allocation guard; the runtime may query these positions over
    // several bounded native frames, without treating an incomplete cell as Pass.
    inline constexpr std::size_t PlannedSamples(std::size_t columns, std::size_t rows) noexcept
    {
        return columns && rows && columns <= 64 && rows <= 64
            ? (columns + 1) * (rows + 1) + columns * rows : 0;
    }
}
