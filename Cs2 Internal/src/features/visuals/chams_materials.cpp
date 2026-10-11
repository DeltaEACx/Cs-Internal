#include "chams_materials.h"
#include "game/sdk/ChamsBindings.h"
#include <cstdio>

namespace visuals::chams::materials
{
    namespace
    {
        // Material presets adapted from Spectral.win.
        constexpr char electric[] = R"#(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d}
            format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
            {
                shader = "csgo_complex.vfx"

                F_SELF_ILLUM = 1
                F_RENDER_BACKFACES = 1
                F_TRANSLUCENT = 1

                g_vColorTint = [0.0, 0.0, 0.0]
                g_flModelTintAmount = 1.0
                g_flOpacityScale = 0.8

                g_flSelfIllumBrightness = 3.0
                g_flSelfIllumScale = 1.5
                g_vSelfIllumTint = [0.4, 0.7, 1.0]
                g_flSelfIllumAlbedoFactor = 0.3
                g_vSelfIllumScrollSpeed = [0.15, 0.1]

                g_vTexCoordScrollSpeed = [0.03, 0.02]
                g_vTexCoordScale = [2.0, 2.0]

                g_bFogEnabled = 0

                g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
                g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
                g_tSelfIllumMask = resource:"materials/particle/electrical/electrical_cracks.vtex"
                g_tAmbientOcclusion = resource:"materials/default/default_mask_tga_fde710a5.vtex"
                g_tTintMask = resource:"materials/default/default_mask_tga_fde710a5.vtex"
            })#";
        constexpr char liquid[] = R"#(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d}
            format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
            {
                shader = "csgo_complex.vfx"

                F_SELF_ILLUM = 1
                F_RENDER_BACKFACES = 1
                F_TRANSLUCENT = 1

                g_vColorTint = [0.0, 0.0, 0.0]
                g_flModelTintAmount = 1.0
                g_flOpacityScale = 0.8

                g_flSelfIllumBrightness = 3.0
                g_flSelfIllumScale = 1.5
                g_vSelfIllumTint = [0.4, 0.7, 1.0]
                g_flSelfIllumAlbedoFactor = 0.3
                g_vSelfIllumScrollSpeed = [0.05, 0.03]

                g_vTexCoordScrollSpeed = [0.01, 0.005]

                g_bFogEnabled = 0

                g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
                g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
                g_tSelfIllumMask = resource:"materials/dev/water_waves.vtex"
                g_tAmbientOcclusion = resource:"materials/default/default_mask_tga_fde710a5.vtex"
                g_tTintMask = resource:"materials/default/default_mask_tga_fde710a5.vtex"
            })#";
        constexpr char liquid_ignorez[] = R"#(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d}
            format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
            {
                shader = "csgo_complex.vfx"

                F_SELF_ILLUM = 1
                F_RENDER_BACKFACES = 1
                F_TRANSLUCENT = 1
                F_DISABLE_Z_BUFFERING = 1

                g_vColorTint = [1.0, 1.0, 1.0]
                g_flModelTintAmount = 1.0
                g_flOpacityScale = 0.8

                g_flSelfIllumBrightness = 3.0
                g_flSelfIllumScale = 1.5
                g_vSelfIllumTint = [0.4, 0.7, 1.0]
                g_flSelfIllumAlbedoFactor = 0.3
                g_vSelfIllumScrollSpeed = [0.05, 0.03]

                g_vTexCoordScrollSpeed = [0.01, 0.005]

                g_bFogEnabled = 0

                g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
                g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
                g_tSelfIllumMask = resource:"materials/dev/water_waves.vtex"
                g_tAmbientOcclusion = resource:"materials/default/default_mask_tga_fde710a5.vtex"
                g_tTintMask = resource:"materials/default/default_mask_tga_fde710a5.vtex"
            })#";
        constexpr char metallic[] = R"#(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d}
            format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
            {
                shader = "csgo_character.vfx"

                F_IRIDESCENCE = 1
                F_CLOTH_SHADING = 1
                F_RENDER_BACKFACES = 1
                F_DISABLE_Z_PREPASS = 1
                F_TRANSLUCENT = 1
                F_ADDITIVE_BLEND = 1

                g_vColorTint = [1.0, 1.0, 1.0]
                g_flModelTintAmount = 1.0
                g_flOpacityScale = 1.0

                g_vTexCoordScrollSpeed = [0.05, 0.02]
                g_vTexCoordScale = [1.2, 1.2]

                g_flIridescentStrength = 2.0
                g_flIridescentFresnelStrength = 15.0
                g_flIridescentHueShift = 0.5

                g_flSheenScale = 10.0
                g_flSheenTintColor = [1.0, 1.0, 1.0]

                g_fContrast = 0.5
                g_fBrightness = 1.5
                g_fSaturation = 1.5

                g_flAmbientOcclusionMasking = 0.0
                g_bFogEnabled = 0

                g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
                g_tNormal = resource:"materials/dev/water_waves.vtex"
                g_tMetalness = resource:"materials/dev/water_waves.vtex"
                g_tAmbientOcclusion = resource:"materials/default/default_mask_tga_fde710a5.vtex"
                g_tIridescentThickness_Mask = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
            })#";
        constexpr char matte[] = R"#(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d}
            format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
            {
                shader = "generic.vfx"

                g_vColorTint = [1.0, 1.0, 1.0, 1.0]

                g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
                g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
            })#";
        constexpr char matte_ignorez[] = R"#(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d}
            format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
            {
                shader = "generic.vfx"

                g_vColorTint = [1.0, 1.0, 1.0, 1.0]

                g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
                g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
            })#";
        constexpr char flat[] = R"#(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d}
            format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
            {
                shader = "csgo_unlitgeneric.vfx"

                F_RENDER_BACKFACES = 0
                F_DISABLE_Z_BUFFERING = 0
                F_PAINT_VERTEX_COLORS = 1
                F_TRANSLUCENT = 1
                F_BLEND_MODE = 1

                g_vColorTint = [1.0, 1.0, 1.0, 1.0]

                g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
                g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
            })#";
        constexpr char flat_ignorez[] = R"#(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d}
            format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
            {
                shader = "csgo_unlitgeneric.vfx"

                F_RENDER_BACKFACES = 0
                F_DISABLE_Z_BUFFERING = 1
                F_DISABLE_Z_WRITE = 1
                F_PAINT_VERTEX_COLORS = 1
                F_TRANSLUCENT = 1
                F_BLEND_MODE = 1

                g_vColorTint = [1.0, 1.0, 1.0, 1.0]

                g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
                g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
            })#";
        constexpr char bloom[] = R"#(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d}
            format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
            {
                shader = "solidcolor.vfx"

                F_DISABLE_Z_WRITE = 0

                g_vColorTint = [8.0, 8.0, 8.0]
            })#";
        constexpr char bloom_ignorez[] = R"#(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d}
            format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
            {
                shader = "solidcolor.vfx"

                F_IGNOREZ = 1
                F_DISABLE_Z_WRITE = 1

                g_vColorTint = [5.0, 5.0, 5.0]
            })#";
        constexpr char outlines[] = R"#(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d}
            format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
            {
                shader = "csgo_effects.vfx"

                F_ADDITIVE_BLEND = 1
                F_BLEND_MODE = 1
                F_TRANSLUCENT = 1

                g_vColorTint = [1.0, 1.0, 1.0, 0.0]
                g_flOpacityScale = 0.45
                g_flFresnelExponent = 0.75
                g_flFresnelFalloff = 1.0
                g_flFresnelMax = 0.0
                g_flFresnelMin = 1.0
                g_flColorBoost = 2.25
                g_flToolsVisCubemapReflectionRoughness = 1.0
                g_flBeginMixingRoughness = 1.0

                g_tColor = resource:"materials/default/default_mask_tga_fde710a5.vtex"
                g_tMask1 = resource:"materials/default/default_mask_tga_fde710a5.vtex"
                g_tMask2 = resource:"materials/default/default_mask_tga_fde710a5.vtex"
                g_tMask3 = resource:"materials/default/default_mask_tga_fde710a5.vtex"
                g_tSceneDepth = resource:"materials/default/default_mask_tga_fde710a5.vtex"
            })#";
        constexpr char outlines_ignorez[] = R"#(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d}
            format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
            {
                shader = "csgo_effects.vfx"

                F_ADDITIVE_BLEND = 1
                F_BLEND_MODE = 1
                F_TRANSLUCENT = 1
                F_DISABLE_Z_BUFFERING = 1
                F_DISABLE_Z_WRITE = 1

                g_vColorTint = [1.0, 1.0, 1.0, 0.0]
                g_flOpacityScale = 0.45
                g_flFresnelExponent = 0.75
                g_flFresnelFalloff = 1.0
                g_flFresnelMax = 0.0
                g_flFresnelMin = 1.0
                g_flColorBoost = 2.25
                g_flToolsVisCubemapReflectionRoughness = 1.0
                g_flBeginMixingRoughness = 1.0

                g_tColor = resource:"materials/default/default_mask_tga_fde710a5.vtex"
                g_tMask1 = resource:"materials/default/default_mask_tga_fde710a5.vtex"
                g_tMask2 = resource:"materials/default/default_mask_tga_fde710a5.vtex"
                g_tMask3 = resource:"materials/default/default_mask_tga_fde710a5.vtex"
                g_tSceneDepth = resource:"materials/default/default_mask_tga_fde710a5.vtex"
            })#";
        constexpr char glow[] = R"#(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d}
            format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
            {
                shader = "csgo_effects.vfx"

                F_ADDITIVE_BLEND = 1
                F_BLEND_MODE = 1
                F_TRANSLUCENT = 1
                F_IGNOREZ = 0
                F_DISABLE_Z_BUFFERING = 0
                F_RENDER_BACKFACES = 0

                g_vColorTint = [1.0, 1.0, 1.0, 1.0]
                g_flFresnelExponent = 1.5
                g_flFresnelFalloff = 5.0
                g_flFresnelMax = 0.0
                g_flFresnelMin = 1.0
                g_flColorBoost = 20.0
                g_flOpacityScale = 0.6

                g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
                g_tMask1 = resource:"materials/default/default_mask_tga_344101f8.vtex"
                g_tMask2 = resource:"materials/default/default_mask_tga_344101f8.vtex"
                g_tMask3 = resource:"materials/default/default_mask_tga_344101f8.vtex"
            })#";
        constexpr char glow_ignorez[] = R"#(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d}
            format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
            {
                shader = "csgo_effects.vfx"

                F_ADDITIVE_BLEND = 1
                F_BLEND_MODE = 1
                F_TRANSLUCENT = 1
                F_IGNOREZ = 1
                F_DISABLE_Z_BUFFERING = 1
                F_DISABLE_Z_WRITE = 1
                F_RENDER_BACKFACES = 0

                g_vColorTint = [1.0, 1.0, 1.0, 1.0]
                g_flFresnelExponent = 1.5
                g_flFresnelFalloff = 5.0
                g_flFresnelMax = 0.0
                g_flFresnelMin = 1.0
                g_flColorBoost = 20.0
                g_flOpacityScale = 0.6

                g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
                g_tMask1 = resource:"materials/default/default_mask_tga_344101f8.vtex"
                g_tMask2 = resource:"materials/default/default_mask_tga_344101f8.vtex"
                g_tMask3 = resource:"materials/default/default_mask_tga_344101f8.vtex"
            })#";
        constexpr char hologram[] = R"#(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d}
            format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
            {
                shader = "csgo_complex.vfx"

                F_SELF_ILLUM = 1
                F_RENDER_BACKFACES = 1
                F_TRANSLUCENT = 1

                g_vColorTint = [0.0, 0.0, 0.0]
                g_flModelTintAmount = 1.0
                g_flOpacityScale = 0.6

                g_flSelfIllumBrightness = 4.5
                g_flSelfIllumScale = 2.0
                g_vSelfIllumTint = [0.45, 0.85, 1.0]
                g_flSelfIllumAlbedoFactor = 0.55
                g_vSelfIllumScrollSpeed = [0.0, 0.35]

                g_vTexCoordScale = [0.75, 9.0]

                g_bFogEnabled = 0

                g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
                g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
                g_tSelfIllumMask = resource:"materials/dev/water_waves.vtex"
                g_tAmbientOcclusion = resource:"materials/default/default_mask_tga_fde710a5.vtex"
                g_tTintMask = resource:"materials/default/default_mask_tga_fde710a5.vtex"
            })#";
        constexpr char hologram_ignorez[] = R"#(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d}
            format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
            {
                shader = "csgo_complex.vfx"

                F_SELF_ILLUM = 1
                F_RENDER_BACKFACES = 1
                F_TRANSLUCENT = 1
                F_DISABLE_Z_BUFFERING = 1

                g_vColorTint = [0.0, 0.0, 0.0]
                g_flModelTintAmount = 1.0
                g_flOpacityScale = 0.6

                g_flSelfIllumBrightness = 4.5
                g_flSelfIllumScale = 2.0
                g_vSelfIllumTint = [0.45, 0.85, 1.0]
                g_flSelfIllumAlbedoFactor = 0.55
                g_vSelfIllumScrollSpeed = [0.0, 0.35]

                g_vTexCoordScale = [0.75, 9.0]

                g_bFogEnabled = 0

                g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
                g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
                g_tSelfIllumMask = resource:"materials/dev/water_waves.vtex"
                g_tAmbientOcclusion = resource:"materials/default/default_mask_tga_fde710a5.vtex"
                g_tTintMask = resource:"materials/default/default_mask_tga_fde710a5.vtex"
            })#";
        constexpr char distortion[] = R"#(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d}
            format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
            {
                shader = "csgo_effects.vfx"

                F_ADDITIVE_BLEND = 1
                F_BLEND_MODE = 1
                F_TRANSLUCENT = 1
                F_RENDER_BACKFACES = 1

                g_vColorTint = [1.0, 1.0, 1.0, 1.0]
                g_flOpacityScale = 0.85
                g_flFresnelExponent = 1.25
                g_flFresnelFalloff = 2.25
                g_flFresnelMax = 0.32
                g_flFresnelMin = 1.0
                g_flColorBoost = 14.0
                g_vTexCoordScrollSpeed = [0.24, 0.17]
                g_vTexCoordScale = [2.75, 2.75]
                g_flToolsVisCubemapReflectionRoughness = 1.0
                g_flBeginMixingRoughness = 1.0

                g_tColor = resource:"materials/dev/water_waves.vtex"
                g_tMask1 = resource:"materials/dev/water_waves.vtex"
                g_tMask2 = resource:"materials/default/default_mask_tga_344101f8.vtex"
                g_tMask3 = resource:"materials/default/default_mask_tga_344101f8.vtex"
                g_tSceneDepth = resource:"materials/default/default_mask_tga_fde710a5.vtex"
            })#";
        constexpr char distortion_ignorez[] = R"#(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d}
            format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
            {
                shader = "csgo_effects.vfx"

                F_ADDITIVE_BLEND = 1
                F_BLEND_MODE = 1
                F_TRANSLUCENT = 1
                F_RENDER_BACKFACES = 1
                F_DISABLE_Z_BUFFERING = 1
                F_DISABLE_Z_WRITE = 1

                g_vColorTint = [1.0, 1.0, 1.0, 1.0]
                g_flOpacityScale = 0.85
                g_flFresnelExponent = 1.25
                g_flFresnelFalloff = 2.25
                g_flFresnelMax = 0.32
                g_flFresnelMin = 1.0
                g_flColorBoost = 14.0
                g_vTexCoordScrollSpeed = [0.24, 0.17]
                g_vTexCoordScale = [2.75, 2.75]
                g_flToolsVisCubemapReflectionRoughness = 1.0
                g_flBeginMixingRoughness = 1.0

                g_tColor = resource:"materials/dev/water_waves.vtex"
                g_tMask1 = resource:"materials/dev/water_waves.vtex"
                g_tMask2 = resource:"materials/default/default_mask_tga_344101f8.vtex"
                g_tMask3 = resource:"materials/default/default_mask_tga_344101f8.vtex"
                g_tSceneDepth = resource:"materials/default/default_mask_tga_fde710a5.vtex"
            })#";
        constexpr char pearl[] = R"#(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d}
            format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
            {
                shader = "csgo_character.vfx"

                F_IRIDESCENCE = 1
                F_CLOTH_SHADING = 1
                F_RENDER_BACKFACES = 1
                F_DISABLE_Z_PREPASS = 1

                g_vColorTint = [1.0, 1.0, 1.0]
                g_flModelTintAmount = 1.0
                g_flOpacityScale = 1.0

                g_vTexCoordScale = [1.0, 1.0]

                g_flIridescentStrength = 3.5
                g_flIridescentFresnelStrength = 4.0
                g_flIridescentHueShift = 1.0

                g_flSheenScale = 6.0
                g_flSheenTintColor = [1.0, 1.0, 1.0]

                g_fContrast = 0.35
                g_fBrightness = 1.25
                g_fSaturation = 1.6

                g_flAmbientOcclusionMasking = 0.0
                g_bFogEnabled = 0

                g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
                g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
                g_tMetalness = resource:"materials/default/default_mask_tga_344101f8.vtex"
                g_tAmbientOcclusion = resource:"materials/default/default_mask_tga_fde710a5.vtex"
                g_tIridescentThickness_Mask = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
            })#";
        struct Api { std::uintptr_t alloc{}, destroy{}, load{}, create{}; } api;
        struct Slot
        {
            std::atomic<std::uintptr_t> material{};
            std::uintptr_t binding{};
            bool attempted{};
        };
        constexpr auto materialCount = static_cast<std::size_t>(Material::Count);
        std::array<Slot, materialCount * 2> slots;
        std::mutex resourceMutex;
        std::atomic<bool> supported{};
        const auto session = GetTickCount64();
        std::uint64_t nextId{};
        std::size_t Index(Material material, bool ignoreDepth)
        {
            const auto index = static_cast<std::size_t>(material);
            return index < materialCount ? index + (ignoreDepth ? materialCount : 0) : slots.size();
        }
        __declspec(noinline) void Destroy(cstypes::key_values3 &tree)
        {
            __try { memory::call<void>(api.destroy, &tree, 0u); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        __declspec(noinline) std::uintptr_t Create(const char *source, const char *name, std::uintptr_t &binding)
        {
            constexpr cstypes::kv3_id id{"generic", 0x41B818518343427E, 0xB5F447C23C0CDF8C};
            cstypes::key_values3 tree{};
            cstypes::strong_handle handle{};
            bool allocated{}, loaded{};
            __try
            {
                allocated = memory::call<cstypes::key_values3 *>(api.alloc, &tree, 1u, 6u) == &tree;
                if (allocated)
                {
                    loaded = memory::call<bool>(api.load, &tree, nullptr, source, &id, nullptr, 0u);
                    if (loaded) memory::call<void *>(api.create, nullptr, &handle, name, &tree, 0, true);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { loaded = false; }
            if (allocated) Destroy(tree);
            binding = reinterpret_cast<std::uintptr_t>(handle.binding);
            return loaded ? memory::read<std::uintptr_t>(binding) : 0;
        }
    }
    const char *Name(Material material)
    {
        static constexpr const char *names[] = {"Liquid", "Metallic", "Matte", "Flat", "Bloom", "Outlines", "Glow", "Electric", "Distortion", "Hologram", "Pearl"};
        const auto index = static_cast<std::size_t>(material);
        return index < materialCount ? names[index] : "Flat";
    }
    std::string Source(Material material, bool ignoreDepth)
    {
        const char *visible[] = {liquid, metallic, matte, flat, bloom, outlines, glow, electric, distortion, hologram, pearl};
        const char *hidden[] = {liquid_ignorez, metallic, matte_ignorez, flat_ignorez, bloom_ignorez, outlines_ignorez, glow_ignorez, electric, distortion_ignorez, hologram_ignorez, pearl};
        auto index = static_cast<std::size_t>(material);
        if (index >= materialCount) index = static_cast<std::size_t>(Material::Flat);
        std::string source = ignoreDepth ? hidden[index] : visible[index];
        if (ignoreDepth)
        {
            const auto setFlag = [&](std::string_view flag)
            {
                const auto existing = source.find(flag);
                if (existing != std::string::npos)
                {
                    const auto equal = source.find('=', existing + flag.size());
                    const auto value = equal == std::string::npos ? std::string::npos : source.find_first_not_of(" \t", equal + 1);
                    if (value != std::string::npos && (source[value] == '0' || source[value] == '1')) source[value] = '1';
                }
                else
                {
                    const auto header = source.find("-->");
                    const auto body = source.find('{', header == std::string::npos ? 0 : header + 3);
                    if (body != std::string::npos) source.insert(body + 1, "\n    " + std::string(flag) + " = 1\n");
                }
            };
            setFlag("F_DISABLE_Z_BUFFERING");
            setFlag("F_DISABLE_Z_WRITE");
        }
        return source;
    }
    bool Initialize()
    {
        std::lock_guard lock(resourceMutex);
        supported = false;
        api = {};
        if (!game::module_matches("client.dll", skin_bindings::kClientSha256) ||
            !game::module_matches("scenesystem.dll", chams_bindings::kSceneSha256) ||
            !game::module_matches("materialsystem2.dll", chams_bindings::kMaterialSha256) ||
            !game::module_matches("tier0.dll", chams_bindings::kTier0Sha256)) return false;
        const auto scene = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"scenesystem.dll"));
        const auto &entry = chams_bindings::kGeneratePrimitives;
        std::array<std::uint8_t, 24> bytes{};
        if (!memory::copy(bytes.data(), scene + entry.rva, bytes.size()) || bytes != entry.entry ||
            PATTERN(patterns::generate_primitives) != scene + entry.rva) return false;
        for (const auto vtable : chams_bindings::kModelDescriptorVtables)
            if (memory::read<std::uintptr_t>(scene + vtable + chams_bindings::kGeneratePrimitivesSlot * sizeof(std::uintptr_t)) !=
                scene + entry.rva) return false;
        api.alloc = PATTERN(patterns::kv3_alloc);
        api.destroy = PATTERN(patterns::kv3_destroy);
        api.create = PATTERN(patterns::material_create);
        api.load = reinterpret_cast<std::uintptr_t>(GetProcAddress(GetModuleHandleW(L"tier0.dll"), chams_bindings::kLoadKv3));
        supported = api.alloc && api.destroy && api.load && api.create;
        for (auto &slot : slots) if (!slot.binding) slot.attempted = false;
        return supported;
    }
    void Ensure(Material material, bool ignoreDepth)
    {
        const auto index = Index(material, ignoreDepth);
        if (!supported || index >= slots.size() || slots[index].material) return;
        std::lock_guard lock(resourceMutex);
        auto &slot = slots[index];
        if (!supported || slot.material) return;
        if (slot.binding)
        {
            slot.material = memory::read<std::uintptr_t>(slot.binding);
            return;
        }
        if (slot.attempted) return;
        slot.attempted = true;
        const auto source = Source(material, ignoreDepth);
        char name[128]{};
        std::snprintf(name, sizeof(name), "materials/dev/cs2_internal_chams_%llx_%llu.vmat",
                      static_cast<unsigned long long>(session), static_cast<unsigned long long>(++nextId));
        slot.material = Create(source.c_str(), name, slot.binding);
        // Engine strong handles retain at most one material per style/depth pair.
    }
    std::uintptr_t Find(Material material, bool ignoreDepth)
    {
        const auto index = Index(material, ignoreDepth);
        return supported && index < slots.size() ? slots[index].material.load() : 0;
    }
    std::uintptr_t CreateCached(const std::string& key,const std::string& source)
    {
        std::lock_guard lock(resourceMutex);if(!supported)return 0;
        struct Cached{std::uintptr_t binding{},material{};};
        static std::unordered_map<std::string,Cached> cache;
        if(auto found=cache.find(key);found!=cache.end())return found->second.material;
        if(cache.size()>=32)return 0;
        Cached result;const auto name="materials/dev/cs2_image_"+std::to_string(session)+"_"+std::to_string(++nextId)+".vmat";
        result.material=Create(source.c_str(),name.c_str(),result.binding);
        if(result.material)cache.emplace(key,result);return result.material;
    }
    void Reset()
    {
        std::lock_guard lock(resourceMutex);
        for (auto &slot : slots) if (!slot.binding) slot.attempted = false;
    }
    void Shutdown()
    {
        std::lock_guard lock(resourceMutex);
        supported = false;
    }
}
