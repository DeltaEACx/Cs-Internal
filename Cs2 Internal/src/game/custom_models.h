#pragma once
#include <array>
#include <string_view>

namespace custom_models
{
    struct Model
    {
        int definition;
        const char *id;
        const char *name;
        const char *style;
        const char *weapon;
        const char *path;
        const char *previewKey;
        const char *previewFile;
    };

    inline constexpr std::array<Model, 17> kModels{{
        {7, "ak74_wings", "AK-74 Wings", "Fantasy", "AK-47", "models/custom/ak74_wings/ak74_wings.vmdl", "__ak74_wings",
         "custom_models/ak74_wings/ak74_wings_preview.png"},
        {7, "ak74_tactical", "AK-74 Tactical", "Realistic", "AK-47",
         "models/custom/mixed_pack/ak74_tactical/ak74_tactical.vmdl", "__custom_ak74_tactical",
         "custom_models/mixed_pack/ak74_tactical/preview.png"},
        {7, "ak74_compact", "AK Vector Compact", "Sci-fi", "AK-47",
         "models/custom/mixed_pack/ak74_compact/ak74_compact.vmdl", "__custom_ak74_compact",
         "custom_models/mixed_pack/ak74_compact/preview.png"},
        {7, "ak_dragon", "AK Dragon", "Fantasy", "AK-47", "models/custom/mixed_pack/ak_dragon/ak_dragon.vmdl",
         "__custom_ak_dragon", "custom_models/mixed_pack/ak_dragon/preview.png"},
        {60, "m4_carbine", "M4 Lynx Carbine", "Realistic", "M4A1-S",
         "models/custom/mixed_pack/m4_carbine/m4_carbine.vmdl", "__custom_m4_carbine",
         "custom_models/mixed_pack/m4_carbine/preview.png"},
        {60, "m4_ion", "M4 Ion", "Sci-fi", "M4A1-S", "models/custom/mixed_pack/m4_ion/m4_ion.vmdl", "__custom_m4_ion",
         "custom_models/mixed_pack/m4_ion/preview.png"},
        {60, "m4_seraph", "M4 Seraph", "Fantasy", "M4A1-S", "models/custom/mixed_pack/m4_seraph/m4_seraph.vmdl",
         "__custom_m4_seraph", "custom_models/mixed_pack/m4_seraph/preview.png"},
        {9, "awp_arctic", "AWP Arctic", "Realistic", "AWP", "models/custom/mixed_pack/awp_arctic/awp_arctic.vmdl",
         "__custom_awp_arctic", "custom_models/mixed_pack/awp_arctic/preview.png"},
        {9, "awp_rail", "AWP Rail Lance", "Sci-fi", "AWP", "models/custom/mixed_pack/awp_rail/awp_rail.vmdl",
         "__custom_awp_rail", "custom_models/mixed_pack/awp_rail/preview.png"},
        {9, "awp_crystal", "AWP Crystal", "Fantasy", "AWP", "models/custom/mixed_pack/awp_crystal/awp_crystal.vmdl",
         "__custom_awp_crystal", "custom_models/mixed_pack/awp_crystal/preview.png"},
        {1, "deagle_match", "Desert Eagle Match", "Realistic", "Desert Eagle",
         "models/custom/mixed_pack/deagle_match/deagle_match.vmdl", "__custom_deagle_match",
         "custom_models/mixed_pack/deagle_match/preview.png"},
        {1, "deagle_fusion", "Desert Eagle Fusion", "Sci-fi", "Desert Eagle",
         "models/custom/mixed_pack/deagle_fusion/deagle_fusion.vmdl", "__custom_deagle_fusion",
         "custom_models/mixed_pack/deagle_fusion/preview.png"},
        {1, "deagle_obsidian", "Desert Eagle Obsidian", "Fantasy", "Desert Eagle",
         "models/custom/mixed_pack/deagle_obsidian/deagle_obsidian.vmdl", "__custom_deagle_obsidian",
         "custom_models/mixed_pack/deagle_obsidian/preview.png"},
        {100, "fatality", "Fatality", "Terror", "Knife", "models/player/fatality/fatality.mdl", "__fatality",
         "custom_models/fatality/fatality_preview.png"},
        {101, "kolka", "Kolka", "Counter-Terror", "Knife", "models/player/kolka/kolka.mdl", "__kolka",
         "custom_models/kolka/kolka_preview.png"},
        {102, "madebyruzh", "MadebyRuzh", "Terror", "Knife", "models/player/madebyruzh/madebyruzh.mdl", "__madebyruzh",
         "custom_models/madebyruzh/madebyruzh_preview.png"},
        {500, "banana", "Banana Knife", "Fun", "Knife", "models/custom/banana/banana.vmdl", "__banana",
         "custom_models/banana/preview.png"},
    }};

    inline constexpr const Model *Find(std::string_view path)
    {
        for (const auto &model : kModels)
            if (path == model.path)
                return &model;
        return nullptr;
    }
    inline constexpr const Model *FindPreview(std::string_view key)
    {
        for (const auto &model : kModels)
            if (key == model.previewKey)
                return &model;
        return nullptr;
    }
    inline constexpr bool IsKnife(int definition)
    {
        constexpr int definitions[] = {41,  42,  59,  500, 503, 505, 506, 507, 508, 509, 512, 514,
                                       515, 516, 517, 518, 519, 520, 521, 522, 523, 525, 526};
        for (int id : definitions)
            if (id == definition)
                return true;
        return false;
    }

    // Verified against item_catalog.json model_player/model_world for each definition.
    inline constexpr const char *StockModel(int definition)
    {
        switch (definition)
        {
        case 7:
            return "weapons/models/ak47/weapon_rif_ak47.vmdl";
        case 60:
            return "weapons/models/m4a1_silencer/weapon_rif_m4a1_silencer.vmdl";
        case 9:
            return "weapons/models/awp/weapon_snip_awp.vmdl";
        case 1:
            return "weapons/models/deagle/weapon_pist_deagle.vmdl";
        case 41:
            return "weapons/models/knife/knife_default_t/weapon_knife_default_t.vmdl";
        case 42:
            return "weapons/models/knife/knife_default_ct/weapon_knife_default_ct.vmdl";
        case 59:
            return "weapons/models/knife/knife_default_t/weapon_knife_default_t.vmdl";
        case 500:
            return "weapons/models/knife/knife_bayonet/weapon_knife_bayonet.vmdl";
        case 503:
            return "weapons/models/knife/knife_css/weapon_knife_css.vmdl";
        case 505:
            return "weapons/models/knife/knife_flip/weapon_knife_flip.vmdl";
        case 506:
            return "weapons/models/knife/knife_gut/weapon_knife_gut.vmdl";
        case 507:
            return "weapons/models/knife/knife_karambit/weapon_knife_karambit.vmdl";
        case 508:
            return "weapons/models/knife/knife_m9/weapon_knife_m9.vmdl";
        case 509:
            return "weapons/models/knife/knife_tactical/weapon_knife_tactical.vmdl";
        case 512:
            return "weapons/models/knife/knife_falchion/weapon_knife_falchion.vmdl";
        case 514:
            return "weapons/models/knife/knife_bowie/weapon_knife_bowie.vmdl";
        case 515:
            return "weapons/models/knife/knife_butterfly/weapon_knife_butterfly.vmdl";
        case 516:
            return "weapons/models/knife/knife_push/weapon_knife_push.vmdl";
        case 517:
            return "weapons/models/knife/knife_cord/weapon_knife_cord.vmdl";
        case 518:
            return "weapons/models/knife/knife_canis/weapon_knife_canis.vmdl";
        case 519:
            return "weapons/models/knife/knife_ursus/weapon_knife_ursus.vmdl";
        case 520:
            return "weapons/models/knife/knife_navaja/weapon_knife_navaja.vmdl";
        case 521:
            return "weapons/models/knife/knife_outdoor/weapon_knife_outdoor.vmdl";
        case 522:
            return "weapons/models/knife/knife_stiletto/weapon_knife_stiletto.vmdl";
        case 523:
            return "weapons/models/knife/knife_talon/weapon_knife_talon.vmdl";
        case 525:
            return "weapons/models/knife/knife_skeleton/weapon_knife_skeleton.vmdl";
        case 526:
            return "weapons/models/knife/knife_kukri/weapon_knife_kukri.vmdl";
        default:
            return nullptr;
        }
    }
} // namespace custom_models
