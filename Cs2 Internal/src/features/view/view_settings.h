#pragma once
namespace view_settings
{
    void Install();
    void Tick(); // Game thread only.
    void Draw(); // ImGui render thread.
    void OnLevelShutdown(); // Game thread, before replacing input/map state.
    bool Stop(); // Restores camera on the game thread before disabling its hooks.
} // namespace view_settings
