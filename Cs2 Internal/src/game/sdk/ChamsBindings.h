#pragma once
#include "game/sdk/SkinBindings.h"

namespace chams_bindings
{
    inline constexpr std::string_view kSceneSha256 = "040eb851fa7875523346b890f1ed6e158abc59e08bccbd8dc60539435277efa5";
    inline constexpr std::string_view kMaterialSha256 = "b4d5c667eb65f13d9193d743a88eb1f9dfa623cc8f745cef764d9977969e38b6";
    inline constexpr std::string_view kTier0Sha256 = "990ea56bf1626d299ba54b64b423f2e409bd592bb737c7a5edf7972ad5c7143e";
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
