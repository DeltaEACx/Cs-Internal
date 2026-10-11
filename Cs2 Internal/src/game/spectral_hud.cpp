#include "game/combat_access.h"
#include "../../third_party/spectral/bridge/hud.hpp"

namespace combat_access {

bool Finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

float Convar(const char* name, float fallback) {
    if (!name || !game::module_matches("client.dll", skin_bindings::kClientSha256)) return fallback;
    return spectral_bridge::hud::Convar(name, fallback);
}

bool ReadViewAngles(Vec3& angles) {
    if (!game::module_matches("client.dll", skin_bindings::kClientSha256)) return false;
    float xyz[3]{};
    if (!spectral_bridge::hud::ReadViewAngles(xyz)) return false;
    angles = {xyz[0], xyz[1], xyz[2]};
    return true;
}

bool ReadAccuracy(Address weapon, float& spread, float& inaccuracy) {
    return weapon && game::module_matches("client.dll", skin_bindings::kClientSha256) &&
        spectral_bridge::hud::ReadAccuracy(weapon, spread, inaccuracy);
}

} // namespace combat_access
