#pragma once
#include "game/sdk/SkinBindings.h"

namespace chams_bindings
{
    inline constexpr std::string_view kSceneSha256 = "bd770af13a0b1352d87de26cbcfef9e36ef809771ae6a2e5314d7bc6b341f831";
    inline constexpr std::string_view kMaterialSha256 = "336abe089468bd5b218f773f09da3942fa9346b7f49f24d9e3591722fc54c5e6";
    inline constexpr std::string_view kTier0Sha256 = "00ecf3e3d33f9036f3fd6e69f2c89510148e1a6202dd8bdea592fecc7891cfd4";
    // CAnimatableSceneObjectDesc and CBaseSceneObjectDesc share this method.
    // CSceneObjectDesc's static-mesh method does not receive player/viewmodel draws.
    inline constexpr skin_bindings::Function kGeneratePrimitives{0x7D7C0u, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x20, 0x4C, 0x89, 0x40, 0x18, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54}};
    inline constexpr std::array<std::uint32_t, 2> kModelDescriptorVtables{0x5e5c90, 0x5e5d70};
    inline constexpr std::size_t kGeneratePrimitivesSlot = 4;
    inline constexpr std::size_t kSceneOwner = 0xc0;
    inline constexpr std::size_t kSceneFlags = 0x78;
    inline constexpr std::size_t kSceneTransform = 0x30;
    inline constexpr std::size_t kRenderBoneCount = 0xd0;
    inline constexpr std::size_t kRenderBones = 0xd8;
    inline constexpr std::uint8_t kOriginalMaterialFlag = 8;
    inline constexpr const char *kLoadKv3 = "?LoadKV3@@YA_NPEAVKeyValues3@@PEAVCUtlString@@PEBDAEBUKV3ID_t@@2I@Z";
}
