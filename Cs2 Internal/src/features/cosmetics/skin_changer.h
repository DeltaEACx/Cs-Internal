#pragma once
#include <string>
#include <cstdint>
#include <vector>
#include "render/preview_material.h"

namespace skin_changer
{
    struct Selection
    {
        int definition = 0, paintkit = 0, kills = 0, seed = 0;
        float wear = 0.01f;
        bool statTrak = false, legacy = false, knife = false, customModel = false, agentModel = false, gloves = false;
        int team = 0;

        std::string model, name;
    };
    struct EnemyModelSelection
    {
        bool enabled = false;
        std::string model, name;
    };
    bool SaveEnemyModel(const EnemyModelSelection &selection);
    EnemyModelSelection GetEnemyModel();
    std::string EnemyModelStatus();
    // UI operations only enqueue settings. Game calls run after post-data update.
    bool Save(const Selection &selection);
    bool GetSelection(int definition, bool knife, Selection &selection, std::uint64_t *revision = nullptr);
    bool GetCosmetic(bool agent, int team, Selection &selection);
    std::string CosmeticStatus(bool agent, int team = 0);
    int CurrentTeam();
    int ReadPaintKit(std::uintptr_t weapon); // Game-thread snapshot only.
    bool ReadPreviewMaterials(std::uintptr_t weapon, std::vector<preview_scene::MaterialTexture> &textures);
    std::string Status(int definition = 0, bool knife = false);
    bool CustomModelAvailable(const std::string &model);
    bool Install();
    void SetStageCallback(void (*callback)());
    void SetBeforeStageCallback(void (*callback)(int));
    void SetAfterStageCallback(void (*callback)(int));
    bool Stop();
} // namespace skin_changer
