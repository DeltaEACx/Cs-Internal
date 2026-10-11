#include "platform/windows.h"
#include "ui/agent_preview.h"
#include "render/preview_scene.h"
#include "core/runtime_paths.h"
#include "game/game_access.h"
#include "game/custom_models.h"
#include "ui/catalog_ui.h"
#include "ui/menu_shell.h"
#include "features/cosmetics/skin_changer.h"
#include "game/sdk/SkinBindings.h"
#include "imgui/imgui.h"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>

namespace agent_preview
{
    namespace
    {
        using namespace game_access;
        namespace fs = std::filesystem;
        struct Snapshot
        {
            bool valid = false;
            int definition = 0, paintkit = 0;
            ULONGLONG time = 0;
            std::string agent, weapon, pose;
            int seed = 0, statTrak = -1;
            float wear = .01f;
            bool legacy = false, awaitingFinish = false, finishSupported = true;
            std::uint64_t revision = 0;
            std::vector<preview_scene::MaterialTexture> materials;
            preview_scene::ImageFinish image;
            std::string Key() const
            {
                auto key = agent + "|" + weapon + "|" + pose + "|" + std::to_string(definition) + "|" +
                    std::to_string(paintkit) + "|" + std::to_string(seed) + "|" +
                    std::to_string(std::bit_cast<std::uint32_t>(wear)) + "|" + std::to_string(statTrak) + "|" +
                    std::to_string(legacy) + "|" + std::to_string(revision);
                for (const auto &material : materials)
                    key += "|" + material.material + "@" +
                        std::to_string(reinterpret_cast<std::uintptr_t>(material.texture.Get()));
                key+="|image:"+std::to_string(reinterpret_cast<std::uintptr_t>(image.texture.Get()))+":"+std::to_string(image.cube);
                for(auto channel:image.tint)key+=":"+std::to_string(channel);
                return key;
            }
        };
        std::mutex mutex;
        std::condition_variable changed;
        Snapshot current, request;
        preview_scene::ImageFinish imageFinish;
        std::string requestedKey, completedKey, error;
        std::unique_ptr<preview_scene::Model> completed;
        std::thread loader;
        std::atomic<bool> cancel{false};
        bool pending = false;
        preview_scene::Renderer renderer;
        ID3D11Device *lastDevice = nullptr;
        std::string displayedKey, displayError;
        float yaw = -.8f, pitch = .05f, zoom = 1.f;
        bool rotate = false;
        std::string ModelName(Address entity)
        {
            Address scene = 0, handle = 0, resource = 0, name = 0;
            if (!Read(entity + field::C_BaseEntity::m_pGameSceneNode, scene) || !scene ||
                !Read(scene + field::CSkeletonInstance::m_modelState + field::CModelState::m_hModel, handle) ||
                !handle || !Read(handle, resource) || !resource ||
                !Read(resource + skin_bindings::kModelNameOffset, name) || !name)
                return {};
            std::string result;
            for (size_t i = 0; i < 260; ++i)
            {
                char ch = 0;
                if (!Read(name + i, ch))
                    return {};
                if (!ch)
                    return result;
                result += ch == '\\' ? '/' : ch;
            }
            return {};
        }
        bool SafeModel(const std::string &model)
        {
            if (model.empty() || !model.ends_with(".vmdl") || model.find("..") != std::string::npos ||
                model.find_first_of(":,\\\r\n\"") != std::string::npos)
                return false;
            return model.starts_with("agents/") || model.starts_with("characters/models/") ||
                   model.starts_with("weapons/models/") || model.starts_with("models/custom/") ||
                   model.starts_with("models/player/");
        }
        const char *Pose(int id)
        {
            if (custom_models::IsKnife(id))
                return "idle_knife";
            switch (id)
            {
            case 1:
            case 2:
            case 3:
            case 4:
            case 30:
            case 32:
            case 36:
            case 61:
            case 63:
            case 64:
                return "idle_pistol";
            default:
                return "idle_rifle";
            }
        }
        fs::path Export(const std::string &model, bool agent)
        {
            if (!SafeModel(model))
                throw std::runtime_error("This model format is not supported by the preview.");
            const auto game = runtime_paths::GameDirectory();
            auto relative = fs::path(std::u8string(model.begin(), model.end()));
            relative.replace_extension(L".glb");
            if (!game.empty() && fs::is_regular_file(runtime_paths::NativePath(game / relative)))
                return runtime_paths::NativePath(game / relative);
            const auto root = runtime_paths::DataRoot() / L"preview_models";
            std::ifstream stream(runtime_paths::NativePath(root / L"index.json"));
            if (!stream)
                throw std::runtime_error("3D assets missing. Keep cs2_assets/preview_models beside the DLL.");
            const auto index = nlohmann::json::parse(stream);
            auto found = index.find(model);
            if (found == index.end())
                for (auto it = index.begin(); it != index.end(); ++it)
                    if (SameModelPath(it.key(), model))
                    {
                        found = it;
                        break;
                    }
            if (found == index.end())
                throw std::runtime_error(
                    "No 3D preview for this custom model. Place a matching .glb beside its .vmdl_c file.");
            const auto name = found->get<std::string>();
            const auto path = fs::path(std::u8string(name.begin(), name.end()));
            if (path.has_root_path() || name.find("..") != std::string::npos || name.find(':') != std::string::npos)
                throw std::runtime_error("Invalid 3D asset index path.");
            return runtime_paths::NativePath(root / path);
        }
        void LoadLoop()
        {
            while (!cancel)
            {
                Snapshot selected;
                {
                    std::unique_lock lock(mutex);
                    changed.wait(lock, [] { return cancel || pending; });
                    if (cancel)
                        return;
                    selected = request;
                    pending = false;
                }
                std::unique_ptr<preview_scene::Model> result;
                std::string message;
                try
                {
                    auto agent = preview_scene::Load(Export(selected.agent, true), selected.pose, true,
                                                     Export("agents/models/ctm_sas/ctm_sas.vmdl", true));
                    if (cancel)
                        return;
                    if (!selected.weapon.empty() && agent.hasHand && agent.posed)
                    {
                        auto weapon = preview_scene::Load(Export(selected.weapon, false), "", false, {}, selected.legacy);
                        if (!selected.image.texture && selected.paintkit > 0 && !preview_scene::ApplyFinish(weapon, selected.materials))
                            throw std::runtime_error("Weapon skin materials do not match this preview model.");
                        preview_scene::ApplyImage(weapon,selected.image);
                        preview_scene::Attach(agent, std::move(weapon));
                    }
                    result = std::make_unique<preview_scene::Model>(std::move(agent));
                }
                catch (const std::exception &e)
                {
                    message = e.what();
                }
                if (cancel)
                    return;
                std::lock_guard lock(mutex);
                if (requestedKey == selected.Key())
                {
                    completed = std::move(result);
                    error = message;
                    completedKey = selected.Key();
                }
            }
        }
    } // namespace
    void SetImageFinish(const preview_scene::ImageFinish &image){std::lock_guard lock(mutex);imageFinish=image;current.image=image;}
    void Capture()
    {
        static ULONGLONG next = 0;
        const auto now = GetTickCount64();
        if (now < next)
            return;
        next = now + 100;
        Snapshot snapshot;
        snapshot.time = now;
        const Address base = verifiedClient.load();
        const auto player = ReadLocalPlayer(base);
        if (player.state == PlayerState::Alive)
        {
            snapshot.agent = ModelName(player.pawn);
            snapshot.pose = "idle_rifle";
            snapshot.valid = SafeModel(snapshot.agent);
            Address services = 0, weapon = 0;
            std::uint32_t weaponHandle = 0xffffffffu, owner = 0xffffffffu;
            std::uint16_t definition = 0;
            if (Read(player.pawn + field::C_BasePlayerPawn::m_pWeaponServices, services) && services &&
                Read(services + field::CPlayer_WeaponServices::m_hActiveWeapon, weaponHandle) &&
                (weapon = Resolve(player.list, weaponHandle)) &&
                Read(weapon + field::C_BaseEntity::m_hOwnerEntity, owner) &&
                Resolve(player.list, owner) == player.pawn &&
                Read(weapon + field::C_EconEntity::m_AttributeManager + field::C_AttributeContainer::m_Item +
                         field::C_EconItemView::m_iItemDefinitionIndex,
                     definition))
            {
                snapshot.weapon = ModelName(weapon);
                if (!SafeModel(snapshot.weapon))
                    snapshot.weapon.clear();
                snapshot.definition = definition;
                snapshot.pose = Pose(definition);
                snapshot.paintkit = skin_changer::ReadPaintKit(weapon);
                if (snapshot.paintkit < 0)
                    Read(weapon + field::C_EconEntity::m_nFallbackPaintKit, snapshot.paintkit);
                Read(weapon + field::C_EconEntity::m_nFallbackSeed, snapshot.seed);
                Read(weapon + field::C_EconEntity::m_flFallbackWear, snapshot.wear);
                Read(weapon + field::C_EconEntity::m_nFallbackStatTrak, snapshot.statTrak);
                Address scene = 0;
                std::uint64_t mesh = 0;
                if (Read(weapon + field::C_BaseEntity::m_pGameSceneNode, scene) && scene &&
                    Read(scene + field::CSkeletonInstance::m_modelState + field::CModelState::m_MeshGroupMask, mesh))
                    snapshot.legacy = mesh == 2;
                skin_changer::Selection choice;
                if (skin_changer::GetSelection(definition, custom_models::IsKnife(definition), choice, &snapshot.revision))
                    snapshot.awaitingFinish = choice.definition != definition || choice.paintkit != snapshot.paintkit ||
                        choice.seed != snapshot.seed || std::abs(choice.wear - snapshot.wear) > .00001f ||
                        (choice.statTrak ? choice.kills : -1) != snapshot.statTrak ||
                        (choice.legacy && !choice.knife && !choice.customModel) != snapshot.legacy ||
                        (!choice.model.empty() && !SameModelPath(choice.model, snapshot.weapon));
                if (snapshot.paintkit > 0)
                {
                    snapshot.finishSupported = skin_changer::ReadPreviewMaterials(weapon, snapshot.materials);
                    snapshot.awaitingFinish |= snapshot.materials.empty();
                }
            }
        }
        std::lock_guard lock(mutex);
        snapshot.image=imageFinish;
        current = std::move(snapshot);
    }
    void Draw(ID3D11Device *device, ID3D11DeviceContext *context, bool *open)
    {
        if (!*open)
            return;
        ImGui::SetNextWindowPos(menu_shell::previewPosition,ImGuiCond_Always);
        ImGui::SetNextWindowSize(menu_shell::previewSize,ImGuiCond_Always);
        if (!ImGui::Begin("Preview###Agent preview", nullptr, ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize))
        {
            ImGui::End();
            return;
        }
        Snapshot snapshot;
        {
            std::lock_guard lock(mutex);
            snapshot = current;
        }
        const auto selectedAgent = catalog_ui::SelectedAgentPreview();
        if (!selectedAgent.empty())
        {
            if (GetTickCount64() - snapshot.time > 2000 || !snapshot.valid)
                snapshot = {};
            snapshot.agent = selectedAgent;
            snapshot.valid = true;
            snapshot.time = GetTickCount64();
            if (snapshot.pose.empty())
                snapshot.pose = "idle_rifle";
            if (ImGui::Button("Show equipped"))
                catalog_ui::PreviewEquippedAgent();
        }
        if (!snapshot.valid || GetTickCount64() - snapshot.time > 2000)
        {
            ImGui::TextUnformatted("Select an agent to preview.");
            ImGui::End();
            return;
        }
        const auto key = snapshot.Key();
        ImGui::TextWrapped("%s", catalog_ui::AgentName(snapshot.agent).c_str());
        const auto *custom = custom_models::Find(snapshot.weapon);
        if (!snapshot.weapon.empty())
            ImGui::TextWrapped("%s", custom ? custom->name : catalog_ui::WeaponName(snapshot.definition).c_str());
        ImGui::Checkbox("Rotate", &rotate);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Automatic rotation. Drag the preview to orbit; scroll to zoom.");
        ImGui::SameLine();
        if (ImGui::Button("Reset"))
        {
            yaw = -.8f;
            pitch = .05f;
            zoom = 1.f;
        }
        ImGui::SameLine();
        const bool retry = ImGui::Button("Reload");
        ImGui::Separator();
        skin_changer::Selection latest;
        std::uint64_t latestRevision = 0;
        const bool changedSelection = snapshot.definition &&
            skin_changer::GetSelection(snapshot.definition, custom_models::IsKnife(snapshot.definition), latest, &latestRevision) &&
            latestRevision != snapshot.revision;
        if (changedSelection || (!snapshot.image.texture && (snapshot.awaitingFinish || !snapshot.finishSupported)))
        {
            catalog_ui::DrawEquippedFinish(device, changedSelection ? latest.definition : snapshot.definition,
                                          changedSelection ? latest.paintkit : snapshot.paintkit, snapshot.weapon);
            ImGui::End();
            return;
        }
        try
        {
            if (lastDevice != device)
            {
                renderer.Reset();
                lastDevice = device;
                displayedKey.clear();
            }
            std::unique_ptr<preview_scene::Model> ready;
            {
                std::lock_guard lock(mutex);
                if (requestedKey != key || retry ||
                    (displayedKey != key && completedKey == key && !completed && error.empty()))
                {
                    request = snapshot;
                    requestedKey = key;
                    completedKey.clear();
                    completed.reset();
                    error.clear();
                    pending = true;
                    displayError.clear();
                    changed.notify_one();
                }
                if (completedKey == key)
                {
                    ready = std::move(completed);
                    displayError = error;
                }
            }
            if (!loader.joinable())
                loader = std::thread(LoadLoop);
            if (ready)
            {
                renderer.Upload(device, *ready);
                displayedKey = key;
            }
            if (!displayError.empty())
                ImGui::TextWrapped("%s", displayError.c_str());
            else if (displayedKey != key)
                ImGui::TextUnformatted("Loading...");
            else
            {
                if (rotate)
                    yaw += ImGui::GetIO().DeltaTime * .35f;
                const ImVec2 size(
                    std::max(1.f, ImGui::GetContentRegionAvail().x),
                    std::max(120.f, ImGui::GetContentRegionAvail().y - (snapshot.weapon.empty() ? 0.f : 72.f)));
                auto *image = renderer.Render(context, static_cast<unsigned>(size.x), static_cast<unsigned>(size.y),
                                              yaw, pitch, zoom);
                ImGui::Image(image, size);
                if (ImGui::IsItemHovered())
                {
                    const auto &io = ImGui::GetIO();
                    if (ImGui::IsMouseDragging(0))
                    {
                        yaw -= io.MouseDelta.x * .01f;
                        pitch = std::clamp(pitch + io.MouseDelta.y * .006f, -.6f, .6f);
                    }
                    zoom = std::clamp(zoom + io.MouseWheel * .1f, .65f, 1.65f);
                }
                catalog_ui::DrawEquippedFinish(device, snapshot.definition, snapshot.paintkit, snapshot.weapon);
            }
        }
        catch (const std::exception &e)
        {
            displayError = e.what();
            ImGui::TextWrapped("%s", displayError.c_str());
        }
        ImGui::End();
    }
    WebFrame RenderWeb(ID3D11Device* device, ID3D11DeviceContext* context, WebAction action,
                       float x, float y, bool enabled, unsigned width, unsigned height)
    {
        WebFrame result;
        if(action==WebAction::Rotate)rotate=enabled;
        if(action==WebAction::Reset){yaw=-.8f;pitch=.05f;zoom=1.f;}
        if(action==WebAction::Equipped)catalog_ui::PreviewEquippedAgent();
        if(action==WebAction::Drag){yaw-=x*.01f;pitch=std::clamp(pitch+y*.006f,-.6f,.6f);}
        if(action==WebAction::Zoom)zoom=std::clamp(zoom+x*.1f,.65f,1.65f);
        result.rotate=rotate;
        Snapshot snapshot;
        {std::lock_guard lock(mutex);snapshot=current;}
        const auto selected=catalog_ui::SelectedAgentPreview();
        if(!selected.empty()) {
            if(GetTickCount64()-snapshot.time>2000||!snapshot.valid)snapshot={};
            snapshot.agent=selected;snapshot.valid=true;snapshot.time=GetTickCount64();
            if(snapshot.pose.empty())snapshot.pose="idle_rifle";
        }
        if(!snapshot.valid||GetTickCount64()-snapshot.time>2000){result.message="Select an agent to preview.";return result;}
        result.name=catalog_ui::AgentName(snapshot.agent);
        if(!snapshot.weapon.empty()) {
            const auto* custom=custom_models::Find(snapshot.weapon);
            result.weapon=custom?custom->name:catalog_ui::WeaponName(snapshot.definition);
        }
        skin_changer::Selection latest;std::uint64_t latestRevision{};
        const bool changedSelection=snapshot.definition&&skin_changer::GetSelection(snapshot.definition,
            custom_models::IsKnife(snapshot.definition),latest,&latestRevision)&&latestRevision!=snapshot.revision;
        if(changedSelection||(!snapshot.image.texture&&(snapshot.awaitingFinish||!snapshot.finishSupported))) {
            result.status="loading";result.message="Updating equipped finish";return result;
        }
        try {
            if(!device||!context)throw std::runtime_error("Preview renderer is unavailable.");
            if(lastDevice!=device){renderer.Reset();lastDevice=device;displayedKey.clear();}
            const auto key=snapshot.Key();std::unique_ptr<preview_scene::Model> ready;
            {
                std::lock_guard lock(mutex);
                if(requestedKey!=key||action==WebAction::Reload||
                    (displayedKey!=key&&completedKey==key&&!completed&&error.empty())) {
                    request=snapshot;requestedKey=key;completedKey.clear();completed.reset();error.clear();
                    pending=true;displayError.clear();changed.notify_one();
                }
                if(completedKey==key){ready=std::move(completed);displayError=error;}
            }
            if(!loader.joinable())loader=std::thread(LoadLoop);
            if(ready){renderer.Upload(device,*ready);displayedKey=key;}
            if(!displayError.empty()){result.status="error";result.message=displayError;return result;}
            if(displayedKey!=key){result.status="loading";result.message="Loading model";return result;}
            static ULONGLONG lastFrame{};
            const auto now=GetTickCount64();
            if(rotate&&lastFrame)yaw+=std::min(.25f,static_cast<float>(now-lastFrame)*.001f)*.35f;
            lastFrame=now;
            result.texture=renderer.Render(context,std::clamp(width,128u,1024u),std::clamp(height,128u,1024u),yaw,pitch,zoom);result.status="ready";
        }catch(const std::exception& exception){result.status="error";result.message=exception.what();}
        return result;
    }
    void Shutdown()
    {
        cancel = true;
        changed.notify_all();
        if (loader.joinable())
            loader.join();
        std::lock_guard lock(mutex);
        current = {};
        request = {};
        completed.reset();
        requestedKey.clear();
        completedKey.clear();
        error.clear();
        pending = false;
        renderer.Reset();
        lastDevice = nullptr;
        displayedKey.clear();
        displayError.clear();
        cancel = false;
    }
} // namespace agent_preview
