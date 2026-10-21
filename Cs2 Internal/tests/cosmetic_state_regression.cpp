#include "features/cosmetics/skin_changer.cpp"
#include "player_fixture.h"
#include <iostream>
#include <fstream>
namespace
{
    unsigned precaches = 0, purges = 0, requests = 0, originals = 0;
    Address expectedPawn = 0;
    const char *expectedPath = "agents/models/test/missing.vmdl";
    const char *__fastcall InsertStub(void *buffer, int, const char *path, int, bool)
    {
        static_cast<BufferString *>(buffer)->storage = const_cast<char *>(path);
        return path;
    }
    void __fastcall PurgeStub(void *buffer, int)
    {
        ++purges;
        static_cast<BufferString *>(buffer)->storage = nullptr;
    }
    void *__fastcall PrecacheStub(void *, void *buffer, const char *)
    {
        Check(std::strcmp(static_cast<BufferString *>(buffer)->storage, expectedPath) == 0, "precache path");
        ++precaches;
        return nullptr;
    }
    void __fastcall ModelStub(void *pawn, const char *path)
    {
        Check(reinterpret_cast<Address>(pawn) == expectedPawn && std::strcmp(path, expectedPath) == 0,
              "model request target");
        Check(precaches > 0, "SetModel before precache");
        ++requests;
    }
    void __fastcall StageStub(void *, int stage)
    {
        if (stage == 0)
            Check(requests > 0, "agent request did not precede original FRAME_START");
        ++originals;
    }
    std::array<unsigned char, 3 * 0x48> gloveAttributes{};
    unsigned gloveRefreshes = 0, bodygroups = 0;
    void __fastcall AttributeStub(void *pointer, const char *name, float value)
    {
        const Address item = reinterpret_cast<Address>(pointer);
        const unsigned slot = std::strcmp(name, "set item texture prefab") == 0 ? 0
                              : std::strcmp(name, "set item texture seed") == 0 ? 1
                                                                                : 2;
        Write(item + field::C_EconItemView::m_AttributeList + 8, 3);
        Write(item + field::C_EconItemView::m_AttributeList + 16, reinterpret_cast<Address>(gloveAttributes.data()));
        const auto attribute = reinterpret_cast<Address>(gloveAttributes.data()) + slot * 0x48;
        Write(attribute + field::CEconItemAttribute::m_iAttributeDefinitionIndex, static_cast<std::uint16_t>(6 + slot));
        Write(attribute + field::CEconItemAttribute::m_flValue, value);
    }
    int __fastcall PaintStub(void *item)
    {
        float paint = 0;
        const auto attribute = FindAttribute(reinterpret_cast<Address>(item), 6);
        Read(attribute + field::CEconItemAttribute::m_flValue, paint);
        return static_cast<int>(paint);
    }
    void __fastcall InvalidateStub(void *) {}
    void __fastcall MeshStub(void *state, const std::uint64_t *mask)
    {
        Write(reinterpret_cast<Address>(state) + field::CModelState::m_MeshGroupMask, *mask);
    }
    void __fastcall StockModelStub(void *entity, const char *path)
    {
        Check(reinterpret_cast<Address>(entity) == expectedPawn &&
              std::strcmp(path, custom_models::StockModel(7)) == 0, "lobby stock restore target");
        ++requests;
    }
    void __fastcall GlovePostStub(void *pointer, int update)
    {
        const Address pawn = reinterpret_cast<Address>(pointer), item = pawn + field::C_CSPlayerPawn::m_EconGloves;
        bool reapply = false, restore = false;
        std::uint64_t id = 0;
        std::uint32_t account = 0;
        Read(pawn + field::C_CSPlayerPawn::m_bNeedToReApplyGloves, reapply);
        Read(item + field::C_EconItemView::m_bRestoreCustomMaterialAfterPrecache, restore);
        Read(item + field::C_EconItemView::m_iItemID, id);
        Read(item + field::C_EconItemView::m_iAccountID, account);
        Check(pawn == expectedPawn && update == 1 && reapply && restore, "glove native refresh sequence");
        Check(id == kGloveItemId && account == 123456, "glove identity/account incomplete");
        ++gloveRefreshes;
    }
    void __fastcall BodygroupStub(void *pawn, int group, unsigned value)
    {
        Check(reinterpret_cast<Address>(pawn) == expectedPawn && group >= 0 && group <= 1 && value == 1,
              "glove bodygroup");
        ++bodygroups;
    }
    void DefaultAgentSelections()
    {
        namespace fs = std::filesystem;
        struct GameFixture
        {
            fs::path temporary, root;
            std::wstring oldGameDirectory;
            bool hadGameDirectory = false;
            GameFixture()
            {
                wchar_t value[32768]{};
                const auto length = GetEnvironmentVariableW(L"CS2_GAME_DIR", value, 32768);
                Check(length < 32768, "Game directory fixture environment is too long");
                hadGameDirectory = length != 0;
                if (hadGameDirectory) oldGameDirectory.assign(value, length);
                Check(GetTempPathW(32768, value) != 0, "Temporary model fixture directory unavailable");
                temporary = fs::weakly_canonical(value);
                root = temporary / (L"CS2-default-agent-" + std::to_wstring(GetCurrentProcessId()));
                Check(root.is_absolute() && root.parent_path() == temporary, "Unsafe model fixture directory");
                fs::create_directories(root);
                std::ofstream(root / L"gameinfo.gi") << "default model fixture";
                Check(SetEnvironmentVariableW(L"CS2_GAME_DIR", root.c_str()) != FALSE, "Could not set model fixture game directory");
            }
            ~GameFixture()
            {
                SetEnvironmentVariableW(L"CS2_GAME_DIR", hadGameDirectory ? oldGameDirectory.c_str() : nullptr);
                std::error_code error;
                if (root.is_absolute() && root.parent_path() == temporary &&
                    root.filename() == L"CS2-default-agent-" + std::to_wstring(GetCurrentProcessId()))
                    fs::remove_all(root, error);
            }
            void Add(const char *relative)
            {
                const auto path = root / relative;
                fs::create_directories(path.parent_path());
                std::ofstream(path, std::ios::binary) << "resource fixture";
            }
        } files;
        selections.clear(); enemySelection = {}; enemyRevision = nextRevision = 0;
        SeedDefaultAgentSelections();
        Check(selections.empty() && !skin_changer::GetEnemyModel().enabled && !nextRevision,
              "Missing custom character replaced the stock model defaults");
        files.Add(kDefaultAgentResources.front());
        SeedDefaultAgentSelections();
        Check(selections.empty() && !enemyRevision,
              "Incomplete custom character bundle was selected by default");
        for (const auto *resource : kDefaultAgentResources) files.Add(resource);
        Check(DefaultAgentAvailable(), "Complete installed custom character was not available");
        SeedDefaultAgentSelections();
        for (const int team : {2, 3})
        {
            skin_changer::Selection choice;
            Check(skin_changer::GetCosmetic(true, team, choice) && choice.team == team &&
                  choice.agentModel && choice.customModel && !choice.gloves && !choice.knife &&
                  choice.model == kDefaultAgentModel && choice.name == kDefaultAgentName,
                  "T/CT default character selection was not queued through cosmetics");
        }
        const auto enemy = skin_changer::GetEnemyModel();
        Check(enemy.enabled && enemy.model == kDefaultAgentModel && enemy.name == kDefaultAgentName,
              "Default character was not enabled for enemies");
        const auto revision = nextRevision;
        SeedDefaultAgentSelections();
        Check(nextRevision == revision && selections.size() == 2,
              "Hook reinstall repeatedly changed model selections");
        Check(!requests && !precaches, "Default configuration made native model calls outside a frame update");

        // The queued default uses the ordinary native loader, including async
        // confirmation and the separate team slot when the local team changes.
        PlayerFixture player;
        clientBase = player.Base(); enabled = true; expectedPawn = player.Pawn(); expectedPath = kDefaultAgentModel;
        std::vector<unsigned char> scene(0x5000), resource(32);
        Address binding = reinterpret_cast<Address>(resource.data());
        const char original[] = "agents/models/test/original.vmdl";
        player.Set(resource, skin_bindings::kModelNameOffset, reinterpret_cast<Address>(original));
        player.Set(scene, field::CSkeletonInstance::m_modelState + field::CModelState::m_hModel,
                   reinterpret_cast<Address>(&binding));
        player.Set(player.pawn, field::C_BaseEntity::m_pGameSceneNode, reinterpret_cast<Address>(scene.data()));
        insertString = InsertStub; purgeString = PurgeStub; precacheResource = PrecacheStub;
        resourceSystem = &player; setModel = ModelStub;
        TickCosmetics(1000, true);
        Check(requests == 1 && precaches == 1 && skin_changer::CosmeticStatus(true, 2) == "Loading selected agent...",
              "Default agent bypassed the normal precache/loading lifecycle");
        player.Set(resource, skin_bindings::kModelNameOffset, reinterpret_cast<Address>(expectedPath));
        TickCosmetics(1100, true);
        Check(skin_changer::CosmeticStatus(true, 2) == "Agent applied" && requests == 1,
              "Default agent was not confirmed after streaming");
        player.Team(3);
        player.Set(resource, skin_bindings::kModelNameOffset, reinterpret_cast<Address>(original));
        TickCosmetics(1200, true);
        Check(requests == 2 && precaches == 2, "CT local team did not load its default character");

        skin_changer::Selection custom;
        custom.agentModel = true; custom.team = 2; custom.name = "User choice";
        custom.model = "agents/models/test/user_choice.vmdl";
        Check(skin_changer::Save(custom) && skin_changer::SaveEnemyModel({}), "Explicit model overrides rejected");
        SeedDefaultAgentSelections();
        skin_changer::Selection selected;
        Check(skin_changer::GetCosmetic(true, 2, selected) && selected.model == custom.model &&
              !skin_changer::GetEnemyModel().enabled && skin_changer::GetEnemyModel().model.empty(),
              "Default model seeding overwrote a local choice or explicit enemy clear");
        const auto disabledRevision = enemyRevision;
        const auto defaultEnemy = skin_changer::EnemyModelSelection{false, kDefaultAgentModel, kDefaultAgentName};
        Check(skin_changer::SaveEnemyModel(defaultEnemy), "Explicit disabled custom model rejected");
        SeedDefaultAgentSelections();
        Check(!skin_changer::GetEnemyModel().enabled && enemyRevision == disabledRevision + 1,
              "Default model seeding re-enabled an explicitly disabled enemy model");
        selections.clear(); cosmeticStates.clear(); enemySelection = {}; enemyRevision = nextRevision = 0;
        clientBase = 0; enabled = false; expectedPawn = 0; expectedPath = "agents/models/test/missing.vmdl";
        insertString = nullptr; purgeString = nullptr; precacheResource = nullptr; resourceSystem = nullptr; setModel = nullptr;
        requests = precaches = purges = 0;
    }
} // namespace
int main()
{
    try
    {
        DefaultAgentSelections();
        PlayerFixture fixture;
        clientBase = fixture.Base();
        enabled = true;
        expectedPawn = fixture.Pawn();
        std::vector<unsigned char> scene(0x5000), resource(32);
        const char actualName[] = "AGENTS\\models\\test\\agent.vmdl_c";
        Address resourceAddress = reinterpret_cast<Address>(resource.data());
        fixture.Set(resource, skin_bindings::kModelNameOffset, reinterpret_cast<Address>(actualName));
        fixture.Set(scene, field::CSkeletonInstance::m_modelState + field::CModelState::m_hModel,
                    reinterpret_cast<Address>(&resourceAddress));
        fixture.Set(fixture.pawn, field::C_BaseEntity::m_pGameSceneNode, reinterpret_cast<Address>(scene.data()));
        skin_changer::Selection choice;
        choice.agentModel = true;
        choice.team = 3;
        choice.model = "agents/models/test/agent.vmdl";
        Check(skin_changer::Save(choice), "save CT agent");
        TickCosmetics(1000, true);
        Check(skin_changer::CurrentTeam() == 2, "current team");
        Check(skin_changer::CosmeticStatus(true, 3) == "Waiting for CT team", "wrong team was left waiting for player");
        fixture.Team(3);
        TickCosmetics(1100, true);
        Check(skin_changer::CosmeticStatus(true, 3) == "Agent applied",
              "already applied model retained stale waiting status");
        fixture.Health(0);
        TickCosmetics(1200, true);
        Check(skin_changer::CosmeticStatus(true, 3) == "Respawn to apply", "dead player state");
        fixture.Health(100);
        TickCosmetics(1300, true);
        Check(skin_changer::CosmeticStatus(true, 3) == "Agent applied", "respawn did not re-evaluate model");
        choice.model = expectedPath;
        const auto saveCustom = [&]() {
            Check(skin_changer::Save(choice), "save fixture");
            // Synthetic resource: native loading is independent of filesystem validation.
            selections[CosmeticKey(true, 3)].choice.customModel = true;
        };
        saveCustom();
        TickCosmetics(1400, true);
        Check(skin_changer::CosmeticStatus(true, 3) == "Agent loader unavailable; check game build.",
              "loader failure swallowed");
        insertString = InsertStub;
        purgeString = PurgeStub;
        precacheResource = PrecacheStub;
        resourceSystem = &fixture;
        setModel = ModelStub;
        saveCustom();
        TickCosmetics(2000, true);
        Check(precaches == 1 && purges == 1 && requests == 1, "uncached model never reached SetModel");
        Check(skin_changer::CosmeticStatus(true, 3) == "Loading selected agent...",
              "unloaded resource reported success");
        TickCosmetics(2500, true);
        Check(requests == 1 && precaches == 1, "per-frame request flood");
        fixture.Set(resource, skin_bindings::kModelNameOffset, reinterpret_cast<Address>(expectedPath));
        TickCosmetics(2600, true);
        Check(skin_changer::CosmeticStatus(true, 3) == "Agent applied", "asynchronous assignment not confirmed");
        bool refresh = false;
        Read(fixture.Pawn() + field::C_CSPlayerPawn::m_bNeedToReApplyGloves, refresh);
        Check(refresh, "confirmed agent did not refresh gloves");
        fixture.Set(resource, skin_bindings::kModelNameOffset, reinterpret_cast<Address>(actualName));
        TickCosmetics(3000, true);
        Check(requests == 2 && precaches == 2, "server model reset not reapplied");
        TickCosmetics(4000, true);
        TickCosmetics(5000, true);
        TickCosmetics(6000, true);
        Check(requests == 4, "streaming retries were not bounded");
        TickCosmetics(8000, true);
        Check(skin_changer::CosmeticStatus(true, 3).find("timed out") != std::string::npos, "loading did not time out");
        TickCosmetics(9000, true);
        Check(requests == 4, "failed load retried forever");
        saveCustom();
        TickCosmetics(10000, true);
        Check(requests == 5, "Apply did not restart failed load");
        fixture.Set(fixture.page, 0x80, std::uint32_t{0x10001});
        TickCosmetics(11000, true);
        Check(skin_changer::CosmeticStatus(true, 3) == "Waiting for spawn" && requests == 5,
              "stale entity handle accepted");
        fixture.Set(fixture.page, 0x80, std::uint32_t{0x8001});
        originalStage = StageStub;
        requests = 0;
        StageHook(nullptr, 0);
        Check(requests == 1 && originals == 1, "FRAME_START dispatch");
        StageHook(nullptr, 7);
        Check(requests == 1 && originals == 2, "agent ran during data update");
        Check(!game_access::SameModelPath(choice.model, "agents/models/test/missing_other.vmdl"),
              "different resource matched");
        selections.clear();
        cosmeticStates.clear();
        fixture.Set(fixture.controller, field::CBasePlayerController::m_steamID,
                    std::uint64_t{0x110000100000000ull + 123456});
        setAttribute = AttributeStub;
        getPaintKit = PaintStub;
        invalidateItem = InvalidateStub;
        choice = {};
        choice.gloves = true;
        choice.definition = 5030;
        choice.paintkit = 10018;
        choice.seed = 71;
        choice.wear = .15f;
        Check(skin_changer::Save(choice), "save gloves");
        TickCosmetics(12000, false);
        Check(skin_changer::CosmeticStatus(false) == "Glove refresh unavailable for this game build.",
              "missing native glove refresh not reported");
        playerPostDataUpdate = GlovePostStub;
        setBodygroup = BodygroupStub;
        Check(skin_changer::Save(choice), "retry gloves");
        TickCosmetics(12100, false);
        Check(gloveRefreshes == 1 && bodygroups == 2 && GloveMatches(fixture.Pawn(), choice),
              "glove data saved without native refresh");
        TickCosmetics(12200, false);
        Check(gloveRefreshes == 1, "gloves rebuilt every tick");
        const auto item = fixture.Pawn() + field::C_CSPlayerPawn::m_EconGloves;
        Write(item + field::C_EconItemView::m_iItemID, std::uint64_t{0});
        TickCosmetics(12300, false);
        Check(gloveRefreshes == 2, "server glove identity reset was ignored");
        choice.paintkit = 10019;
        Check(skin_changer::Save(choice), "change glove finish");
        TickCosmetics(12400, false);
        Check(gloveRefreshes == 3 && PaintStub(reinterpret_cast<void *>(item)) == 10019,
              "glove finish change not refreshed");
        fixture.Health(0);
        TickCosmetics(12500, false);
        Check(gloveRefreshes == 3, "gloves changed while dead");
        fixture.Health(100);
        fixture.Team(2);
        Write(item + field::C_EconItemView::m_iItemID, std::uint64_t{0});
        TickCosmetics(12600, false);
        Check(gloveRefreshes == 4 && bodygroups == 7, "T glove refresh/respawn");
        // A lobby item preview is a CBaseAnimGraph, with no pawn or econ data.
        // It must work with no local controller and must reject other classes.
        selections.clear();
        lobbyModels.clear();
        nextLobbyTick = 0;
        fixture.Set(fixture.globals, global::dwLocalPlayerController, Address{0});
        std::vector<unsigned char> preview(0x1300);
        expectedPawn = reinterpret_cast<Address>(preview.data());
        fixture.Set(fixture.page, 2 * 0x70, expectedPawn);
        fixture.Set(fixture.page, 2 * 0x70 + 0x10, std::uint32_t{0x8002});
        // MSVC primary-vtable RTTI with a module-relative type descriptor.
        fixture.Set(preview, 0, clientBase + 0x208);
        fixture.Set(fixture.globals, 0x200, clientBase + 0x300);
        fixture.Set(fixture.globals, 0x300, std::uint32_t{1});
        fixture.Set(fixture.globals, 0x30c, std::uint32_t{0x400});
        fixture.Set(fixture.globals, 0x314, std::uint32_t{0x300});
        const char previewClass[] = ".?AVC_CSGO_PreviewModel@@";
        std::memcpy(fixture.globals.data() + 0x410, previewClass, sizeof(previewClass));
        Check(IsLobbyPreview(expectedPawn), "lobby preview class not recognized");
        fixture.Set(preview, field::C_BaseEntity::m_pGameSceneNode, reinterpret_cast<Address>(scene.data()));
        const char *stock = custom_models::StockModel(7);
        fixture.Set(resource, skin_bindings::kModelNameOffset, reinterpret_cast<Address>(stock));
        fixture.Set(scene, field::CSkeletonInstance::m_modelState + field::CModelState::m_MeshGroupMask,
                    std::uint64_t{2});
        choice = {};
        choice.definition = 7;
        choice.customModel = true;
        choice.model = custom_models::kModels[0].path;
        expectedPath = choice.model.c_str();
        selections[7] = {choice, ++nextRevision, "Saved"};
        setMesh = MeshStub;
        setModel = ModelStub;
        requests = precaches = 0;
        TickLobbyModels(20000);
        Check(requests == 1 && precaches == 1 && skin_changer::Status(7) == "Loading selected lobby model...",
              "lobby requires live pawn or writes econ fields");
        TickLobbyModels(20500);
        Check(requests == 1, "lobby request flood");
        fixture.Set(resource, skin_bindings::kModelNameOffset, reinterpret_cast<Address>(expectedPath));
        TickLobbyModels(20600);
        Check(skin_changer::Status(7) == "Applied in lobby (local preview)", "lobby async loading not confirmed");
        std::uint64_t meshMask = 0;
        Read(reinterpret_cast<Address>(scene.data()) + field::CSkeletonInstance::m_modelState +
                 field::CModelState::m_MeshGroupMask, meshMask);
        Check(meshMask == 1, "custom lobby mesh remains hidden");
        TickLobbyModels(20700);
        Check(requests == 1, "confirmed lobby model rebuilt each frame");
        fixture.Set(resource, skin_bindings::kModelNameOffset, reinterpret_cast<Address>(stock));
        TickLobbyModels(20800);
        Check(requests == 2, "lobby model reset not reapplied");
        TickLobbyModels(22000);
        TickLobbyModels(23000);
        TickLobbyModels(24000);
        TickLobbyModels(26000);
        Check(requests == 5 && skin_changer::Status(7).find("timed out") != std::string::npos,
              "lobby loading retries not bounded");
        selections[7].revision = ++nextRevision;
        TickLobbyModels(27000);
        Check(requests == 6, "Apply did not retry lobby model");
        fixture.Set(resource, skin_bindings::kModelNameOffset, reinterpret_cast<Address>(expectedPath));
        selections[7].choice.customModel = false;
        selections[7].choice.model = stock;
        selections[7].revision = ++nextRevision;
        setModel = StockModelStub;
        TickLobbyModels(27100);
        Check(requests == 7, "switching to stock left custom lobby model assigned");
        fixture.Set(fixture.page, 2 * 0x70 + 0x10, std::uint32_t{0x10002});
        std::memcpy(fixture.globals.data() + 0x410, ".?AVC_BaseAnimGraph@@", 21);
        Check(!IsLobbyPreview(expectedPawn), "unrelated entity accepted as lobby preview");
        TickLobbyModels(28000);
        Check(requests == 7 && lobbyModels.empty(), "recycled/unrelated lobby entity retained");
        std::cout << "PASS: installed/default model bundles, stock fallback, user override preservation; agent loading/state/frame ordering; glove identity, float attributes, account, native "
                     "refresh, bodygroups, finish changes, server resets and respawn; lobby preview class isolation, "
                     "loading, retries, resets, stock restore and recycled handles\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
