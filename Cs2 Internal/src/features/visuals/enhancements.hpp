#pragma once
#include "engine.hpp"
#include "settings.hpp"

namespace visuals::enhancements
{
    struct ThrowableSettings {
        bool trajectory{true};
        color pathColor{189,74,81,230};
        float width{2}, markerRadius{12};
    };
    constexpr int ThrowableIndex(int definition){return definition>=43&&definition<=48?definition-43:-1;}
    struct Configuration
    {
        bool spreadCircle{}, spreadFill{}, crosshair{}, scope{}, scopeGlow{};
        int crosshairStyle{}, scopeStyle{};
        float spreadScale{1}, crosshairSize{4}, scopeLength{120}, scopeGap{12}, scopeThickness{1.5f};
        color accent{130,185,255,230};
        bool spectators{}, velocityChart{}, watermark{}, hat{};
        int hatStyle{};
        bool jumpRings{}, enemyJumpsOnly{true};
        float jumpRadius{60}, jumpDuration{1.5f};
        color jumpColor{175,110,255,230};
        bool ragdollGravity{}, radar{}, flashLimit{}, smokeTint{};
        float gravityScale{}, flashOpacity{30};
        color smokeColor{175,110,255,255};
        bool enemyGlow{}, teamGlow{}, localGlow{}, corpseGlow{}, itemGlow{};
        color enemyColor{255,100,120,220}, teamColor{100,165,255,220}, itemColor{255,210,100,220};
        bool localOpacity{}, opacityScopedOnly{true};
        float opacity{35};
        bool viewmodel{}, removeCrosshair{}, removeScope{}, removeLegs{};
        std::array<float,3> viewmodelOffset{};
        float viewmodelFov{68};
        bool fireArea{}, smokeArea{}, grenadePrediction{};
        std::array<ThrowableSettings,6> throwables{};
        bool dynamicLight{};float lightRadius{250},lightHeight{50};color lightColor{175,110,255,255};
    };
    enum class Page {Hud, Widgets, Movement, Models, Removals, Grenades, Local};
    Configuration Get();
    void Set(Configuration value);
    bool Initialize();
    bool HideLocalLegs(std::uintptr_t owner);
    std::optional<math::vector3> LightColor(std::uintptr_t scene);
    void Frame();
    void Render(const frame_snapshot &frame);
    void Menu(Page page);
    void Reset(); // Game thread, before map teardown.
    void RequestStop();
    bool Stopped();
    float SpreadRadius(float spread, float inaccuracy, float horizontalFov, float width);
}
