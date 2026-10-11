#include "features/combat/combat.h"
#include <atomic>
#include "../../../third_party/spectral/bridge/runtime.hpp"

namespace combat_features
{
    namespace { std::atomic<bool> retryRequested{}; }
    void Install()
    {
        retryRequested = false;
        if (game::module_matches("client.dll", skin_bindings::kClientSha256)) spectral_bridge::Install();
        else spectral_bridge::SetUnavailable("The installed client does not match this combat build.");
    }
    void ProcessPendingInstall()
    {
        if (!retryRequested.exchange(false)) return;
        if (game::module_matches("client.dll", skin_bindings::kClientSha256)) spectral_bridge::Install(true);
        else spectral_bridge::SetUnavailable("The installed client does not match this combat build.");
    }
    bool Stop() { retryRequested = false; return spectral_bridge::Stop(); }
    void Draw() { if (spectral_bridge::Draw()) retryRequested = true; }
    void Render() { spectral_bridge::Render(); }
    void SetMenuOpen(bool open) { spectral_bridge::SetMenuOpen(open); }
    void FrameStage(int stage) { spectral_bridge::FrameStage(stage); }
    void AfterFrameStage(int stage) { spectral_bridge::AfterFrameStage(stage); }
    void LevelShutdown() { spectral_bridge::LevelShutdown(); }
    void OnHurt(std::uintptr_t pawn, int damage, int hitgroup) { spectral_bridge::OnHurt(pawn, damage, hitgroup); }
    void OnImpact(float x, float y, float z) { spectral_bridge::OnImpact(x, y, z); }
    void OnWeaponFire() { spectral_bridge::OnWeaponFire(); }
}
