#pragma once
#include "SkinBindings.h"

// Audited against the supported installed modules.
namespace preview_bindings
{
    inline constexpr std::string_view kMaterialSha256 =
        "336abe089468bd5b218f773f09da3942fa9346b7f49f24d9e3591722fc54c5e6";
    inline constexpr std::string_view kRenderSha256 =
        "321af39487adad1b0f0cfc7e0ad519c5b07937a40e16593fdfd12e1f67da8a56";
    inline constexpr std::uint32_t kMaterialVtable = 0x129ad0;
    inline constexpr std::uint32_t kGetTextureSlot = 16;
    inline constexpr std::uint32_t kColorToken = 0x5963540f; // CUtlStringToken::Make("g_tColor")
    inline constexpr std::uint32_t kRepresentativeToken = 0x5d700411; // Shader's primary color texture.
    inline constexpr std::uint32_t kMaterialName = 0x10, kOriginalMaterial = 0x60;
    inline constexpr std::uint32_t kOwnerMaterials = 0x4a0;
    inline constexpr std::uint32_t kTextureSrgbView = 0x18, kTextureLinearView = 0x10;
    inline constexpr skin_bindings::Function GetTexture{0xC0B0u, {0x48, 0x83, 0xEC, 0x18, 0x66, 0x41, 0x0F, 0x6E, 0xC8, 0x4C, 0x8D, 0x89, 0xF0, 0x01, 0x00, 0x00, 0x66, 0x0F, 0x70, 0xC9, 0x00, 0x4C, 0x8D, 0x1D}};
}
