#pragma once
#include "features/combat/detail/hvh_math.hpp"
namespace hvh {
struct SpreadPrediction {
    hvh_math::Vec3 origin{}, angles{};
    std::optional<hvh_math::PredictedShot> exact;
    float range{}, radius{};
    std::uint32_t localHandle{0xffffffffu}, weaponHandle{0xffffffffu};
    unsigned epoch{};
    std::uint64_t time{};
    bool compensated{};
};
std::optional<SpreadPrediction> Prediction();
bool Install();
const char* Status();
bool Stop();
void SetInputState(bool menuOpen, float width, float height);
void DrawMenu();
void DrawAntiAimMenu();
}
