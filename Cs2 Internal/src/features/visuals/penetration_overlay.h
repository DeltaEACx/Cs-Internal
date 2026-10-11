#pragma once
#include "frame_data.hpp"
#include <array>

namespace visuals::penetration_overlay {
struct Settings {
    bool enabled{true}, walls{true}, props{true};
    int quality{1}, probesPerFrame{32};
    float budgetMs{2.f}, maxDistance{2500.f}, minDamage{1.f};
    std::array<float,4> color{.2f,.95f,.6f,.30f};
};
Settings Get();
void Save(Settings value);
void Initialize();
void Frame(bool inWorld);
void Render(const frame_snapshot& frame);
void DrawMenu();
bool Reset();
void RequestStop();
bool Stopped();
}
