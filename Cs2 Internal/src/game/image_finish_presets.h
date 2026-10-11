#pragma once
#include <array>
namespace image_finishes
{
    struct Preset { const char *name; const char *texture; };
    inline constexpr std::array<Preset, 8> kPresets{{
        {"Woodland Camo", "materials/custom/camo/woodland.vtex"},
        {"Desert Camo", "materials/custom/camo/desert.vtex"},
        {"Arctic Camo", "materials/custom/camo/arctic.vtex"},
        {"Urban Digital", "materials/custom/camo/urban.vtex"},
        {"Woodland Digital", "materials/custom/camo/woodland_digital.vtex"},
        {"Desert Digital", "materials/custom/camo/desert_digital.vtex"},
        {"Tiger Stripe", "materials/custom/camo/tiger.vtex"},
        {"Midnight Digital", "materials/custom/camo/midnight.vtex"},
    }};
}
