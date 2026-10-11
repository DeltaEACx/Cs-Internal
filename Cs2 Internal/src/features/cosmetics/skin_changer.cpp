#include "platform/windows.h"
#include "features/cosmetics/skin_changer.h"
#include "core/runtime_paths.h"
#include "core/module_digest.hpp"
#include "game/character_catalog.h"
#include "game/game_access.h"
#include "game/custom_models.h"
#include "game/sdk/SkinBindings.h"
#include "game/sdk/EconBindings.h"
#include "game/sdk/PreviewBindings.h"
#include "game/sdk/client_dll.hpp"
#include "game/sdk/offsets.hpp"
#include "features/visuals/engine.hpp"
#include "minhook/include/MinHook.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    namespace field = cs2_dumper::schemas::client_dll;
    namespace global = cs2_dumper::offsets::client_dll;
    using Address = std::uintptr_t;
    using StageFn = void(__fastcall *)(void *, int);
    using AttributeFn = void(__fastcall *)(void *, const char *, float);
    using ItemFn = void(__fastcall *)(void *);
    using CompositeFn = void(__fastcall *)(void *, bool);
    using PaintFn = int(__fastcall *)(void *);
    using MeshFn = void(__fastcall *)(void *, const std::uint64_t *);
    using ModelFn = void(__fastcall *)(void *, const char *);
    using PostDataFn = void(__fastcall *)(void *, int);
    using BodygroupFn = void(__fastcall *)(void *, int, unsigned);
    using FindModelFn = void *(__fastcall *)(void *, Address *, const char *);
    using InsertFn = const char *(__fastcall *)(void *, int, const char *, int, bool);
    using PurgeFn = void(__fastcall *)(void *, int);
    using PrecacheFn = void *(__fastcall *)(void *, void *, const char *);
    StageFn originalStage = nullptr;
    AttributeFn setAttribute = nullptr;
    ItemFn invalidateItem = nullptr, updateSubclass = nullptr;
    CompositeFn updateOwner = nullptr, updateSet = nullptr;
    PaintFn getPaintKit = nullptr;
    MeshFn setMesh = nullptr;
    ModelFn setModel = nullptr;
    PostDataFn playerPostDataUpdate = nullptr;
    BodygroupFn setBodygroup = nullptr;
    constexpr std::uint64_t kGloveItemId = 0xf000000000000010ull;
    InsertFn insertString = nullptr;
    PurgeFn purgeString = nullptr;
    PrecacheFn precacheResource = nullptr;
    void *resourceSystem = nullptr;
    Address clientBase = 0;
    Address previewMaterialBase = 0;
    void *hookTarget = nullptr;
    bool hookCreated = false;
    std::atomic<bool> enabled{false}, faulted{false};
    std::atomic<unsigned> callbacks{0};
    std::atomic<void (*)()> stageCallback{nullptr};
    std::atomic<void (*)(int)> beforeStageCallback{nullptr};
    std::atomic<void (*)(int)> afterStageCallback{nullptr};
    std::atomic<int> currentTeam{0};
    std::mutex settingsMutex;
    struct Saved
    {
        skin_changer::Selection choice;
        std::uint64_t revision = 0;
        std::string status;
    };
    std::unordered_map<int, Saved> selections;
    struct CosmeticState
    {
        Address pawn = 0;
        std::uint32_t handle = 0;
        std::uint64_t revision = 0;
        ULONGLONG started = 0, lastRequest = 0;
        unsigned requests = 0;
        bool failed = false, confirmed = false;
    };
    std::unordered_map<int, CosmeticState> cosmeticStates;
    skin_changer::EnemyModelSelection enemySelection;
    std::uint64_t enemyRevision = 0, enemyPrecacheRevision = 0;
    std::string enemyStatus = "Disabled";
    std::atomic<bool> enemyStopping{false};
    std::atomic<unsigned> enemyChanges{0}, enemyCallbacks{0};
    struct EnemyModelState
    {
        Address list = 0, pawn = 0, scene = 0, writtenModel = 0;
        std::uint32_t handle = 0xffffffffu;
        std::uint64_t revision = 0;
        std::string original, replacement;
        ULONGLONG started = 0, lastRequest = 0;
        unsigned requests = 0;
        bool owned = false, confirmed = false, failed = false, restoring = false;
    };
    // Only the frame-stage game thread accesses this map. Full handles and scene
    // ownership prevent a restore from touching a recycled entity or new scene.
    std::unordered_map<std::uint32_t, EnemyModelState> enemyModels;
    bool enemyPrecached = false;
    std::uint64_t nextRevision = 0;
    std::string backendStatus = "Skin updater has not started";
    std::string bindingFailure;
    struct Applied
    {
        Address weapon = 0, pawn = 0, hud = 0;
        std::uint64_t revision = 0;
        ULONGLONG lastAttempt = 0;
        bool failed = false;
        Address modelHandle = 0;
        ULONGLONG loadStarted = 0;
    };
    // Game-thread only. Full handles distinguish recycled entity slots.
    std::unordered_map<std::uint32_t, Applied> applied;
    ULONGLONG nextTick = 0;

    __declspec(noinline) bool Copy(Address address, void *output, std::size_t size)
    {
        if (address < 0x10000 || address >= 0x00007fffffff0000ull || size > 0x00007fffffff0000ull - address)
            return false;
        __try
        {
            std::memcpy(output, reinterpret_cast<void *>(address), size);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
    template <class T> bool Read(Address address, T &value)
    {
        return Copy(address, &value, sizeof(value));
    }
    // Writes are used only inside ApplyGuarded on the game thread.
    template <class T> void Write(Address address, const T &value)
    {
        *reinterpret_cast<T *>(address) = value;
    }

    Address Resolve(Address list, std::uint32_t handle)
    {
        if (!list || handle == 0xffffffffu || !(handle & 0x7fff) || (handle & 0x7fff) >= 0x7fff)
            return 0;
        const auto index = handle & 0x7fff;
        Address page = 0, entity = 0;
        std::uint32_t actual = 0;
        if (!Read(list + 0x10 + 8 * (index >> 9), page) || !page ||
            !Read(page + 0x70 * (index & 0x1ff) + 0x10, actual) || actual != handle ||
            !Read(page + 0x70 * (index & 0x1ff), entity) || entity < 0x10000)
            return 0;
        return entity;
    }
    bool IsKnife(int definition)
    {
        return custom_models::IsKnife(definition);
    }
    int Key(int definition, bool knife)
    {
        return knife ? -1 : definition;
    }
    int CosmeticKey(bool agent, int team)
    {
        return agent ? -10 - team : -20;
    }
    constexpr const char *kDefaultAgentModel = "models/custom/agents/silver_casual/silver_casual.vmdl";
    constexpr const char *kDefaultAgentName = "Jeffrey Epstein";
    constexpr std::array<const char *, 7> kDefaultAgentResources{
        "models/custom/agents/silver_casual/silver_casual.vmdl_c",
        "materials/custom/agents/silver_casual/silver_casual.vmat_c",
        "materials/custom/agents/silver_casual/silver_casual_head.vmat_c",
        "materials/custom/agents/silver_casual/turnaround_png_25379ab0.vtex_c",
        "materials/custom/agents/silver_casual/head_detail_color_png_9a8aedd3.vtex_c",
        "materials/custom/agents/silver_casual/normal_png_f680e532.vtex_c",
        "materials/custom/agents/silver_casual/ao_png_bcf1f3db.vtex_c"
    };
    bool DefaultAgentAvailable()
    {
        const auto game = runtime_paths::GameDirectory();
        if (game.empty() || !character_catalog::Installed(kDefaultAgentModel))
            return false;
        for (const auto *resource : kDefaultAgentResources)
        {
            const auto path = runtime_paths::NativePath(game / resource);
            std::error_code error;
            if (!std::filesystem::is_regular_file(path, error) || error ||
                std::filesystem::file_size(path, error) == 0 || error)
                return false;
        }
        return true;
    }
    void SeedDefaultAgentSelections()
    {
        // Keep the stock agents if the packaged model or one of its material
        // resources is missing. Seeding only queues settings; the frame hook
        // performs the normal precache and asynchronous model assignment.
        if (!DefaultAgentAvailable())
            return;
        std::lock_guard lock(settingsMutex);
        for (const int team : {2, 3})
        {
            const auto key = CosmeticKey(true, team);
            if (selections.contains(key))
                continue;
            skin_changer::Selection choice;
            choice.agentModel = choice.customModel = true;
            choice.team = team;
            choice.model = kDefaultAgentModel;
            choice.name = kDefaultAgentName;
            selections.emplace(key, Saved{std::move(choice), ++nextRevision, "Saved; waiting for player"});
        }
        // A revision also records an explicit disable, which must not be
        // reversed by a hook reinstall or a later settings refresh.
        if (!enemyRevision)
        {
            enemySelection = {true, kDefaultAgentModel, kDefaultAgentName};
            enemyRevision = ++nextRevision;
            enemyStatus = "Saved; waiting for enemies";
        }
    }
    bool IsGlove(int definition)
    {
        return definition == 4725 || (definition >= 5027 && definition <= 5035);
    }
    Address ItemView(Address weapon)
    {
        return weapon + field::C_EconEntity::m_AttributeManager + field::C_AttributeContainer::m_Item;
    }
    Address ModelHandle(Address entity)
    {
        Address scene = 0, handle = 0;
        if (entity && Read(entity + field::C_BaseEntity::m_pGameSceneNode, scene) && scene)
            Read(scene + field::CSkeletonInstance::m_modelState + field::CModelState::m_hModel, handle);
        return handle;
    }
    std::string ModelPath(Address handle)
    {
        Address resource = 0, name = 0;
        if (!handle || !Read(handle, resource) || !resource ||
            !Read(resource + skin_bindings::kModelNameOffset, name) || !name)
            return {};
        char actual[512]{};
        for (std::size_t i = 0; i < sizeof(actual); ++i)
        {
            if (!Read(name + i, actual[i]))
                return {};
            if (!actual[i])
                return std::string(actual, i);
        }
        return {};
    }
    bool ModelHasPath(Address handle, const char *path)
    {
        // Check the loaded resource, not the entity's replicated model name.
        const auto actual = ModelPath(handle);
        return !actual.empty() && game_access::SameModelPath(actual, path);
    }
    bool CustomModelAssigned(Address entity, Address expectedModel)
    {
        Address scene = 0;
        std::uint64_t mask = 0;
        return expectedModel && ModelHandle(entity) == expectedModel &&
               Read(entity + field::C_BaseEntity::m_pGameSceneNode, scene) && scene &&
               Read(scene + field::CSkeletonInstance::m_modelState + field::CModelState::m_MeshGroupMask, mask) &&
               (mask & 1) != 0;
    }
    __declspec(noinline) Address FindModel(const char *path)
    {
        __try
        {
            Address system = 0, table = 0, function = 0, handle = 0;
            if (!Read(clientBase + skin_bindings::kModelSystemPointerRva, system) || !system || !Read(system, table) ||
                !table || !Read(table + skin_bindings::kFindModelVtableSlot * sizeof(Address), function) || !function)
                return 0;
            reinterpret_cast<FindModelFn>(function)(reinterpret_cast<void *>(system), &handle, path);
            return ModelHasPath(handle, path) ? handle : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }
    struct BufferString
    {
        int length = 0;
        std::uint32_t capacity = 0xc0000008u;
        char *storage = nullptr;
    };
    static_assert(sizeof(BufferString) == 16);
    __declspec(noinline) bool PrecacheCustomModel(const char *path)
    {
        if (!insertString || !purgeString || !precacheResource || !resourceSystem)
            return false;
        BufferString name;
        __try
        {
            __try
            {
                insertString(&name, 0, path, -1, false);
                precacheResource(resourceSystem, &name, "");
            }
            __finally
            {
                purgeString(&name, 0);
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    struct Held
    {
        Address list = 0, pawn = 0, weapon = 0;
        std::uint32_t handle = 0;
        std::uint16_t definition = 0;
    };
    bool ReadHeld(Held &held)
    {
        Address controller = 0, services = 0;
        std::uint32_t pawnHandle = 0xffffffffu, owner = 0xffffffffu;
        int health = 0;
        std::uint8_t life = 1;
        if (!clientBase || !Read(clientBase + global::dwEntityList, held.list) || !held.list ||
            !Read(clientBase + global::dwLocalPlayerController, controller) || !controller ||
            !Read(controller + field::CCSPlayerController::m_hPlayerPawn, pawnHandle) ||
            !(held.pawn = Resolve(held.list, pawnHandle)) ||
            !Read(held.pawn + field::C_BaseEntity::m_iHealth, health) || health <= 0 ||
            !Read(held.pawn + field::C_BaseEntity::m_lifeState, life) || life != 0 ||
            !Read(held.pawn + field::C_BasePlayerPawn::m_pWeaponServices, services) || !services ||
            !Read(services + field::CPlayer_WeaponServices::m_hActiveWeapon, held.handle) ||
            !(held.weapon = Resolve(held.list, held.handle)) ||
            !Read(held.weapon + field::C_BaseEntity::m_hOwnerEntity, owner) || Resolve(held.list, owner) != held.pawn ||
            !Read(ItemView(held.weapon) + field::C_EconItemView::m_iItemDefinitionIndex, held.definition))
            return false;
        return held.definition != 0;
    }
    Address HudWeapon(const Held &held)
    {
        std::uint32_t handle = 0xffffffffu;
        Address scene = 0, node = 0;
        if (!Read(held.pawn + field::C_CSPlayerPawn::m_hHudModelArms, handle))
            return 0;
        const Address arms = Resolve(held.list, handle);
        if (!arms || !Read(arms + field::C_BaseEntity::m_pGameSceneNode, scene) || !scene ||
            !Read(scene + field::CGameSceneNode::m_pChild, node))
            return 0;
        Address visited[64]{};
        for (int index = 0; node && index < 64; ++index)
        {
            for (int previous = 0; previous < index; ++previous)
                if (visited[previous] == node)
                    return 0;
            visited[index] = node;
            Address entity = 0;
            if (Read(node + field::CGameSceneNode::m_pOwner, entity) && entity &&
                Read(entity + field::C_BaseEntity::m_hOwnerEntity, handle) && Resolve(held.list, handle) == held.weapon)
                return entity;
            if (!Read(node + field::CGameSceneNode::m_pNextSibling, node))
                return 0;
        }
        return 0;
    }
    std::uint32_t SubclassHash(int definition)
    {
        char key[16]{};
        const int length = sprintf_s(key, "%d", definition);
        std::uint32_t result = 0x31415926u ^ length;
        const auto *data = reinterpret_cast<const unsigned char *>(key);
        int remaining = length;
        while (remaining >= 4)
        {
            std::uint32_t value = 0;
            std::memcpy(&value, data, 4);
            value *= 0x5bd1e995u;
            value ^= value >> 24;
            value *= 0x5bd1e995u;
            result = result * 0x5bd1e995u ^ value;
            data += 4;
            remaining -= 4;
        }
        switch (remaining)
        {
        case 3:
            result ^= data[2] << 16;
            [[fallthrough]];
        case 2:
            result ^= data[1] << 8;
            [[fallthrough]];
        case 1:
            result ^= data[0];
            result *= 0x5bd1e995u;
        }
        result ^= result >> 13;
        result *= 0x5bd1e995u;
        result ^= result >> 15;
        return result;
    }
    Address FindAttribute(Address item, std::uint16_t definition)
    {
        const Address list = item + field::C_EconItemView::m_AttributeList;
        int count = 0;
        Address entries = 0;
        if (!Read(list + 8, count) || count < 0 || count > 128 || !Read(list + 16, entries) || !entries)
            return 0;
        for (int index = 0; index < count; ++index)
        {
            const Address attribute = entries + 0x48 * index;
            std::uint16_t id = 0;
            if (!Read(attribute + field::CEconItemAttribute::m_iAttributeDefinitionIndex, id))
                return 0;
            if (id == definition)
                return attribute;
        }
        return 0;
    }
    bool SetIntegerAttribute(Address item, const char *name, std::uint16_t id, std::uint32_t value)
    {
        if (!FindAttribute(item, id))
            setAttribute(reinterpret_cast<void *>(item), name, 1.0f);
        const Address entry = FindAttribute(item, id);
        if (!entry)
            return false;
        // The schema marks these attributes stored_as_integer. Their value slot
        // carries uint32 bits, even though the generic schema field is float32.
        Write(entry + field::CEconItemAttribute::m_flValue, value);
        Write(entry + field::CEconItemAttribute::m_flInitialValue, value);
        return true;
    }
    bool ApplyMesh(Address entity, bool legacy)
    {
        Address scene = 0;
        std::uint64_t oldMask = 0;
        if (!entity || !Read(entity + field::C_BaseEntity::m_pGameSceneNode, scene) || !scene)
            return false;
        const Address modelState = scene + field::CSkeletonInstance::m_modelState;
        if (!Read(modelState + field::CModelState::m_MeshGroupMask, oldMask))
            return false;
        const std::uint64_t mask = legacy ? 2 : 1;
        if (oldMask != mask)
            setMesh(reinterpret_cast<void *>(modelState), &mask);
        return Read(modelState + field::CModelState::m_MeshGroupMask, oldMask) && oldMask == mask;
    }
    bool VisualMatches(Address entity, const skin_changer::Selection &choice)
    {
        Address scene = 0;
        std::uint64_t mask = 0;
        const std::uint64_t expected = choice.legacy && !choice.knife && !choice.customModel ? 2 : 1;
        return (choice.model.empty() || ModelHasPath(ModelHandle(entity), choice.model.c_str())) &&
               Read(entity + field::C_BaseEntity::m_pGameSceneNode, scene) && scene &&
               Read(scene + field::CSkeletonInstance::m_modelState + field::CModelState::m_MeshGroupMask, mask) &&
               mask == expected;
    }
    __declspec(noinline) bool RestoreStockModel(Address weapon, Address hud, int definition)
    {
        const char *stockModel = custom_models::StockModel(definition);
        if (!stockModel)
            return false;
        __try
        {
            if (!ModelHasPath(ModelHandle(weapon), stockModel))
                setModel(reinterpret_cast<void *>(weapon), stockModel);
            const bool worldOk = ModelHasPath(ModelHandle(weapon), stockModel) && ApplyMesh(weapon, false);
            if (hud && !ModelHasPath(ModelHandle(hud), stockModel))
                setModel(reinterpret_cast<void *>(hud), stockModel);
            return worldOk && (!hud || (ModelHasPath(ModelHandle(hud), stockModel) && ApplyMesh(hud, false)));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
    enum class Result
    {
        Applied,
        MissingScene,
        AttributeFailed,
        PaintReadbackFailed,
        ModelPending,
        ModelUnavailable,
        ModelReadbackFailed,
        Fault
    };
    __declspec(noinline) Result ApplyGuarded(const Held &held, Address hud, const skin_changer::Selection &choice,
                                             Address expectedModel = 0)
    {
        __try
        {
            const Address item = ItemView(held.weapon);
            Address scene = 0;
            if (!Read(held.weapon + field::C_BaseEntity::m_pGameSceneNode, scene) || !scene)
                return Result::MissingScene;
            if (choice.customModel && (!expectedModel || !ModelHasPath(expectedModel, choice.model.c_str())))
                return Result::ModelPending;
            if (choice.knife && held.definition != choice.definition)
            {
                Write(item + field::C_EconItemView::m_iItemDefinitionIndex,
                      static_cast<std::uint16_t>(choice.definition));
                Write(held.weapon + field::C_BaseEntity::m_nSubclassID, SubclassHash(choice.definition));
                updateSubclass(reinterpret_cast<void *>(held.weapon));
            }
            if (!choice.customModel && !choice.model.empty())
            {
                setModel(reinterpret_cast<void *>(held.weapon), choice.model.c_str());
                if (hud)
                    setModel(reinterpret_cast<void *>(hud), choice.model.c_str());
            }
            Write(item + field::C_EconItemView::m_iItemIDHigh, 0xffffffffu);
            Write(item + field::C_EconItemView::m_iItemIDLow, 1u);
            Write(item + field::C_EconItemView::m_iEntityQuality, choice.statTrak ? 9 : choice.knife ? 3 : 0);
            Write(item + field::C_EconItemView::m_bInitialized, true);
            Write(item + field::C_EconItemView::m_bDisallowSOC, true);
            Write(item + field::C_EconItemView::m_bRestoreCustomMaterialAfterPrecache, true);
            std::uint32_t account = 0;
            if (Read(held.weapon + field::C_EconEntity::m_OriginalOwnerXuidLow, account))
                Write(item + field::C_EconItemView::m_iAccountID, account);
            Write(held.weapon + field::C_EconEntity::m_nFallbackPaintKit, choice.paintkit);
            Write(held.weapon + field::C_EconEntity::m_nFallbackSeed, choice.seed);
            Write(held.weapon + field::C_EconEntity::m_flFallbackWear, choice.wear);
            Write(held.weapon + field::C_EconEntity::m_nFallbackStatTrak, choice.statTrak ? choice.kills : -1);
            setAttribute(reinterpret_cast<void *>(item), "set item texture prefab",
                         static_cast<float>(choice.paintkit));
            setAttribute(reinterpret_cast<void *>(item), "set item texture seed", static_cast<float>(choice.seed));
            setAttribute(reinterpret_cast<void *>(item), "set item texture wear", choice.wear);
            if (choice.statTrak || FindAttribute(item, 80))
            {
                if (!SetIntegerAttribute(item, "kill eater", 80,
                                         choice.statTrak ? static_cast<std::uint32_t>(choice.kills) : 0xffffffffu) ||
                    !SetIntegerAttribute(item, "kill eater score type", 81, 0u))
                    return Result::AttributeFailed;
            }
            invalidateItem(reinterpret_cast<void *>(item));
            if (getPaintKit(reinterpret_cast<void *>(item)) != choice.paintkit)
                return Result::PaintReadbackFailed;
            if (!choice.customModel)
            {
                // Knives use their single mesh group even for finishes marked
                // legacy. The alternate mesh bit is for dual-mesh gun models.
                if (!ApplyMesh(held.weapon, choice.legacy && !choice.knife) ||
                    (hud && !ApplyMesh(hud, choice.legacy && !choice.knife)))
                    return Result::MissingScene;
            }
            // This is the sequence used by the current native regeneration path.
            updateOwner(reinterpret_cast<void *>(held.weapon + skin_bindings::kCompositeOwnerOffset), true);
            if (!choice.customModel)
                updateSet(reinterpret_cast<void *>(held.weapon), false);
            else
            {
                // Clear old finish composites before assigning custom materials.
                setModel(reinterpret_cast<void *>(held.weapon), choice.model.c_str());
                if (hud)
                    setModel(reinterpret_cast<void *>(hud), choice.model.c_str());
                if (ModelHandle(held.weapon) != expectedModel || !ModelHasPath(expectedModel, choice.model.c_str()) ||
                    (hud && ModelHandle(hud) != expectedModel))
                    return Result::ModelReadbackFailed;
                // SetModel rebuilds the scene node; select visible meshes afterwards.
                if (!ApplyMesh(held.weapon, false) || (hud && !ApplyMesh(hud, false)))
                    return Result::ModelReadbackFailed;
            }
            if (!choice.model.empty() && (!ModelHasPath(ModelHandle(held.weapon), choice.model.c_str()) ||
                                          (hud && !ModelHasPath(ModelHandle(hud), choice.model.c_str()))))
                return Result::ModelReadbackFailed;
            return Result::Applied;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return Result::Fault;
        }
    }
    __declspec(noinline) bool Matches(const Held &held, const skin_changer::Selection &choice)
    {
        __try
        {
            const Address item = ItemView(held.weapon);
            int fallback = -1, kills = -1, seed = -1;
            float wear = -1;
            std::uint32_t high = 0;
            return held.definition == choice.definition && Read(item + field::C_EconItemView::m_iItemIDHigh, high) &&
                   high == 0xffffffffu && Read(held.weapon + field::C_EconEntity::m_nFallbackPaintKit, fallback) &&
                   fallback == choice.paintkit && Read(held.weapon + field::C_EconEntity::m_nFallbackStatTrak, kills) &&
                   kills == (choice.statTrak ? choice.kills : -1) &&
                   Read(held.weapon + field::C_EconEntity::m_nFallbackSeed, seed) && seed == choice.seed &&
                   Read(held.weapon + field::C_EconEntity::m_flFallbackWear, wear) && wear == choice.wear &&
                   VisualMatches(held.weapon, choice) && getPaintKit(reinterpret_cast<void *>(item)) == choice.paintkit;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
    __declspec(noinline) bool UpdateHud(Address hud, const skin_changer::Selection &choice, Address expectedModel)
    {
        __try
        {
            if (!choice.model.empty())
                setModel(reinterpret_cast<void *>(hud), choice.model.c_str());
            if (choice.customModel &&
                (!ModelHasPath(expectedModel, choice.model.c_str()) || ModelHandle(hud) != expectedModel))
                return false;
            if (!choice.model.empty() && !ModelHasPath(ModelHandle(hud), choice.model.c_str()))
                return false;
            return ApplyMesh(hud, !choice.customModel && !choice.knife && choice.legacy);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
    __declspec(noinline) bool GloveMatches(Address pawn, const skin_changer::Selection &choice)
    {
        __try
        {
            const Address item = pawn + field::C_CSPlayerPawn::m_EconGloves;
            std::uint16_t definition = 0;
            bool initialized = false;
            float wear = -1, seed = -1;
            std::uint64_t id = 0;
            std::uint32_t high = 0, low = 0;
            const Address wearAttribute = FindAttribute(item, 8), seedAttribute = FindAttribute(item, 7);
            return Read(item + field::C_EconItemView::m_iItemDefinitionIndex, definition) &&
                   definition == choice.definition && Read(item + field::C_EconItemView::m_iItemID, id) &&
                   id == kGloveItemId && Read(item + field::C_EconItemView::m_iItemIDHigh, high) &&
                   high == static_cast<std::uint32_t>(kGloveItemId >> 32) &&
                   Read(item + field::C_EconItemView::m_iItemIDLow, low) &&
                   low == static_cast<std::uint32_t>(kGloveItemId) &&
                   Read(item + field::C_EconItemView::m_bInitialized, initialized) && initialized &&
                   getPaintKit(reinterpret_cast<void *>(item)) == choice.paintkit && wearAttribute &&
                   Read(wearAttribute + field::CEconItemAttribute::m_flValue, wear) && wear == choice.wear &&
                   seedAttribute && Read(seedAttribute + field::CEconItemAttribute::m_flValue, seed) &&
                   seed == static_cast<float>(choice.seed);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
    __declspec(noinline) bool ApplyGloves(Address pawn, Address controller, int team,
                                          const skin_changer::Selection &choice)
    {
        __try
        {
            if (!playerPostDataUpdate || !setBodygroup)
                return false;
            std::uint64_t steamId = 0;
            if (!Read(controller + field::CBasePlayerController::m_steamID, steamId))
                return false;
            const Address item = pawn + field::C_CSPlayerPawn::m_EconGloves;
            Write(item + field::C_EconItemView::m_iItemDefinitionIndex, static_cast<std::uint16_t>(choice.definition));
            Write(item + field::C_EconItemView::m_iItemID, kGloveItemId);
            Write(item + field::C_EconItemView::m_iItemIDHigh, static_cast<std::uint32_t>(kGloveItemId >> 32));
            Write(item + field::C_EconItemView::m_iItemIDLow, static_cast<std::uint32_t>(kGloveItemId));
            Write(item + field::C_EconItemView::m_iAccountID, static_cast<std::uint32_t>(steamId));
            Write(item + field::C_EconItemView::m_iEntityQuality, 3);
            Write(item + field::C_EconItemView::m_bInitialized, true);
            Write(item + field::C_EconItemView::m_bDisallowSOC, true);
            Write(item + field::C_EconItemView::m_bRestoreCustomMaterialAfterPrecache, true);
            setAttribute(reinterpret_cast<void *>(item), "set item texture prefab",
                         static_cast<float>(choice.paintkit));
            setAttribute(reinterpret_cast<void *>(item), "set item texture seed", static_cast<float>(choice.seed));
            setAttribute(reinterpret_cast<void *>(item), "set item texture wear", choice.wear);
            invalidateItem(reinterpret_cast<void *>(item));
            if (!GloveMatches(pawn, choice))
                return false;
            // Request the native render refresh as well as changing item attributes.
            Write(pawn + field::C_CSPlayerPawn::m_bNeedToReApplyGloves, true);
            playerPostDataUpdate(reinterpret_cast<void *>(pawn), 1);
            setBodygroup(reinterpret_cast<void *>(pawn), 0, 1u);
            if (team == 3)
                setBodygroup(reinterpret_cast<void *>(pawn), 1, 1u);
            return GloveMatches(pawn, choice);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
    __declspec(noinline) bool ApplyAgent(Address pawn, const char *model)
    {
        __try
        {
            if (!setModel)
                return false;
            setModel(reinterpret_cast<void *>(pawn), model);
            // The resource can finish streaming after SetModel returns.
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
    Address EnemyScene(Address pawn)
    {
        Address scene = 0;
        Read(pawn + field::C_BaseEntity::m_pGameSceneNode, scene);
        return scene;
    }
    std::string AgentResourcePath(Address pawn)
    {
        auto path = ModelPath(ModelHandle(pawn));
        if (path.ends_with("_c"))
            path.resize(path.size() - 2);
        std::replace(path.begin(), path.end(), '\\', '/');
        return path;
    }
    void PublishEnemyChanges()
    {
        unsigned count = 0;
        for (const auto &[handle, state] : enemyModels)
            count += state.owned;
        enemyChanges = count;
    }
    bool EnemyIdentity(const EnemyModelState &state, Address list)
    {
        return list && list == state.list && Resolve(list, state.handle) == state.pawn;
    }
    // Network updates may replace the selected model with a new authoritative
    // agent. Capture that resource as the next restore target, never our custom
    // replacement. A fresh scene is not owned unless it completes our request.
    void ObserveEnemyModels(Address list)
    {
        for (auto &[handle, state] : enemyModels)
        {
            if (!state.owned || !EnemyIdentity(state, list))
                continue;
            const auto scene = EnemyScene(state.pawn);
            const auto actual = AgentResourcePath(state.pawn);
            if (game_access::SameModelPath(actual, state.replacement))
            {
                if (!state.confirmed && !state.restoring && state.requests)
                {
                    state.scene = scene;
                    state.writtenModel = ModelHandle(state.pawn);
                }
                continue;
            }
            if (state.restoring)
            {
                if (game_access::SameModelPath(actual, state.original))
                    state.owned = false;
                continue;
            }
            // An error resource while streaming belongs to our request; keep
            // its original backup so a failed load can be rolled back.
            if (!character_catalog::IsAgentPath(actual))
                continue;
            if (scene != state.scene || ModelHandle(state.pawn) != state.writtenModel)
            {
                state.original = actual;
                state.scene = scene;
                state.owned = state.confirmed = false;
                state.requests = 0;
                state.started = state.lastRequest = 0;
            }
        }
        PublishEnemyChanges();
    }
    bool RestoreEnemy(EnemyModelState &state, Address list, ULONGLONG now)
    {
        if (!state.owned || !EnemyIdentity(state, list))
            return true;
        const auto scene = EnemyScene(state.pawn);
        const auto actual = AgentResourcePath(state.pawn);
        if (game_access::SameModelPath(actual, state.original))
        {
            state.owned = false;
            return true;
        }
        // Respect engine/third-party changes and never write to a new scene.
        if (!scene || scene != state.scene ||
            (!game_access::SameModelPath(actual, state.replacement) &&
             ModelHandle(state.pawn) != state.writtenModel))
        {
            state.owned = false;
            return true;
        }
        if (!state.restoring)
        {
            state.restoring = true;
            state.started = now;
            state.requests = 0;
            state.lastRequest = 0;
        }
        if (state.requests < 3 && (!state.requests || now - state.lastRequest >= 500))
        {
            ++state.requests;
            state.lastRequest = now;
            if (EnemyIdentity(state, list) && EnemyScene(state.pawn) == state.scene &&
                ApplyAgent(state.pawn, state.original.c_str()))
            {
                state.scene = EnemyScene(state.pawn);
                state.writtenModel = ModelHandle(state.pawn);
                if (ModelHasPath(state.writtenModel, state.original.c_str()))
                {
                    state.owned = false;
                    return true;
                }
            }
        }
        return false;
    }
    bool EnemyCandidate(const game_access::LocalPlayer &local, Address controller, Address &pawn,
                        std::uint32_t &handle)
    {
        if (!controller || controller == local.controller ||
            memory::read_string(reinterpret_cast<Address>(systems::g_entities.get_schema_name(controller)), 64) !=
                "CCSPlayerController" ||
            !Read(controller + field::CCSPlayerController::m_hPlayerPawn, handle) ||
            !(pawn = Resolve(local.list, handle)) || pawn == local.pawn)
            return false;
        std::uint8_t team = 0, life = 1;
        int health = 0;
        if (!Read(pawn + field::C_BaseEntity::m_iTeamNum, team) || (team != 2 && team != 3) ||
            team == local.team || !Read(pawn + field::C_BaseEntity::m_iHealth, health) || health <= 0 ||
            !Read(pawn + field::C_BaseEntity::m_lifeState, life) || life != 0)
            return false;
        const auto scene = EnemyScene(pawn);
        const auto dormantOffset = systems::schemas::lookup("CGameSceneNode", fnv1a::runtime_hash("m_bDormant"));
        bool dormant = true;
        return scene && dormantOffset && Read(scene + dormantOffset, dormant) && !dormant;
    }
    void TickEnemyModels(ULONGLONG now)
    {
        struct Scope
        {
            Scope() { ++enemyCallbacks; }
            ~Scope() { --enemyCallbacks; }
        } scope;
        skin_changer::EnemyModelSelection choice;
        std::uint64_t revision = 0;
        {
            std::lock_guard lock(settingsMutex);
            choice = enemySelection;
            revision = enemyRevision;
        }
        if (!choice.enabled && enemyModels.empty())
        {
            std::lock_guard lock(settingsMutex);
            if (enemyRevision == revision)
                enemyStatus = "Disabled";
            return;
        }
        const auto local = game_access::ReadLocalPlayer(clientBase);
        ObserveEnemyModels(local.list);
        const bool active = choice.enabled && !enemyStopping && !faulted && local.list && local.controller &&
                            local.pawn && (local.team == 2 || local.team == 3);
        std::unordered_set<std::uint32_t> eligible;
        unsigned matched = 0;
        if (active)
            for (unsigned index = 1; index <= 64; ++index)
            {
                Address pawn = 0;
                std::uint32_t handle = 0xffffffffu;
                if (!EnemyCandidate(local, game_access::Entity(local.list, index), pawn, handle))
                    continue;
                eligible.insert(handle);
                auto found = enemyModels.find(handle);
                if (found != enemyModels.end() &&
                    (found->second.pawn != pawn || found->second.list != local.list ||
                     found->second.revision != revision))
                {
                    if (!RestoreEnemy(found->second, local.list, now))
                        continue;
                    enemyModels.erase(found);
                    found = enemyModels.end();
                }
                const auto actual = AgentResourcePath(pawn);
                if (game_access::SameModelPath(actual, choice.model))
                {
                    ++matched;
                    if (found != enemyModels.end() && found->second.owned)
                        found->second.confirmed = true;
                    continue;
                }
                if (found == enemyModels.end())
                {
                    // Refuse to replace a pawn whose original model is unknown.
                    if (!character_catalog::IsAgentPath(actual) || enemyStopping)
                        continue;
                    EnemyModelState state;
                    state.list = local.list;
                    state.pawn = pawn;
                    state.handle = handle;
                    state.scene = EnemyScene(pawn);
                    state.original = actual;
                    state.replacement = choice.model;
                    state.revision = revision;
                    state.started = now;
                    found = enemyModels.emplace(handle, std::move(state)).first;
                }
                auto &state = found->second;
                if (state.restoring)
                {
                    RestoreEnemy(state, local.list, now);
                    continue;
                }
                if (state.failed)
                    continue;
                if (!state.started)
                    state.started = now;
                if (enemyPrecacheRevision != revision)
                {
                    enemyPrecacheRevision = revision;
                    enemyPrecached = character_catalog::Installed(choice.model) &&
                                     PrecacheCustomModel(choice.model.c_str());
                }
                if (!setModel || !enemyPrecached || now - state.started >= 5000)
                {
                    state.failed = true;
                    RestoreEnemy(state, local.list, now);
                    continue;
                }
                if (state.requests < 3 && (!state.requests || now - state.lastRequest >= 1000))
                {
                    if (enemyStopping || !EnemyIdentity(state, local.list) || EnemyScene(pawn) != state.scene)
                        continue;
                    ++state.requests;
                    state.lastRequest = now;
                    // Publish ownership before the native call so Stop cannot
                    // disable the callback while its first assignment is running.
                    state.owned = true;
                    PublishEnemyChanges();
                    const bool assigned = ApplyAgent(pawn, choice.model.c_str());
                    state.scene = EnemyScene(pawn);
                    state.writtenModel = ModelHandle(pawn);
                    if (!assigned)
                    {
                        state.failed = true;
                        RestoreEnemy(state, local.list, now);
                        continue;
                    }
                    if (ModelHasPath(state.writtenModel, choice.model.c_str()))
                    {
                        state.confirmed = true;
                        ++matched;
                    }
                }
            }
        for (auto it = enemyModels.begin(); it != enemyModels.end();)
        {
            if (!active || enemyStopping || !eligible.contains(it->first) || it->second.revision != revision)
            {
                if (RestoreEnemy(it->second, local.list, now))
                    it = enemyModels.erase(it);
                else
                    ++it;
            }
            else
                ++it;
        }
        PublishEnemyChanges();
        unsigned failed = 0, loading = 0;
        for (const auto &[handle, state] : enemyModels)
        {
            failed += state.failed;
            loading += !state.failed && !state.confirmed && !state.restoring;
        }
        std::string message;
        if (!active)
            message = enemyChanges ? "Restoring enemy models..." : choice.enabled && !enemyStopping
                          ? "Join CT or T to apply" : "Disabled";
        else if (failed)
            message = !enemyPrecached ? "Enemy model loader unavailable; check installed files and game build."
                                      : "Model load failed. Select the model again to retry.";
        else if (loading)
            message = "Loading enemy model...";
        else if (matched)
            message = "Applied to " + std::to_string(matched) + (matched == 1 ? " enemy" : " enemies");
        else
            message = "Waiting for enemies";
        std::lock_guard lock(settingsMutex);
        if (enemyRevision == revision)
            enemyStatus = std::move(message);
    }
    void TickCosmetics(ULONGLONG now, bool agent)
    {
        const auto player = game_access::ReadLocalPlayer(clientBase);
        currentTeam = player.team == 2 || player.team == 3 ? player.team : 0;
        {
            std::lock_guard lock(settingsMutex);
            for (auto &[key, saved] : selections)
            {
                if (agent ? !saved.choice.agentModel : !saved.choice.gloves)
                    continue;
                if (saved.choice.agentModel && currentTeam && saved.choice.team != currentTeam)
                {
                    cosmeticStates.erase(key);
                    saved.status = saved.choice.team == 3 ? "Waiting for CT team" : "Waiting for T team";
                }
                else
                    switch (player.state)
                    {
                    case game_access::PlayerState::Unavailable:
                        saved.status = "Player data unavailable";
                        break;
                    case game_access::PlayerState::NoController:
                        saved.status = "Join a game to apply";
                        break;
                    case game_access::PlayerState::NoPawn:
                        saved.status = "Waiting for spawn";
                        break;
                    case game_access::PlayerState::Dead:
                        saved.status = "Respawn to apply";
                        break;
                    case game_access::PlayerState::NoTeam:
                        saved.status = "Join CT or T to apply";
                        break;
                    default:
                        break;
                    }
            }
        }
        if (player.state != game_access::PlayerState::Alive)
        {
            cosmeticStates.clear();
            return;
        }
        const auto pawn = player.pawn;
        const auto handle = player.handle;
        const auto team = player.team;
        // Agents run before FRAME_START; gloves run after the data update.
        // Both paths re-resolve the full pawn handle immediately before use.
        {
            const int key = CosmeticKey(agent, team);
            Saved saved;
            {
                std::lock_guard lock(settingsMutex);
                const auto found = selections.find(key);
                if (found == selections.end())
                    return;
                saved = found->second;
            }
            auto &state = cosmeticStates[key];
            if (state.pawn != pawn || state.handle != handle || state.revision != saved.revision)
                state = {pawn, handle, saved.revision, now};
            const bool matched =
                agent ? ModelHasPath(ModelHandle(pawn), saved.choice.model.c_str()) : GloveMatches(pawn, saved.choice);
            std::string message;
            if (matched)
            {
                if (agent && !state.confirmed && state.requests)
                    game_access::Write(pawn + field::C_CSPlayerPawn::m_bNeedToReApplyGloves, true);
                state.failed = false;
                state.confirmed = true;
                message = agent ? "Agent applied" : "Gloves applied";
            }
            else if (state.failed)
                return;
            else if (agent)
            {
                if (state.confirmed)
                    state = {pawn, handle, saved.revision, now};
                // SetModel itself requests the resource. Waiting for FindModel
                // first prevents an uncached agent from ever being assigned.
                if (!state.requests && saved.choice.customModel && !PrecacheCustomModel(saved.choice.model.c_str()))
                {
                    state.failed = true;
                    message = "Agent loader unavailable; check game build.";
                }
                else if (now - state.started >= 5000)
                {
                    state.failed = true;
                    message = "Agent load timed out. Check model files; Apply to retry.";
                }
                else
                {
                    if (!state.requests || (state.requests < 3 && now - state.lastRequest >= 1000))
                    {
                        ++state.requests;
                        state.lastRequest = now;
                        if (!ApplyAgent(pawn, saved.choice.model.c_str()))
                            state.failed = true;
                    }
                    if (state.failed)
                        message = "Agent assignment failed; Apply to retry.";
                    else if (ModelHasPath(ModelHandle(pawn), saved.choice.model.c_str()))
                    {
                        state.confirmed = true;
                        game_access::Write(pawn + field::C_CSPlayerPawn::m_bNeedToReApplyGloves, true);
                        message = "Agent applied";
                    }
                    else
                        message = "Loading selected agent...";
                }
            }
            else if (!playerPostDataUpdate || !setBodygroup)
            {
                state.failed = true;
                message = "Glove refresh unavailable for this game build.";
            }
            else if (ApplyGloves(pawn, player.controller, team, saved.choice))
                message = "Glove refresh requested";
            else
            {
                state.failed = true;
                message = "Glove update failed verification; press Apply to retry.";
            }
            std::lock_guard lock(settingsMutex);
            const auto found = selections.find(key);
            if (found != selections.end() && found->second.revision == saved.revision)
                found->second.status = message;
        }
    }
    #include "lobby_preview.inl"

    void Tick()
    {
        const ULONGLONG now = GetTickCount64();
        if (now < nextTick || faulted)
            return;
        nextTick = now + 100;
        Held held;
        if (!ReadHeld(held))
        {
            applied.clear();
            return;
        }
        const int key = Key(held.definition, IsKnife(held.definition));
        Saved saved;
        {
            std::lock_guard lock(settingsMutex);
            const auto found = selections.find(key);
            if (found == selections.end())
                return;
            saved = found->second;
        }
        if (applied.size() > 128)
            applied.clear();
        auto &state = applied[held.handle];
        const Address hud = HudWeapon(held);
        const bool same = state.revision == saved.revision && state.weapon == held.weapon && state.pawn == held.pawn;
        if (same && (state.failed || now - state.lastAttempt < 1000))
            return;
        if (same && Matches(held, saved.choice) &&
            (!saved.choice.customModel || (ModelHasPath(state.modelHandle, saved.choice.model.c_str()) &&
                                           CustomModelAssigned(held.weapon, state.modelHandle))))
        {
            if (!hud || (hud == state.hud && VisualMatches(hud, saved.choice) &&
                         (!saved.choice.customModel || CustomModelAssigned(hud, state.modelHandle))))
                return;
            if (UpdateHud(hud, saved.choice, state.modelHandle))
            {
                state.hud = hud;
                return;
            }
        }
        ULONGLONG loadStarted = same ? state.loadStarted : 0;
        Result result = Result::Applied;
        Address expectedModel = 0;
        if (saved.choice.customModel)
        {
            if (!loadStarted)
            {
                loadStarted = now;
                if (!PrecacheCustomModel(saved.choice.model.c_str()))
                    result = Result::ModelUnavailable;
            }
            if (result == Result::Applied && !(expectedModel = FindModel(saved.choice.model.c_str())))
                result = now - loadStarted < 5000 ? Result::ModelPending : Result::ModelUnavailable;
        }
        if (result == Result::Applied)
            result = ApplyGuarded(held, hud, saved.choice, expectedModel);
        bool restored = true;
        if (saved.choice.customModel && result != Result::Applied)
        {
            std::uint16_t currentDefinition = held.definition;
            Read(ItemView(held.weapon) + field::C_EconItemView::m_iItemDefinitionIndex, currentDefinition);
            restored = RestoreStockModel(held.weapon, hud, currentDefinition);
        }
        state = {held.weapon,
                 held.pawn,
                 hud,
                 saved.revision,
                 now,
                 result != Result::Applied && result != Result::MissingScene && result != Result::ModelPending &&
                     !(result == Result::ModelReadbackFailed && !saved.choice.customModel)};
        state.modelHandle = ModelHandle(held.weapon);
        state.loadStarted = loadStarted;
        std::lock_guard lock(settingsMutex);
        if (result == Result::Fault)
        {
            faulted = true;
            backendStatus = "Skin updates stopped: a native call faulted. Check this client build.";
        }
        const auto found = selections.find(key);
        if (found == selections.end() || found->second.revision != saved.revision)
            return;
        const auto *custom = saved.choice.customModel ? custom_models::Find(saved.choice.model) : nullptr;
        const std::string modelName = custom ? custom->name : "Custom model";
        switch (result)
        {
        case Result::Applied:
            found->second.status = "Applied";
            break;
        case Result::MissingScene:
            found->second.status = "Saved; waiting for the weapon model";
            break;
        case Result::AttributeFailed:
            found->second.status = "Apply failed: StatTrak attribute was not created";
            break;
        case Result::PaintReadbackFailed:
            found->second.status = "Apply failed: native paintkit readback did not match";
            break;
        case Result::ModelPending:
            found->second.status = "Loading " + modelName + "; stock weapon retained while resources stream";
            break;
        case Result::ModelUnavailable:
            found->second.status =
                modelName + " resource unavailable in this session; stock weapon restored. Press Apply to retry.";
            break;
        case Result::ModelReadbackFailed:
            found->second.status = saved.choice.customModel
                                       ? modelName + " assignment failed; stock weapon restored. Press Apply to retry."
                                       : "Saved; waiting for the selected weapon model to load";
            break;
        case Result::Fault:
            found->second.status = backendStatus;
            break;
        }
        if (!restored)
            found->second.status += " Stock-model restore could not be verified.";
    }
    void __fastcall StageHook(void *self, int stage)
    {
        struct Scope
        {
            Scope()
            {
                ++callbacks;
            }
            ~Scope()
            {
                --callbacks;
            }
        } scope;
        if (enabled && stage == skin_bindings::kAgentStage)
        {
            try
            {
                if (!faulted && !enemyStopping)
                    TickCosmetics(GetTickCount64(), true);
                TickEnemyModels(GetTickCount64());
            }
            catch (const std::exception &)
            {
                faulted = true;
                std::lock_guard lock(settingsMutex);
                backendStatus = "Agent updates stopped after an internal error";
            }
        }
        if (const auto callback = beforeStageCallback.load())
            callback(stage);
        originalStage(self, stage);
        if (const auto callback = afterStageCallback.load()) callback(stage);
        if (enabled && stage == skin_bindings::kAgentStage && !faulted)
        {
            try
            {
                TickLobbyModels(GetTickCount64());
            }
            catch (const std::exception &)
            {
                faulted = true;
                std::lock_guard lock(settingsMutex);
                backendStatus = "Lobby updates stopped after an internal error";
            }
        }
        if (!enabled || stage != skin_bindings::kApplyStage)
            return;
        try
        {
            ObserveEnemyModels(game_access::ReadLocalPlayer(clientBase).list);
            // Network updates can replace the glove item every frame.
            if (!faulted)
                TickCosmetics(GetTickCount64(), false);
            Tick();
            if (const auto callback = stageCallback.load())
                callback();
        }
        catch (const std::exception &)
        {
            faulted = true;
            std::lock_guard lock(settingsMutex);
            backendStatus = "Skin updates stopped after an internal error";
        }
    }
    bool FileHashMatches(HMODULE module, std::string_view expectedHash)
    {
        return native_startup::MatchesModule(reinterpret_cast<Address>(module),expectedHash);
    }
    bool BindCustomResources()
    {
        const auto module = GetModuleHandleW(L"resourcesystem.dll");
        const auto tier0 = GetModuleHandleW(L"tier0.dll");
        std::array<std::uint8_t, 24> actual{};
        if (!module || !tier0 || !FileHashMatches(module, skin_bindings::kResourceSha256) ||
            !Copy(reinterpret_cast<Address>(module) + skin_bindings::PrecacheResource.rva, actual.data(),
                  actual.size()) ||
            actual != skin_bindings::PrecacheResource.entry)
            return false;
        using CreateInterfaceFn = void *(*)(const char *, int *);
        const auto create = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(module, "CreateInterface"));
        insertString = reinterpret_cast<InsertFn>(GetProcAddress(tier0, "?Insert@CBufferString@@QEAAPEBDHPEBDH_N@Z"));
        purgeString = reinterpret_cast<PurgeFn>(GetProcAddress(tier0, "?Purge@CBufferString@@QEAAXH@Z"));
        if (!create || !insertString || !purgeString || !(resourceSystem = create("ResourceSystem013", nullptr)))
            return false;
        precacheResource =
            reinterpret_cast<PrecacheFn>(reinterpret_cast<Address>(module) + skin_bindings::PrecacheResource.rva);
        return true;
    }
    bool BindPreviewMaterials()
    {
        previewMaterialBase = 0;
        const auto material = GetModuleHandleW(L"materialsystem2.dll");
        const auto render = GetModuleHandleW(L"rendersystemdx11.dll");
        std::array<std::uint8_t, 24> actual{};
        const auto base = reinterpret_cast<Address>(material);
        Address getter = 0;
        if (!material || !render || !FileHashMatches(material, preview_bindings::kMaterialSha256) ||
            !FileHashMatches(render, preview_bindings::kRenderSha256) ||
            !Copy(base + preview_bindings::GetTexture.rva, actual.data(), actual.size()) ||
            actual != preview_bindings::GetTexture.entry ||
            !Read(base + preview_bindings::kMaterialVtable + preview_bindings::kGetTextureSlot * sizeof(Address), getter) ||
            getter != base + preview_bindings::GetTexture.rva)
            return false;
        previewMaterialBase = base;
        return true;
    }
    __declspec(noinline) ID3D11ShaderResourceView *PreviewColor(Address material)
    {
        Address table = 0;
        if (!previewMaterialBase || !Read(material, table) ||
            table != previewMaterialBase + preview_bindings::kMaterialVtable)
            return nullptr;
        using GetTextureFn = bool(__fastcall *)(void *, Address *, std::uint32_t);
        const auto getter = reinterpret_cast<GetTextureFn>(previewMaterialBase + preview_bindings::GetTexture.rva);
        __try
        {
            Address binding = 0, texture = 0, view = 0;
            int references = 0;
            if (!getter(reinterpret_cast<void *>(material), &binding, preview_bindings::kColorToken) || !binding)
                getter(reinterpret_cast<void *>(material), &binding, preview_bindings::kRepresentativeToken);
            if (!binding || !Read(binding + 0x20, references) || references <= 0 ||
                !Read(binding, texture) || !texture)
                return nullptr;
            if (!Read(texture + preview_bindings::kTextureSrgbView, view) || !view)
                Read(texture + preview_bindings::kTextureLinearView, view);
            if (!view) return nullptr;
            ID3D11ShaderResourceView *retained = nullptr;
            if (FAILED(reinterpret_cast<IUnknown *>(view)->QueryInterface(__uuidof(ID3D11ShaderResourceView),
                       reinterpret_cast<void **>(&retained))))
                return nullptr;
            D3D11_SHADER_RESOURCE_VIEW_DESC description{};
            retained->GetDesc(&description);
            if (description.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D)
            {
                retained->Release();
                return nullptr;
            }
            return retained;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }
    bool BindFunctions()
    {
        bindingFailure.clear();
        const auto module = GetModuleHandleW(L"client.dll");
        clientBase = reinterpret_cast<Address>(module);
        if (!module)
        {
            bindingFailure = "Skin updater unavailable: client.dll is not loaded";
            return false;
        }
        if (!FileHashMatches(module, skin_bindings::kClientSha256))
        {
            bindingFailure = "Skin updater unavailable: client.dll hash differs from the audited build";
            return false;
        }
        const skin_bindings::Function *functions[] = {
            &skin_bindings::FrameStageNotify,     &skin_bindings::SetItemAttribute,   &skin_bindings::InvalidateItem,
            &skin_bindings::UpdateCompositeOwner, &skin_bindings::UpdateCompositeSet, &skin_bindings::GetPaintKit,
            &skin_bindings::SetMeshMask,          &skin_bindings::SetModel,           &skin_bindings::UpdateSubclass};
        for (const auto function : functions)
        {
            std::array<std::uint8_t, 24> actual{};
            if (!Copy(clientBase + function->rva, actual.data(), actual.size()) || actual != function->entry)
            {
                bindingFailure = "Skin updater unavailable: function entry mismatch at client RVA " +
                                 std::to_string(function->rva);
                return false;
            }
        }
        Address table = 0, stage = 0;
        if (!Read(clientBase + econ_bindings::kSource2ClientInstanceRva, table) ||
            table != clientBase + econ_bindings::kClientVtableRva ||
            !Read(table + skin_bindings::kFrameStageVtableSlot * sizeof(Address), stage) ||
            stage != clientBase + skin_bindings::FrameStageNotify.rva)
        {
            bindingFailure = "Skin updater unavailable: Source2Client interface or frame-stage slot differs from the audited build";
            return false;
        }
        hookTarget = reinterpret_cast<void *>(stage);
        setAttribute = reinterpret_cast<AttributeFn>(clientBase + skin_bindings::SetItemAttribute.rva);
        invalidateItem = reinterpret_cast<ItemFn>(clientBase + skin_bindings::InvalidateItem.rva);
        updateOwner = reinterpret_cast<CompositeFn>(clientBase + skin_bindings::UpdateCompositeOwner.rva);
        updateSet = reinterpret_cast<CompositeFn>(clientBase + skin_bindings::UpdateCompositeSet.rva);
        getPaintKit = reinterpret_cast<PaintFn>(clientBase + skin_bindings::GetPaintKit.rva);
        setMesh = reinterpret_cast<MeshFn>(clientBase + skin_bindings::SetMeshMask.rva);
        setModel = reinterpret_cast<ModelFn>(clientBase + skin_bindings::SetModel.rva);
        updateSubclass = reinterpret_cast<ItemFn>(clientBase + skin_bindings::UpdateSubclass.rva);
        std::array<std::uint8_t, 24> glovePostEntry{}, bodygroupEntry{};
        if (Copy(clientBase + skin_bindings::PlayerPostDataUpdate.rva, glovePostEntry.data(), glovePostEntry.size()) &&
            glovePostEntry == skin_bindings::PlayerPostDataUpdate.entry &&
            Copy(clientBase + skin_bindings::SetBodygroup.rva, bodygroupEntry.data(), bodygroupEntry.size()) &&
            bodygroupEntry == skin_bindings::SetBodygroup.entry)
        {
            playerPostDataUpdate = reinterpret_cast<PostDataFn>(clientBase + skin_bindings::PlayerPostDataUpdate.rva);
            setBodygroup = reinterpret_cast<BodygroupFn>(clientBase + skin_bindings::SetBodygroup.rva);
        }
        BindCustomResources(); // An unavailable custom loader must not disable ordinary skins.
        BindPreviewMaterials();
        return true;
    }
} // namespace

bool skin_changer::SaveEnemyModel(const EnemyModelSelection &choice)
{
    if (choice.enabled && (enemyStopping || !character_catalog::IsAgentPath(choice.model) ||
                           !character_catalog::Installed(choice.model)))
        return false;
    if (!choice.model.empty() && !character_catalog::IsAgentPath(choice.model))
        return false;
    std::lock_guard lock(settingsMutex);
    enemySelection = choice;
    enemyRevision = ++nextRevision;
    enemyStatus = choice.enabled ? "Saved; waiting for enemies" : "Restoring default enemy models";
    return true;
}
skin_changer::EnemyModelSelection skin_changer::GetEnemyModel()
{
    std::lock_guard lock(settingsMutex);
    return enemySelection;
}
std::string skin_changer::EnemyModelStatus()
{
    std::lock_guard lock(settingsMutex);
    return enemySelection.enabled && !enabled ? backendStatus : enemyStatus;
}
bool skin_changer::Save(const Selection &choice)
{
    if (choice.agentModel || choice.gloves)
    {
        if (choice.agentModel && choice.gloves)
            return false;
        if (choice.agentModel)
        {
            if ((choice.team != 2 && choice.team != 3) || !character_catalog::IsAgentPath(choice.model) ||
                choice.knife || choice.statTrak || choice.paintkit != 0 || choice.definition < 0 ||
                choice.definition > 65535 || (choice.customModel && !character_catalog::Installed(choice.model)))
                return false;
        }
        else if (!IsGlove(choice.definition) || choice.paintkit < 0 || choice.paintkit > 100000 || choice.seed < 0 ||
                 choice.seed > 1000 || !std::isfinite(choice.wear) || choice.wear < 0 || choice.wear > 1 ||
                 choice.statTrak || choice.knife || choice.customModel)
            return false;
        std::lock_guard lock(settingsMutex);
        selections[CosmeticKey(choice.agentModel, choice.team)] = {choice, ++nextRevision, "Saved; waiting for player"};
        return true;
    }
    const auto *custom = custom_models::Find(choice.model);
    if (choice.definition <= 0 || choice.definition > 65535 || choice.paintkit < 0 || choice.paintkit > 100000 ||
        choice.kills < 0 || choice.seed < 0 || choice.seed > 1000 || !std::isfinite(choice.wear) || choice.wear < 0 ||
        choice.wear > 1 || (choice.knife != IsKnife(choice.definition)) ||
        (choice.knife && !choice.customModel && choice.model != custom_models::StockModel(choice.definition)) ||
        (!choice.customModel && choice.model.starts_with("models/custom/")) ||
        (choice.customModel && (!custom || choice.definition != custom->definition || choice.paintkit != 0 ||
                                !CustomModelAvailable(choice.model))))
        return false;
    std::lock_guard lock(settingsMutex);
    selections[Key(choice.definition, choice.knife)] = {choice, ++nextRevision, "Saved; equip to apply"};
    return true;
}
bool skin_changer::GetSelection(int definition, bool knife, Selection &choice, std::uint64_t *revision)
{
    std::lock_guard lock(settingsMutex);
    const auto found = selections.find(Key(definition, knife));
    if (found == selections.end())
        return false;
    choice = found->second.choice;
    if (revision) *revision = found->second.revision;
    return true;
}
bool skin_changer::GetCosmetic(bool agent, int team, Selection &choice)
{
    std::lock_guard lock(settingsMutex);
    const auto found = selections.find(CosmeticKey(agent, team));
    if (found == selections.end())
        return false;
    choice = found->second.choice;
    return true;
}
__declspec(noinline) int skin_changer::ReadPaintKit(std::uintptr_t weapon)
{
    if (!enabled || faulted || !getPaintKit || !weapon)
        return -1;
    __try
    {
        return getPaintKit(reinterpret_cast<void *>(ItemView(weapon)));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}
bool skin_changer::ReadPreviewMaterials(std::uintptr_t weapon, std::vector<preview_scene::MaterialTexture> &textures)
{
    textures.clear();
    Held held;
    if (!enabled || faulted || !previewMaterialBase || !ReadHeld(held) || held.weapon != weapon)
        return false;
    const Address owner = weapon + skin_bindings::kCompositeOwnerOffset;
    int count = 0;
    Address entries = 0;
    if (!Read(owner + preview_bindings::kOwnerMaterials, count) || count < 0 || count > 64 ||
        !Read(owner + preview_bindings::kOwnerMaterials + 8, entries) || (count && !entries))
        return true;
    for (int i = 0; i < count; ++i)
    {
        Address binding = 0, material = 0, originalBinding = 0, original = 0, name = 0, table = 0;
        if (!Read(entries + i * sizeof(Address), binding) || !binding || !Read(binding, material) || !material ||
            !Read(material, table) || table != previewMaterialBase + preview_bindings::kMaterialVtable ||
            !Read(material + preview_bindings::kOriginalMaterial, originalBinding) || !originalBinding ||
            !Read(originalBinding, original) || !original || !Read(original, table) ||
            table != previewMaterialBase + preview_bindings::kMaterialVtable ||
            !Read(original + preview_bindings::kMaterialName, name) || !name)
            continue;
        std::string path;
        for (int n = 0; n < 260; ++n)
        {
            char ch = 0;
            if (!Read(name + n, ch)) { path.clear(); break; }
            if (!ch) break;
            path += ch;
        }
        if (path.empty() || path.size() == 260 || (!path.ends_with(".vmat") && !path.ends_with(".vmat_c")))
            continue;
        preview_scene::MaterialTexture captured;
        captured.material = std::move(path);
        captured.texture.Attach(PreviewColor(material));
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> stock;
        stock.Attach(PreviewColor(original));
        // A streaming composite may still inherit the original material's texture.
        if (captured.texture == stock) continue;
        if (captured.texture) textures.push_back(std::move(captured));
    }
    return true;
}
std::string skin_changer::CosmeticStatus(bool agent, int team)
{
    std::lock_guard lock(settingsMutex);
    if (!enabled || faulted)
        return backendStatus;
    const auto found = selections.find(CosmeticKey(agent, team));
    return found == selections.end() ? "" : found->second.status;
}
int skin_changer::CurrentTeam()
{
    return currentTeam.load();
}
std::string skin_changer::Status(int definition, bool knife)
{
    std::lock_guard lock(settingsMutex);
    if (!enabled || faulted || !definition)
        return backendStatus;
    const auto found = selections.find(Key(definition, knife));
    return found == selections.end() ? "" : found->second.status;
}
bool skin_changer::CustomModelAvailable(const std::string &model)
{
    if (!custom_models::Find(model))
        return false;
    const auto game = runtime_paths::GameDirectory();
    if (game.empty())
        return false;
    std::error_code error;
    return std::filesystem::is_regular_file(runtime_paths::NativePath(game / (model + "_c")), error);
}
bool skin_changer::Install()
{
    if (enabled)
        return true;
    if (!hookCreated && !BindFunctions())
    {
        std::lock_guard lock(settingsMutex);
        backendStatus = bindingFailure;
        return false;
    }
    if (!hookCreated)
    {
        if (MH_CreateHook(hookTarget, reinterpret_cast<void *>(StageHook), reinterpret_cast<void **>(&originalStage)) !=
            MH_OK)
        {
            std::lock_guard lock(settingsMutex);
            backendStatus = "Could not create the skin update hook";
            return false;
        }
        hookCreated = true;
    }
    faulted = false;
    nextTick = 0;
    applied.clear();
    cosmeticStates.clear();
    lobbyModels.clear();
    nextLobbyTick = 0;
    enemyModels.clear();
    enemyChanges = 0;
    enemyStopping = false;
    enemyPrecacheRevision = 0;
    enemyPrecached = false;
    SeedDefaultAgentSelections();
    enabled = true;
    const MH_STATUS result = MH_EnableHook(hookTarget);
    if (result != MH_OK && result != MH_ERROR_ENABLED)
        enabled = false;
    std::lock_guard lock(settingsMutex);
    backendStatus = enabled ? "Skin updater ready" : "Could not enable the skin update hook";
    return enabled;
}
bool skin_changer::Stop()
{
    enemyStopping = true;
    const auto deadline = GetTickCount64() + 2000;
    while ((enemyChanges.load() || enemyCallbacks.load()) && GetTickCount64() < deadline)
        Sleep(1);
    if (enemyChanges.load() || enemyCallbacks.load())
    {
        std::lock_guard lock(settingsMutex);
        backendStatus = "Waiting for game-thread enemy model restore; keep the DLL loaded.";
        enemyStatus = backendStatus;
        return false;
    }
    enabled = false;
    currentTeam = 0;
    if (hookCreated)
    {
        const MH_STATUS result = MH_DisableHook(hookTarget);
        if (result != MH_OK && result != MH_ERROR_DISABLED)
            return false;
    }
    while (callbacks.load() != 0)
        Sleep(1);
    lobbyModels.clear();
    enemyModels.clear();
    stageCallback = nullptr;
    beforeStageCallback = nullptr;
    afterStageCallback = nullptr;
    // Retain the trampoline for callbacks that entered just before disable.
    std::lock_guard lock(settingsMutex);
    backendStatus = "Skin updater stopped";
    return true;
}
void skin_changer::SetStageCallback(void (*callback)())
{
    stageCallback = callback;
}
void skin_changer::SetBeforeStageCallback(void (*callback)(int))
{
    beforeStageCallback = callback;
}

void skin_changer::SetAfterStageCallback(void (*callback)(int))
{
    afterStageCallback = callback;
}
