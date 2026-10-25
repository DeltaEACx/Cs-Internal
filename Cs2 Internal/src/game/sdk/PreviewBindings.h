#pragma once
#include "SkinBindings.h"

// Audited against the supported installed modules.
namespace preview_bindings
{
    inline constexpr std::string_view kMaterialSha256 =
        "2fe9750177006c24aacd0f86b14efbc00d04ca61d86f27f1b07b263eefb06d69";
    inline constexpr std::string_view kRenderSha256 =
        "fbd28df3458e1f5d0a21ff03c2c25e63a372063098dc4f455b07b80b3473eb8f";
    inline constexpr std::uint32_t kMaterialVtable = 0x129AE0;
    inline constexpr std::uint32_t kGetTextureSlot = 16;
    inline constexpr std::uint32_t kColorToken = 0x5963540f; // CUtlStringToken::Make("g_tColor")
    inline constexpr std::uint32_t kRepresentativeToken = 0x5d700411; // Shader's primary color texture.
    inline constexpr std::uint32_t kMaterialName = 0x10, kOriginalMaterial = 0x60;
    inline constexpr std::uint32_t kOwnerMaterials = 0x4a0;
    inline constexpr std::uint32_t kTextureSrgbView = 0x18, kTextureLinearView = 0x10;
    inline constexpr skin_bindings::Function GetTexture{0xC0B0u, {0x48, 0x83, 0xEC, 0x18, 0x66, 0x41, 0x0F, 0x6E, 0xC8, 0x4C, 0x8D, 0x89, 0xF0, 0x01, 0x00, 0x00, 0x66, 0x0F, 0x70, 0xC9, 0x00, 0x4C, 0x8D, 0x1D}};
}
