#pragma once

namespace visual_features
{
    // Reuse the existing frame hook when available; otherwise install a visual-only one.
    void Install(bool sharedFrameHook);
    void BeforeStage(int stage);
    void AfterStage(int stage);
    void Render();
    enum class Page {Players, Items, Hud, Chams, WeaponParticles, EventEffects, Skybox, Lighting, Weather, WorldParticles, Crosshair, Movement, Glow, Removals, GrenadeEffects, GunImages, EnemyModels, PenetrableSurfaces, Local};
    void DrawMenu(Page page=Page::Players);
    bool Stop();
}
