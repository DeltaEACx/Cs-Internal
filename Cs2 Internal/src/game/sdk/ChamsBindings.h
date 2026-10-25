#pragma once
#include "game/sdk/SkinBindings.h"

namespace chams_bindings
{
    inline constexpr std::string_view kSceneSha256 = "0f17ccdc083290a496e6ac13e4fe54413077ad2b83b7dfd281749cffd2733d9b";
    inline constexpr std::string_view kMaterialSha256 = "2fe9750177006c24aacd0f86b14efbc00d04ca61d86f27f1b07b263eefb06d69";
    inline constexpr std::string_view kTier0Sha256 = "f96f4f9666be66ee3dd42a015b305d954b5dd5582b0608a74e50548df426584d";
    // CAnimatableSceneObjectDesc and CBaseSceneObjectDesc share this method.
    // CSceneObjectDesc's static-mesh method does not receive player/viewmodel draws.
    inline constexpr skin_bindings::Function kGeneratePrimitives{0x7D7C0u, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x20, 0x4C, 0x89, 0x40, 0x18, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54}};
    inline constexpr std::array<std::uint32_t, 2> kModelDescriptorVtables{0x5E5C90, 0x5E5D70};
    inline constexpr std::size_t kGeneratePrimitivesSlot = 4;
    inline constexpr std::size_t kSceneOwner = 0xc0;
    inline constexpr std::size_t kSceneFlags = 0x78;
    inline constexpr std::size_t kSceneTransform = 0x30;
    inline constexpr std::size_t kRenderBoneCount = 0xd0;
    inline constexpr std::size_t kRenderBones = 0xd8;
    inline constexpr std::uint8_t kOriginalMaterialFlag = 8;
    inline constexpr const char *kLoadKv3 = "?LoadKV3@@YA_NPEAVKeyValues3@@PEAVCUtlString@@PEBDAEBUKV3ID_t@@2I@Z";
}
