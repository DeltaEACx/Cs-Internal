#include "features/visuals/detail/penetration_surface.hpp"
#include "features/visuals/detail/surface_square_projection.hpp"
#include "features/visuals/detail/penetrable_props_cache.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace surface = visuals::detail::penetration_surface;
namespace square = visuals::detail::surface_square;
namespace props = visuals::detail::penetrable_props;
using surface::Vec2;
using surface::Vec3;

namespace
{
    void Check(bool value, const char* message)
    { if (!value) throw std::runtime_error(message); }
    Vec3 Add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    Vec3 Scale(Vec3 a, double x)
    { return {float(a.x * x), float(a.y * x), float(a.z * x)}; }
    double Dot(Vec3 a, Vec3 b) { return double(a.x) * b.x + double(a.y) * b.y + double(a.z) * b.z; }
    Vec3 Cross(Vec3 a, Vec3 b)
    { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
    double Length(Vec3 v) { return std::sqrt(Dot(v, v)); }
    Vec3 Unit(Vec3 v) { return Scale(v, 1 / Length(v)); }
    bool Close(Vec3 a, Vec3 b, double e = 1e-4) { return Length(Sub(a, b)) <= e; }
    bool Close(Vec2 a, Vec2 b, double e = 1e-4)
    { return std::hypot(double(a.x) - b.x, double(a.y) - b.y) <= e; }

    struct Fixture
    {
        Vec3 origin{13, -31, 9}, forward{0, 0, 1}, right{1, 0, 0}, up{0, 1, 0};
        float sx{1.4f}, sy{1.9f}, biasX{}, biasY{};
        surface::Frame frame;

        Fixture()
        { frame.key = {1, 0x12340001, 0x98760002, 0x445566}; frame.eye = origin; frame.nowMs = 1000; Refresh(); }
        void Refresh(float globalScale = 1)
        {
            float matrix[4][4]{};
            const Vec3 row0 = Add(Scale(right, sx), Scale(forward, biasX));
            const Vec3 row1 = Add(Scale(up, sy), Scale(forward, biasY));
            const auto row = [&](int i, Vec3 basis)
            {
                matrix[i][0] = basis.x * globalScale; matrix[i][1] = basis.y * globalScale;
                matrix[i][2] = basis.z * globalScale; matrix[i][3] = float(-Dot(basis, origin)) * globalScale;
            };
            row(0, row0); row(1, row1); row(3, forward);
            // A reversed-Z style row, deliberately irrelevant to clip x/y/w.
            matrix[2][2] = -.0001f; matrix[2][3] = .1f;
            auto camera = surface::MakeCamera(matrix);
            Check(bool(camera), "known perspective camera rejected");
            frame.camera = *camera;
        }
        // This independent physical-camera construction does not use the helper's inverse.
        Vec3 Ray(Vec2 ndc) const
        { return Unit(Add(forward, Add(Scale(right, (ndc.x - biasX) / sx), Scale(up, (ndc.y - biasY) / sy)))); }
        surface::Sample Hit(Vec2 ndc, Vec3 planePoint, Vec3 normal, surface::State state = surface::State::Pass) const
        {
            const Vec3 ray = Ray(ndc);
            const double t = Dot(Sub(planePoint, origin), normal) / Dot(ray, normal);
            Check(t > 0, "fixture plane not in front");
            surface::Sample s;
            s.state = state; s.identity = {0x101010, 0x202020, 0xffffffffu, 1, true};
            s.position = Add(origin, Scale(ray, t)); s.normal = normal; s.ndc = ndc; s.frame = frame;
            return s;
        }
    };
    struct Cell
    {
        std::array<surface::Sample, 4> corners;
        surface::Sample center;
    };
    Cell PlaneCell(const Fixture& f, Vec3 planePoint, Vec3 normal,
                   Vec2 lo = {-.1f, -.1f}, Vec2 hi = {.1f, .1f})
    {
        return {{{f.Hit({lo.x, hi.y}, planePoint, normal), f.Hit({hi.x, hi.y}, planePoint, normal),
                  f.Hit({hi.x, lo.y}, planePoint, normal), f.Hit({lo.x, lo.y}, planePoint, normal)}},
                f.Hit({(lo.x + hi.x) * .5f, (lo.y + hi.y) * .5f}, planePoint, normal)};
    }
    void VerifyPatch(const Cell& cell, const surface::Frame& frame)
    {
        const auto patch = surface::BuildCell(cell.corners, cell.center, frame);
        Check(bool(patch), "same-plane five-pass cell rejected");
        double winding{};
        Check(Dot(patch->normal, Sub(cell.center.position, frame.camera.origin)) < 0,
              "patch normal does not face the camera");
        for (std::size_t i = 0; i < 4; ++i)
        {
            const Vec3 edge = Sub(patch->points[(i + 1) % 4], patch->points[i]);
            const Vec3 center = Sub(patch->points[4], patch->points[i]);
            const double side = Dot(Cross(edge, center), patch->normal);
            if (i == 0) winding = side > 0 ? 1 : -1;
            Check(side * winding > 0, "fan winding changes inside the surface");
            Check(std::abs(Dot(Sub(patch->points[i], cell.corners[i].position), patch->normal) - .05) < 1e-4,
                  "patch lost surface offset");
            const auto ndc = surface::Project(frame.camera, patch->points[i]);
            Check(ndc && Close(*ndc, cell.corners[i].ndc, .001), "patch no longer follows sampled screen footprint");
        }
        Check(std::abs(Dot(Sub(patch->points[4], cell.center.position), patch->normal) - .05) < 1e-4,
              "center did not retain its native position");
    }

    void NativeCameraHandedness()
    {
        Fixture native;
        // Source camera at yaw0: forward+X, right-Y, up+Z. Its x/y/w
        // basis has negative determinant, unlike the original +Z fixture.
        native.forward = {1, 0, 0}; native.right = {0, -1, 0}; native.up = {0, 0, 1}; native.Refresh();
        Check(Dot(Cross(native.right, native.up), native.forward) < 0,
              "native camera fixture accidentally has positive handedness");
        for (Vec2 ndc : {Vec2{-.8f, .6f}, Vec2{0, 0}, Vec2{.7f, -.5f}})
        {
            const auto ray = surface::Direction(native.frame.camera, ndc);
            Check(ray && Close(*ray, native.Ray(ndc)), "negative-handed camera ray differs from native axes");
        }
        const auto wall = PlaneCell(native, Add(native.origin, {200, 0, 0}), {-1, 0, 0});
        VerifyPatch(wall, native.frame);
        VerifyPatch(PlaneCell(native, Add(native.origin, {200, 0, 0}), Unit({-1, .2f, -.3f})), native.frame);
        auto blocked = wall; blocked.center.state = surface::State::Blocked;
        Check(!surface::BuildCell(blocked.corners, blocked.center, native.frame),
              "negative-handed winding borrowed passing corners around a blocked center");
        auto mismatched = wall; ++mismatched.corners[1].identity.shape;
        Check(!surface::BuildCell(mismatched.corners, mismatched.center, native.frame),
              "negative-handed winding joined a different physical shape");
        auto stale = wall; stale.corners[0].frame.nowMs -= 151;
        Check(!surface::BuildCell(stale.corners, stale.center, native.frame),
              "negative-handed winding admitted expired material evidence");

        native.forward = Unit({1, 0, -1}); native.up = Cross(native.right, native.forward); native.Refresh();
        VerifyPatch(PlaneCell(native, Add(native.origin, {200, 0, -200}), {0, 0, 1}), native.frame);
        native.forward = Unit({1, 0, 1}); native.up = Cross(native.right, native.forward); native.Refresh();
        VerifyPatch(PlaneCell(native, Add(native.origin, {200, 0, 200}), {0, 0, -1}), native.frame);

        // Read-only native capture archived in penetration-camera-handedness-live.json.
        // This oblique/downward camera also has a negative x/y/w determinant.
        const float captured[4][4]{
            {-.6972758173942566f, .4563451409339905f, 6.126477103407524e-8f, -416.9374084472656f},
            {-.7301511764526367f, -1.1156396865844727f, .0023253748659044504f, -3304.3642578125f},
            {-.0009553588461130857f, -.0014596129767596722f, -1.000251054763794f, -230.96163940429688f},
            {-.0009551175753585994f, -.001459244405850768f, -.9999984502792358f, -224.40330505371094f}
        };
        const auto capturedCamera = surface::MakeCamera(captured);
        Check(bool(capturedCamera), "captured native camera was rejected");
        native.frame.camera = *capturedCamera; native.origin = capturedCamera->origin; native.frame.eye = native.origin;
        const Vec3 horizontal{captured[0][0], captured[0][1], captured[0][2]};
        const Vec3 vertical{captured[1][0], captured[1][1], captured[1][2]};
        native.sx = float(Length(horizontal)); native.sy = float(Length(vertical));
        native.right = Unit(horizontal); native.up = Unit(vertical);
        native.forward = Unit({captured[3][0], captured[3][1], captured[3][2]});
        Check(Dot(Cross(native.right, native.up), native.forward) < -.99,
              "captured native basis lost its negative handedness");
        VerifyPatch(PlaneCell(native, Add(native.origin, Scale(native.forward, 200)), Scale(native.forward, -1)), native.frame);
    }

    void CameraGeometry()
    {
        Fixture f;
        for (float scale : {1.f, .001f, 100000.f})
        {
            f.Refresh(scale);
            Check(Close(f.frame.camera.origin, f.origin, .001), "recovered camera origin differs from native matrix");
            for (float x : {-1.f, -.3f, 0.f, .6f, 1.f})
                for (float y : {-1.f, 0.f, .4f, 1.f})
                {
                    const Vec2 ndc{x, y}; const auto direction = surface::Direction(f.frame.camera, ndc);
                    Check(direction && Close(*direction, f.Ray(ndc)), "unprojected ray differs from physical camera");
                    const auto point = Add(f.origin, Scale(f.Ray(ndc), 250));
                    const auto projected = surface::Project(f.frame.camera, point);
                    Check(projected && Close(*projected, ndc), "ray point failed independent projection roundtrip");
                }
        }
        f.forward = Unit({1, 2, 3}); f.right = Unit(Cross({0, 0, 1}, f.forward)); f.up = Cross(f.forward, f.right);
        f.origin = {-1234, 2345, 300}; f.biasX = .12f; f.biasY = -.08f; f.Refresh();
        for (Vec2 ndc : {Vec2{-1, 1}, Vec2{0, 0}, Vec2{1, -1}, Vec2{.25f, -.65f}})
        {
            const auto ray = surface::Direction(f.frame.camera, ndc);
            Check(ray && Close(*ray, f.Ray(ndc)), "rotated/off-axis camera ray wrong");
            const auto p = surface::Project(f.frame.camera, Add(f.origin, Scale(f.Ray(ndc), 400)));
            Check(p && Close(*p, ndc, .0005), "rotated/translating camera projection wrong");
        }
        Check(!surface::Project(f.frame.camera, Sub(f.origin, Scale(f.forward, 3))), "behind-camera point projected");
        Check(!surface::Direction(f.frame.camera, {1.01f, 0}), "outside-grid ray accepted");
        float singular[4][4]{};
        Check(!surface::MakeCamera(singular), "zero matrix treated as camera");
        singular[0][0] = singular[1][0] = singular[3][0] = 1;
        Check(!surface::MakeCamera(singular), "parallel matrix rows treated as camera");
        auto malformed = f.frame.camera;
        malformed.matrix[2][0] = std::numeric_limits<float>::quiet_NaN();
        Check(!surface::MakeCamera(malformed.matrix), "nonfinite depth row accepted");
        malformed = f.frame.camera; malformed.origin.x += 1;
        Check(!surface::Direction(malformed, {0, 0}), "inconsistent copied camera origin accepted");
    }

    void SurfaceContinuity()
    {
        Fixture f;
        const auto point = Add(f.origin, {0, 0, 200});
        const Cell wall = PlaneCell(f, point, {0, 0, -1});
        VerifyPatch(wall, f.frame);
        VerifyPatch(PlaneCell(f, point, Unit({.4f, -.3f, -1})), f.frame);
        Fixture floor;
        floor.origin = {0, 0, 80}; floor.frame.eye = floor.origin;
        floor.forward = Unit({1, 0, -1}); floor.right = {0, -1, 0}; floor.up = Cross(floor.forward, floor.right); floor.Refresh();
        VerifyPatch(PlaneCell(floor, {100, 0, 0}, {0, 0, 1}), floor.frame);
        floor.origin.z = -80; floor.frame.eye = floor.origin;
        floor.forward = Unit({1, 0, 1}); floor.up = Cross(floor.forward, floor.right); floor.Refresh();
        VerifyPatch(PlaneCell(floor, {100, 0, 0}, {0, 0, -1}), floor.frame);

        for (surface::State state : {surface::State::Unknown, surface::State::Clear, surface::State::Blocked})
        {
            Cell c = wall; c.center.state = state;
            Check(!surface::BuildCell(c.corners, c.center, f.frame), "thin corners filled unknown/clear/thick center");
            c = wall; c.corners[2].state = state;
            Check(!surface::BuildCell(c.corners, c.center, f.frame), "nonpassing corner filled");
        }
        for (int field = 0; field < 5; ++field)
        {
            Cell c = wall;
            if (field == 0) ++c.corners[0].identity.object;
            if (field == 1) ++c.corners[0].identity.shape;
            if (field == 2) c.corners[0].identity.ownerHandle = 0x10000001;
            if (field == 3) ++c.corners[0].identity.kind;
            if (field == 4) c.corners[0].identity.valid = false;
            Check(!surface::BuildCell(c.corners, c.center, f.frame), "adjoining identity joined across surface/prop");
        }
        Cell c = wall;
        c.center.identity.object = c.center.identity.shape = 0;
        for (auto& corner : c.corners) corner.identity = c.center.identity;
        Check(!surface::BuildCell(c.corners, c.center, f.frame), "anonymous material-only identity joined");
        c = wall; c.corners[0] = f.Hit(c.corners[0].ndc, Add(point, {0, 0, 20}), {0, 0, -1});
        Check(!surface::BuildCell(c.corners, c.center, f.frame), "separate coplanar-depth ledge joined");
        c = wall; c.corners[0].normal = Unit({.6f, 0, -1});
        Check(!surface::BuildCell(c.corners, c.center, f.frame), "normal discontinuity joined");
        c = wall; c.corners[0].normal = {0, 0, 0};
        Check(!surface::BuildCell(c.corners, c.center, f.frame), "zero normal joined");
        c = wall; c.corners[1].position.x = std::numeric_limits<float>::quiet_NaN();
        Check(!surface::BuildCell(c.corners, c.center, f.frame), "nonfinite surface joined");
        c = wall; c.center.ndc.x = .01f;
        Check(!surface::BuildCell(c.corners, c.center, f.frame), "off-center validation hit used for center");
        c = wall; std::swap(c.corners[0], c.corners[1]);
        Check(!surface::BuildCell(c.corners, c.center, f.frame), "unordered/nonadjacent grid joined");
        surface::Limits small; small.maxEdge = 10;
        Check(!surface::BuildCell(wall.corners, wall.center, f.frame, small), "oversized patch exceeded spacing bound");
        c = wall; c.corners[0].normal = {0, 0, 2};
        VerifyPatch(c, f.frame); // Native normal magnitude/sign is normalized, not interpreted as material identity.
    }

    void CacheInvalidation()
    {
        Fixture f;
        const Cell c = PlaneCell(f, Add(f.origin, {0, 0, 200}), {0, 0, -1});
        auto current = f.frame; current.nowMs += 150;
        Check(surface::Fresh(c.center, current), "TTL inclusive boundary rejected");
        VerifyPatch(c, current);
        ++current.nowMs;
        Check(!surface::Fresh(c.center, current), "expired hit reused");
        current = f.frame; --current.nowMs;
        Check(!surface::Fresh(c.center, current), "future sample reused after clock reset");
        for (int key = 0; key < 4; ++key)
        {
            current = f.frame;
            if (key == 0) ++current.key.generation;
            if (key == 1) current.key.localHandle ^= 0x10000;
            if (key == 2) current.key.weaponHandle ^= 0x10000;
            if (key == 3) ++current.key.ballistics;
            Check(!surface::BuildCell(c.corners, c.center, current), "world/local/weapon/stat generation reused");
        }
        current = f.frame; current.eye.x += .26f;
        Check(!surface::Fresh(c.center, current), "changed shot eye retained penetration verdict");
        Fixture moved = f; moved.origin.x += .26f; moved.Refresh();
        Check(!surface::Fresh(c.center, moved.frame), "changed camera origin retained patch");
        Fixture rotated = f; rotated.forward = Unit({.02f, 0, 1}); rotated.right = Unit(Cross({0, 1, 0}, rotated.forward));
        rotated.up = Cross(rotated.forward, rotated.right); rotated.Refresh();
        Check(!surface::Fresh(c.center, rotated.frame), "camera rotation reused old screen ray");
        Fixture zoom = f; zoom.sx *= 1.2f; zoom.Refresh();
        Check(!surface::BuildCell(c.corners, c.center, zoom.frame), "FOV change retained off-center patch");
        Fixture rolled = f; rolled.right = {0, 1, 0}; rolled.up = {-1, 0, 0}; rolled.Refresh();
        Check(!surface::BuildCell(c.corners, c.center, rolled.frame), "camera roll reused old sampled footprint");
        auto wrong = c.center; wrong.position.x += 3;
        Check(!surface::Fresh(wrong, f.frame), "hit not on saved ray reused");
        current = f.frame; current.eye.y += .05f;
        VerifyPatch(c, current); // Small explicitly bounded shot-eye movement is permitted.
        surface::Limits invalid; invalid.maxPlaneDistance = std::numeric_limits<float>::quiet_NaN();
        Check(!surface::BuildCell(c.corners, c.center, f.frame, invalid), "invalid tolerances used");
    }

    void CompleteGrid()
    {
        // Geometry gives all grid rays the same plane and physics identity. Only
        // center penetration state differs: a thick middle must remain unfilled.
        Fixture f;
        std::size_t filled = 0;
        for (int x = 0; x < 3; ++x)
        {
            const Vec2 lo{-.3f + x * .2f, -.1f}, hi{-.1f + x * .2f, .1f};
            auto c = PlaneCell(f, Add(f.origin, {0, 0, 180}), {0, 0, -1}, lo, hi);
            if (x == 1) c.center.state = surface::State::Blocked;
            if (surface::BuildCell(c.corners, c.center, f.frame)) ++filled;
        }
        Check(filled == 2, "thin adjacent patches colored entire thick shared mesh");
        auto c = PlaneCell(f, Add(f.origin, {0, 0, 180}), {0, 0, -1});
        c.corners[3].frame.nowMs -= 151;
        Check(!surface::BuildCell(c.corners, c.center, f.frame), "partially refreshed cell borrowed expired corner");
        Check(surface::PlannedSamples(12, 8) == 213 && surface::PlannedSamples(20, 12) == 513 &&
              surface::PlannedSamples(28, 16) == 941, "grid nodes/independent center accounting wrong");
        Check(surface::PlannedSamples(0, 12) == 0 && surface::PlannedSamples(1000000, 12) == 0 &&
              surface::PlannedSamples(64, 64) == 8321, "unbounded grid allocation accepted");

        // Each validated fan triangle is clipped using the production near-w /
        // viewport projector. The quad repeats its last vertex to describe a triangle.
        const auto patch = surface::BuildCell(c.corners, PlaneCell(f, Add(f.origin, {0, 0, 180}), {0, 0, -1}).center, f.frame);
        Check(!patch, "expired grid fixture unexpectedly produced triangles");
        c = PlaneCell(f, Add(f.origin, {0, 0, 180}), {0, 0, -1}, {-.95f, -.1f}, {1.f, .1f});
        const auto p = surface::BuildCell(c.corners, c.center, f.frame);
        Check(bool(p), "viewport-edge patch rejected");
        for (std::size_t i = 0; i < 4; ++i)
        {
            const auto a = p->points[i], b = p->points[(i + 1) % 4], m = p->points[4];
            const square::Corners triangle{{{a.x, a.y, a.z}, {b.x, b.y, b.z}, {m.x, m.y, m.z}, {m.x, m.y, m.z}}};
            square::Polygon polygon;
            Check(square::Project(triangle, f.frame.camera.matrix, 1920, 1080, polygon), "validated fan triangle did not project");
            for (std::size_t j = 0; j < polygon.count; ++j)
                Check(polygon.points[j].x >= 0 && polygon.points[j].x <= 1920 &&
                      polygon.points[j].y >= 0 && polygon.points[j].y <= 1080, "projected patch escaped viewport");
        }
    }

    void PersistentPropSelection()
    {
        props::Cache<2> cache;
        props::Stamp first{{17, 0x10000}, 0x12340001u, 0x20000, 0x30000, 0x40000, 0x50000, 0x60000,
                           {{100, -30, 4}, {0, 90, 0}}};
        Check(!cache.Remember(first, true) && cache.Size() == 0,
              "Discovery activated a map without an explicit current-generation check");
        Check(cache.SetMap(first.map) && !cache.Remember(first, false) && cache.Size() == 0,
              "Unqualified native discovery selected a prop");
        Check(cache.Remember(first, true) && cache.Size() == 1 && cache.Matches(first),
              "Qualified model/collision identity was not selected");
        for (int field = 0; field < 10; ++field)
        {
            auto changed = first;
            if (field == 0) ++changed.map.epoch;
            if (field == 1) changed.map.list += 0x10000;
            if (field == 2) changed.handle += 1u << 15; // Reused native index, different full serial.
            if (field == 3) changed.entity += 0x100;
            if (field == 4) changed.node += 0x100;
            if (field == 5) changed.modelBinding += 0x100;
            if (field == 6) changed.modelResource += 0x100;
            if (field == 7) changed.collision += 0x100;
            if (field == 8) changed.transform.origin[0] = std::nextafter(changed.transform.origin[0], INFINITY);
            if (field == 9) changed.transform.angles[1] = std::nextafter(changed.transform.angles[1], INFINITY);
            Check(!cache.Matches(changed), "Changed map/serial/model/collision/transform reused a persistent selection");
        }
        for (int field = 0; field < 11; ++field)
        {
            auto invalid = first;
            if (field == 0) invalid.map.epoch = 0;
            if (field == 1) invalid.map.list = 0;
            if (field == 2) invalid.handle = 0xfffffffeu;
            if (field == 3) invalid.entity = 0;
            if (field == 4) invalid.node = 0x8000000000000000ULL;
            if (field == 5) invalid.modelBinding = 0xffffffffffffffffULL;
            if (field == 6) invalid.modelResource = 1;
            if (field == 7) invalid.collision = 0;
            if (field == 8) invalid.transform.origin[0] = NAN;
            if (field == 9) invalid.transform.angles[1] = INFINITY;
            if (field == 10) invalid.transform.origin[2] = 1e8f;
            Check(!cache.Remember(invalid, true) && !cache.Matches(invalid) && cache.Size() == 1 && cache.Matches(first),
                  "Invalid native token or transform polluted an existing selection");
        }
        for (std::uint32_t handle : {0u, 0x8000u, 0x7fffu, 0xffffffffu})
        {
            auto invalid = first; invalid.handle = handle;
            Check(!cache.Remember(invalid, true), "Invalid/anonymous owner became a persistent prop");
        }
        auto signedZero = first; signedZero.transform.angles[0] = -0.f;
        Check(cache.Matches(signedZero) && props::Fingerprint(signedZero.transform) == props::Fingerprint(first.transform),
              "Equivalent native signed zero flickered a selected prop");
        // Rendering from a new camera or with another weapon does not feed a
        // timer/pose/weapon key into this map-lifetime geometry selection.
        for (int frame = 0; frame < 1000; ++frame)
            Check(cache.Matches(first), "An unchanged native identity expired during repeated renders");

        auto second = first; second.handle = 0x23450002u; second.entity += 0x1000;
        auto third = first; third.handle = 0x34560003u; third.entity += 0x2000;
        Check(cache.Remember(second, true) && cache.Size() == 2 && !cache.Remember(third, true) &&
              cache.Matches(first) && cache.Matches(second) && !cache.Matches(third),
              "Capacity saturation evicted a previously selected prop");
        auto replacement = first; replacement.handle += 1u << 15; replacement.entity += 0x3000;
        replacement.node += 0x1000; replacement.modelBinding += 0x1000;
        replacement.modelResource += 0x1000; replacement.collision += 0x1000;
        Check(cache.Remember(replacement, true) && cache.Size() == 2 && cache.Matches(replacement) &&
              !cache.Matches(first) && cache.Matches(second),
              "Full cache could not replace a reused native slot without evicting another prop");
        Check(!cache.Forget(first.handle) && cache.Matches(replacement),
              "Removing an old serial erased the replacement entity");
        Check(cache.Forget(replacement.handle) && cache.Size() == 1 && cache.Remember(third, true) &&
              cache.Matches(second) && cache.Matches(third), "Explicit removal did not release bounded admission capacity");

        auto newMap = first.map; ++newMap.epoch;
        Check(cache.SetMap(newMap) && cache.Size() == 0 && !cache.Matches(second) && !cache.Remember(first, true),
              "Map transition retained old geometry or accepted a late old-map callback");
        auto current = first; current.map = newMap;
        Check(cache.Remember(current, true) && cache.SetMap(newMap) && cache.Matches(current),
              "Repeated current-map lifecycle update discarded a persistent selection");
        newMap.list += 0x10000;
        Check(cache.SetMap(newMap) && cache.Size() == 0, "Replacement entity list retained prior native pointers");
        current.map = newMap; Check(cache.Remember(current, true), "Selection did not recover on the replacement list");
        cache.Clear(); Check(cache.Size() == 0 && !cache.Matches(current) && !cache.Remember(current, true),
              "Explicit clear retained map lifetime or prior geometry");
        Check(cache.SetMap(newMap) && cache.Remember(current, true), "Cleared cache could not be reactivated");
        Check(!cache.SetMap({}) && cache.Size() == 0 && !cache.Matches(current),
              "Invalid/disconnected map retained a selected prop");
    }
}

int main()
{
    try
    {
        CameraGeometry(); NativeCameraHandedness(); SurfaceContinuity(); CacheInvalidation(); CompleteGrid(); PersistentPropSelection();
        std::cout << "penetration surface regression passed: bounded map-lifetime prop selection, native/positive camera handedness, five-hit continuity, partial coverage, cache invalidation and clipped fan triangles\n";
        return 0;
    }
    catch (const std::exception& e)
    { std::cerr << "penetration surface regression failed: " << e.what() << '\n'; return 1; }
}
