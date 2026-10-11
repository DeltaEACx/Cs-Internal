#pragma once
#include <cstdint>
#include <string>
namespace math { class vector3; }
namespace visuals::particle_chams {
struct Settings {
    bool enabled{}, enemies{true}, teammates{}, local{};
    int style{}, players{4};
    float radius{9.f}, orbitSpeed{.4f}, interval{.7f}, distance{80.f};
    bool operator==(const Settings &) const = default;
};
Settings Get();
void Save(Settings settings);
bool Enabled() noexcept;
void DrawMenu();
// Native frame-stage thread only, alongside the shared particle backend.
void Update();
void Reset();
bool AttachmentPosition(std::uint32_t pawn, unsigned anchor, math::vector3 &point);
std::string Status();
}
