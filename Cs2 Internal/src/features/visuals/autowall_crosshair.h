#pragma once
#include "frame_data.hpp"

namespace visuals::autowall_crosshair {
    struct Settings { bool enabled{}; float size{8}; };
    Settings Get();
    void Save(Settings value);
    void Initialize();
    void Frame(bool inWorld);
    void Render(const frame_snapshot &frame);
    void DrawMenu();
    // Native scratch storage is released on the frame thread before unloading.
    bool Reset();
    void RequestStop();
    bool Stopped();
}
