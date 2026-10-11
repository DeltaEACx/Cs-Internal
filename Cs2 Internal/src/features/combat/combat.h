#pragma once
#include "game/combat_access.h"

namespace combat_features
{
    struct Settings
    {
        bool aimbot{}, triggerbot{}, autowall{}, silentAim{};
        bool penetrationCrosshair{};
        int aimKey{VK_XBUTTON2}, triggerKey{VK_XBUTTON1};
        float fov{5};
        int smooth{5};
        std::array<bool, 5> hitboxes{true, false, false, false, false};
        bool recoilControl{true};
        float recoilStrength{100};
        int triggerDelay{5}, hitchance{80};
        bool headOnly{};
        int minimumDamage{40};
    };
    void Install();
    void ProcessPendingInstall();
    bool Stop();
    void Draw();
    void Render();
    void SetMenuOpen(bool open);
    void FrameStage(int stage);
    void AfterFrameStage(int stage);
    void LevelShutdown();
    void OnHurt(std::uintptr_t pawn, int damage, int hitgroup);
    void OnImpact(float x, float y, float z);
    void OnWeaponFire();
}
