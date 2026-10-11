#include "core/user_defaults.hpp"
#include "chams.h"
#include "chams_materials.h"
#include "detail/primitive_buffer.hpp"
#include "game/sdk/ChamsBindings.h"
#include "effects/weapon_effects.hpp"
#include "enhancements.hpp"
#include "gun_images.h"
#include "game/custom_models.h"
#include "particle_chams.h"
#include "penetration_overlay.h"
#include "penetrable_props.h"
#include "backtrack_chams.h"
#include "features/combat/backtrack.h"

namespace visuals::chams
{
    namespace
    {
        constexpr auto targetCount = static_cast<std::size_t>(Target::Count);
        std::array<Configuration, targetCount> configurations = []
        {
            std::array<Configuration, targetCount> defaults{};
            auto &enemy = defaults[static_cast<std::size_t>(Target::Enemy)];
            enemy.enabled = true;
            // Lit shading keeps the custom model's shape readable; the flat
            // hidden pass gives its silhouette a distinct purple color.
            enemy.visible.material = Material::Matte;
            enemy.visible.tint = {50, 210, 235, 255};
            enemy.hidden.material = Material::Flat;
            enemy.hidden.tint = {155, 85, 235, 210};
            auto &weapon = defaults[static_cast<std::size_t>(Target::ViewmodelWeapon)];
            weapon.enabled = true;
            weapon.visible.material = Material::Pearl;
            weapon.visible.tint = {95, 220, 245, 255};
            weapon.visible.secondary = {185, 100, 255, 255};
            weapon.visible.animation = Animation::Gradient;
            weapon.visible.speed = .2f;
            weapon.hidden.enabled = false;
            return user_defaults::Chams(defaults);
        }();
        std::mutex configurationMutex;
        std::recursive_mutex sceneMutex;
        std::atomic<bool> ready{}, anyEnabled{true};
        std::uintptr_t client{};
        constexpr std::uint8_t FirstNonPlayer = static_cast<std::uint8_t>(Target::Arms);
        bool IsWeapon(std::uint32_t hash)
        {
            switch (hash)
            {
            case "C_DEagle"_hash: case "C_WeaponElite"_hash: case "C_WeaponFiveSeven"_hash:
            case "C_WeaponGlock"_hash: case "C_WeaponHKP2000"_hash: case "C_WeaponUSPSilencer"_hash:
            case "C_WeaponP250"_hash: case "C_WeaponCZ75a"_hash: case "C_WeaponTec9"_hash: case "C_WeaponRevolver"_hash:
            case "C_WeaponMAC10"_hash: case "C_WeaponMP5SD"_hash: case "C_WeaponMP7"_hash: case "C_WeaponMP9"_hash:
            case "C_WeaponBizon"_hash: case "C_WeaponP90"_hash: case "C_WeaponUMP45"_hash:
            case "C_AK47"_hash: case "C_WeaponM4A1"_hash: case "C_WeaponM4A1Silencer"_hash:
            case "C_WeaponAug"_hash: case "C_WeaponFamas"_hash: case "C_WeaponGalilAR"_hash: case "C_WeaponSG556"_hash:
            case "C_WeaponNOVA"_hash: case "C_WeaponSawedoff"_hash: case "C_WeaponXM1014"_hash: case "C_WeaponMag7"_hash:
            case "C_WeaponAWP"_hash: case "C_WeaponG3SG1"_hash: case "C_WeaponSCAR20"_hash: case "C_WeaponSSG08"_hash:
            case "C_WeaponM249"_hash: case "C_WeaponNegev"_hash: case "C_Knife"_hash: case "C_WeaponTaser"_hash:
            case "C_HEGrenade"_hash: case "C_Flashbang"_hash: case "C_SmokeGrenade"_hash:
            case "C_MolotovGrenade"_hash: case "C_IncendiaryGrenade"_hash: case "C_DecoyGrenade"_hash: case "C_C4"_hash:
                return true;
            default: return false;
            }
        }
        template <class T> std::optional<T> Field(std::uintptr_t object, const char *type, const char *name)
        {
            if (!object) return {};
            const auto offset = systems::schemas::lookup(type, fnv1a::runtime_hash(name));
            return offset ? memory::safe_read<T>(object + offset) : std::nullopt;
        }
        std::optional<EntityInfo> ReadEntity(std::uintptr_t scene)
        {
            const auto local = game_access::ReadLocalPlayer(client);
            if (!local.list) return {};
            const auto handle = memory::read<std::uint32_t>(scene + chams_bindings::kSceneOwner);
            const auto owner = ResolveSceneOwner(local.list, handle);
            if (!owner) return {};
            const auto name = memory::read_string(reinterpret_cast<std::uintptr_t>(systems::g_entities.get_schema_name(owner)), 96);
            if (name.empty()) return {};
            bool preview = false;
            if (name == "C_CSGO_PreviewModel" || name == "C_CSGO_PreviewModelAlias_csgo_item_previewmodel" ||
                name == "C_CSGO_TeamPreviewModel")
            {
                const auto node = Field<std::uintptr_t>(owner, "C_BaseEntity", "m_pGameSceneNode").value_or(0);
                const auto modelState = node + game_access::field::CSkeletonInstance::m_modelState;
                const auto handle = node ? memory::read<std::uintptr_t>(modelState + game_access::field::CModelState::m_hModel) : 0;
                const auto resource = memory::read<std::uintptr_t>(handle);
                const auto path = memory::read_string(memory::read<std::uintptr_t>(resource + skin_bindings::kModelNameOffset), 512);
                preview = IsWeaponPreview(name, path);
            }
            else if (IsWeapon(fnv1a::runtime_hash(name.c_str())))
            {
                // Lobby players can hold a regular weapon entity attached to
                // their preview scene, rather than a standalone item preview.
                const auto node = Field<std::uintptr_t>(owner, "C_BaseEntity", "m_pGameSceneNode").value_or(0);
                const auto parent = Field<std::uintptr_t>(node, "CGameSceneNode", "m_pParent").value_or(0);
                const auto parentOwner = Field<std::uintptr_t>(parent, "CGameSceneNode", "m_pOwner").value_or(0);
                const auto parentName = memory::read_string(reinterpret_cast<std::uintptr_t>(
                    systems::g_entities.get_schema_name(parentOwner)), 96);
                const auto identity = memory::read<std::uintptr_t>(parentOwner + 0x10);
                const auto handle = memory::read<std::uint32_t>(identity + 0x10);
                preview = IsPreviewPlayer(parentName) && game_access::Resolve(local.list, handle) == parentOwner;
            }
            if (!local.pawn && !preview) return {};
            auto viewPawn = local.pawn;
            if (local.state != game_access::PlayerState::Alive)
            {
                const auto observer = Field<std::uintptr_t>(local.pawn, "C_BasePlayerPawn", "m_pObserverServices").value_or(0);
                const auto target = Field<std::uint32_t>(observer, "CPlayer_ObserverServices", "m_hObserverTarget").value_or(0xffffffffu);
                if (const auto observed = game_access::Resolve(local.list, target)) viewPawn = observed;
            }
            EntityInfo info{owner, viewPawn, fnv1a::runtime_hash(name.c_str())};
            info.previewWeapon = preview;
            info.localTeam = Field<std::uint8_t>(viewPawn, "C_BaseEntity", "m_iTeamNum").value_or(0);
            if (info.classHash == "C_CSPlayerPawn"_hash)
            {
                const auto health = Field<int>(owner, "C_BaseEntity", "m_iHealth");
                const auto team = Field<std::uint8_t>(owner, "C_BaseEntity", "m_iTeamNum");
                if (!health || !team) return {};
                info.health = *health; info.team = *team;
            }
            else if (IsWeapon(info.classHash))
            {
                const auto node = Field<std::uintptr_t>(owner, "C_BaseEntity", "m_pGameSceneNode").value_or(0);
                const auto parent = Field<std::uintptr_t>(node, "CGameSceneNode", "m_pParent").value_or(0);
                info.localAttachment = Field<std::uintptr_t>(parent, "CGameSceneNode", "m_pOwner").value_or(0) == viewPawn;
            }
            return info;
        }
        struct SceneFlags
        {
            std::uintptr_t address{};
            std::optional<std::uint8_t> original;
            explicit SceneFlags(std::uintptr_t scene) : address(scene + chams_bindings::kSceneFlags), original(memory::safe_read<std::uint8_t>(address))
            {
                if (original) memory::safe_write(address, static_cast<std::uint8_t>(*original & ~chams_bindings::kOriginalMaterialFlag));
            }
            ~SceneFlags()
            {
                if (original)
                    if (const auto current = memory::safe_read<std::uint8_t>(address))
                        memory::safe_write(address, static_cast<std::uint8_t>((*current & ~chams_bindings::kOriginalMaterialFlag) | (*original & chams_bindings::kOriginalMaterialFlag)));
            }
        };
        bool LayerControls(const char *label, Layer &layer)
        {
            ImGui::PushID(label);
            bool changed = ImGui::Checkbox(label, &layer.enabled);
            if (layer.enabled)
            {
                int style = static_cast<int>(layer.material);
                const char *names[] = {"Liquid", "Metallic", "Matte", "Flat", "Bloom", "Outlines", "Glow", "Electric", "Distortion", "Hologram", "Pearl"};
                if (ImGui::Combo("Surface style", &style, names, static_cast<int>(Material::Count))) { layer.material = static_cast<Material>(style); changed = true; }
                float rgba[] = {layer.tint.r / 255.f, layer.tint.g / 255.f, layer.tint.b / 255.f, layer.tint.a / 255.f};
                if (ImGui::ColorEdit4("Tint / opacity", rgba))
                {
                    const auto byte = [](float value) { return static_cast<std::uint8_t>(std::isfinite(value) ? std::clamp(value, 0.f, 1.f) * 255 : 255); };
                    layer.tint = {byte(rgba[0]), byte(rgba[1]), byte(rgba[2]), byte(rgba[3])};
                    changed = true;
                }
                int animation=static_cast<int>(layer.animation);
                const char* animations[]={"Fixed color","Rainbow cycle","Pulse brightness","Two-color wave"};
                if(ImGui::Combo("Animation",&animation,animations,4)) {layer.animation=static_cast<Animation>(animation);changed=true;}
                if(layer.animation!=Animation::Static)changed|=ImGui::SliderFloat("Animation speed",&layer.speed,.1f,5,"%.1f");
                if(layer.animation==Animation::Gradient) {
                    float secondary[]={layer.secondary.r/255.f,layer.secondary.g/255.f,layer.secondary.b/255.f};
                    if(ImGui::ColorEdit3("Second color",secondary)) {
                        layer.secondary={static_cast<std::uint8_t>(std::clamp(secondary[0],0.f,1.f)*255),static_cast<std::uint8_t>(std::clamp(secondary[1],0.f,1.f)*255),static_cast<std::uint8_t>(std::clamp(secondary[2],0.f,1.f)*255),255};changed=true;
                    }
                }
            }
            ImGui::PopID();
            return changed;
        }
        void ConfigurationControls(Target target)
        {
            auto config = GetConfiguration(target);
            bool changed = ImGui::Checkbox("Enable material chams", &config.enabled);
            if (config.enabled)
            {
                changed |= LayerControls("Visible", config.visible);
                if (static_cast<std::uint8_t>(target) < FirstNonPlayer) changed |= LayerControls("Through walls", config.hidden);
            }
            if (changed) SetConfiguration(target, config);
        }
    }
    Configuration GetConfiguration(Target target)
    {
        std::lock_guard lock(configurationMutex);
        const auto index = static_cast<std::size_t>(target);
        return index < configurations.size() ? configurations[index] : Configuration{};
    }
    void SetConfiguration(Target target, Configuration config)
    {
        const auto index = static_cast<std::size_t>(target);
        if (index >= configurations.size()) return;
        for (auto *layer : {&config.visible, &config.hidden}) {
            if (layer->material >= Material::Count) layer->material = Material::Flat;
            if(layer->animation>=Animation::Count)layer->animation=Animation::Static;
            layer->speed=std::isfinite(layer->speed)?std::clamp(layer->speed,.1f,5.f):1.f;
        }
        if (index >= FirstNonPlayer) config.hidden.enabled = false;
        std::lock_guard lock(configurationMutex);
        configurations[index] = config;
        anyEnabled = std::ranges::any_of(configurations, [](const Configuration &value) { return value.enabled && (value.visible.enabled || value.hidden.enabled); });
    }
    Target Classify(const EntityInfo &entity)
    {
        if (!entity.owner) return Target::Unknown;
        if (entity.previewWeapon) return Target::ViewmodelWeapon;
        if (!entity.viewPawn) return Target::Unknown;
        if (entity.classHash == "C_CS2HudModelArms"_hash) return Target::Arms;
        if (entity.classHash == "C_CS2HudModelWeapon"_hash) return Target::ViewmodelWeapon;
        if (entity.classHash == "C_CSPlayerPawn"_hash)
        {
            if (entity.localTeam < 2 || entity.localTeam > 3 || entity.team < 2 || entity.team > 3) return Target::Unknown;
            const int category = entity.owner == entity.viewPawn ? 2 : entity.team == entity.localTeam ? 1 : 0;
            return static_cast<Target>(category + (entity.health <= 0 ? 3 : 0));
        }
        if (IsWeapon(entity.classHash)) return entity.localAttachment ? Target::ViewmodelWeapon : Target::Weapon;
        return Target::Unknown;
    }
    bool IsWeaponPreview(std::string_view className, std::string_view modelPath)
    {
        if (className != "C_CSGO_PreviewModel" &&
            className != "C_CSGO_PreviewModelAlias_csgo_item_previewmodel" &&
            className != "C_CSGO_TeamPreviewModel") return false;
        std::string path(modelPath);
        for (auto &c : path) c = c == '\\' ? '/' : c >= 'A' && c <= 'Z' ? char(c + 'a' - 'A') : c;
        if (path.ends_with("_c")) path.resize(path.size() - 2);
        if (const auto *model = custom_models::Find(path))
            return model->definition != 100 && model->definition != 101 && model->definition != 102;
        return path.starts_with("weapons/models/") && path.ends_with(".vmdl") &&
               path.find("/weapon_") != std::string::npos && path.find("..") == std::string::npos;
    }
    bool IsPreviewPlayer(std::string_view className)
    {
        return className == "C_CSGO_PreviewPlayer" ||
               className == "C_CSGO_PreviewPlayerAlias_csgo_player_previewmodel";
    }
    color AnimatedTint(const Layer &layer,double seconds,unsigned mesh)
    {
        if(!std::isfinite(seconds))return layer.tint;
        const double speed=std::isfinite(layer.speed)?std::clamp(layer.speed,.1f,5.f):1.f;
        const double phase=std::remainder(seconds*speed,1.0)*2*std::numbers::pi;
        const auto byte=[](double value){return static_cast<std::uint8_t>(std::clamp(value,0.0,255.0));};
        if(layer.animation==Animation::Rainbow) {
            float r,g,b;const auto hue=static_cast<float>(std::fmod(std::fmod(seconds*speed*.2,1.0)+1.0,1.0));
            ImGui::ColorConvertHSVtoRGB(hue,.8f,1,r,g,b);
            return {byte(r*255),byte(g*255),byte(b*255),layer.tint.a};
        }
        if(layer.animation==Animation::Breathing) {
            const auto brightness=.35+.65*(.5+.5*std::sin(phase));
            return {byte(layer.tint.r*brightness),byte(layer.tint.g*brightness),byte(layer.tint.b*brightness),layer.tint.a};
        }
        if(layer.animation==Animation::Gradient) {
            const auto blend=.5+.5*std::sin(phase+mesh*.65);
            return {byte(layer.tint.r+(layer.secondary.r-layer.tint.r)*blend),byte(layer.tint.g+(layer.secondary.g-layer.tint.g)*blend),byte(layer.tint.b+(layer.secondary.b-layer.tint.b)*blend),layer.tint.a};
        }
        return layer.tint;
    }
    std::uintptr_t ResolveSceneOwner(std::uintptr_t list, std::uint32_t sceneHandle)
    {
        if (!sceneHandle || sceneHandle == 0xffffffffu) return 0;
        const auto entity = game_access::Entity(list, sceneHandle & 0x7fffu);
        const auto identity = memory::read<std::uintptr_t>(entity + 0x10);
        const auto raw = memory::read<std::uint32_t>(identity + 0x10);
        const auto flags = memory::safe_read<std::uint32_t>(identity + 0x30);
        if (!flags || !raw || raw == 0xffffffffu || game_access::Resolve(list, raw) != entity) return 0;
        const auto adjusted = (((raw >> 15) - (*flags & 1u)) << 15) | (raw & 0x7fffu);
        return adjusted == sceneHandle ? entity : 0;
    }
    bool Initialize()
    {
        ready = false;
        client = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"client.dll"));
        ready = materials::Initialize();
        if (ready) backtrack_chams::Initialize();
        return ready;
    }
    void Update(bool inWorld)
    {
        if (!ready || !inWorld) return;
        const auto surfaces=penetration_overlay::Get();
        if(surfaces.enabled&&surfaces.props)materials::Ensure(Material::Flat,false);
        if(!anyEnabled)return;
        std::array<Configuration, targetCount> snapshot;
        { std::lock_guard lock(configurationMutex); snapshot = configurations; }
        for (const auto &config : snapshot)
        {
            if (!config.enabled) continue;
            if (config.hidden.enabled) materials::Ensure(config.hidden.material, true);
            if (config.visible.enabled) materials::Ensure(config.visible.material, false);
        }
    }
    bool Generate(std::uintptr_t self, std::uintptr_t scene, std::uintptr_t view, std::uintptr_t output, GenerateFn original)
    {
        if (!ready || !original || !scene) return false;
        if (backtrack_chams::Generate(self, scene, view, output, original)) return true;
        const auto surfaces=penetration_overlay::Get();
        if(surfaces.enabled&&surfaces.props){
            const auto byte=[](float v){return static_cast<std::uint8_t>(std::clamp(v,0.f,1.f)*255);};
            const color tint{byte(surfaces.color[0]),byte(surfaces.color[1]),byte(surfaces.color[2]),byte(surfaces.color[3])};
            if(penetrable_props::Generate(self,scene,view,output,original,tint))return true;
        }
        if(!anyEnabled&&!custom::weapon_fx::reactive_enabled()&&!gun_images::Material()&&!enhancements::Get().removeLegs)return false;
        const auto entity = ReadEntity(scene);
        if (!entity) return false;
        if(enhancements::HideLocalLegs(entity->owner))return true;
        const auto target=Classify(*entity);
        if((target==Target::ViewmodelWeapon||target==Target::Weapon)&&
           (entity->previewWeapon||custom::weapon_fx::local_weapon_model(entity->owner))) {
            if(const auto image=gun_images::Material();image&&detail::ReadOutput(output)) {
                return ApplyImage(self,scene,view,output,original,image,gun_images::Tint());
            }
        }
        auto config=GetConfiguration(target);
        if(target==Target::ViewmodelWeapon&&custom::weapon_fx::reactive_enabled()&&custom::weapon_fx::local_weapon_model(entity->owner)) {
            if(!config.enabled) {config.enabled=true;config.visible.enabled=true;config.visible.material=Material::Glow;config.hidden.enabled=false;}
            config.visible.reactive=true;
        }
        return Apply(config, self, scene, view, output, original);
    }
    bool ApplyImage(std::uintptr_t self, std::uintptr_t scene, std::uintptr_t view, std::uintptr_t output,
                    GenerateFn original, std::uintptr_t material, color tint)
    {
        return ApplyImageChecked(self,scene,view,output,original,material,tint,nullptr,nullptr);
    }
    bool ApplyImageChecked(std::uintptr_t self,std::uintptr_t scene,std::uintptr_t view,std::uintptr_t output,
                           GenerateFn original,std::uintptr_t material,color tint,bool(*accept)(void*),void* context)
    {
        if (!scene || !original || !material || !detail::ReadOutput(output)) return false;
        std::lock_guard lock(sceneMutex);
        if(accept&&!accept(context))return false;
        const SceneFlags flags(scene);
        const auto before = detail::ReadOutput(output);
        original(self,scene,view,output);
        // Native generation can replace a model or trigger map teardown. The
        // original was already emitted, so leave its material and consume the
        // call if the caller's selection changed; never emit it twice.
        if(accept&&!accept(context))return true;
        const auto after = detail::ReadOutput(output);
        if (before && after)
            for (int i = before->Count(); i < after->Count(); ++i)
                detail::Replace(after->At(i),scene,material,tint);
        return true;
    }
    bool Apply(const Configuration &config, std::uintptr_t self, std::uintptr_t scene, std::uintptr_t view,
               std::uintptr_t output, GenerateFn original)
    {
        if (!original || !scene || !config.enabled || (!config.visible.enabled && !config.hidden.enabled) ||
            !detail::ReadOutput(output)) return false;
        const auto hidden = config.hidden.enabled ? materials::Find(config.hidden.material, true) : 0;
        const auto visible = config.visible.enabled ? materials::Find(config.visible.material, false) : 0;
        if (!hidden && !visible) return false;
        std::lock_guard lock(sceneMutex);
        const SceneFlags flags(scene);
        const auto seconds=static_cast<double>(GetTickCount64())*.001;
        const auto emit = [&](std::uintptr_t material,const Layer& layer)
        {
            const auto before = detail::ReadOutput(output);
            original(self, scene, view, output);
            const auto after = detail::ReadOutput(output);
            if (!material || !before || !after || after->Count() <= before->Count()) return;
            for (int index = before->Count(); index < after->Count(); ++index) {
                auto tint=AnimatedTint(layer,seconds,static_cast<unsigned>(index-before->Count()));
                if(layer.reactive)tint=custom::weapon_fx::reactive_tint(tint);
                detail::Replace(after->At(index),scene,material,tint);
            }
        };
        // Spectral's secondary (ignore-Z) pass precedes the visible pass.
        if (hidden) emit(hidden, config.hidden);
        if (config.visible.enabled) emit(visible, config.visible);
        return true;
    }
    void Reset() { backtrack_chams::Reset(); backtrack::Reset(); materials::Reset(); }
    void Shutdown() { ready = false; backtrack_chams::Shutdown(); backtrack::Reset(); materials::Shutdown(); }
    void DrawMenu()
    {
        static int selected{};
        constexpr Target targets[]{Target::Enemy,Target::Teammate,Target::EnemyRagdoll,
            Target::TeamRagdoll,Target::ViewmodelWeapon,Target::Weapon};
        constexpr const char* names[]{"Enemies","Teammates","Enemy ragdolls",
            "Teammate ragdolls","Held weapon","World weapons"};
        constexpr int count=6;
        if(selected<0||selected>=count)selected=0;
        ImGui::Combo("Target",&selected,names,count);
        ImGui::Separator();
        ConfigurationControls(targets[selected]);
        ImGui::Separator();
        if(ImGui::CollapsingHeader("Backtrack chams")) backtrack_chams::DrawMenu();
        if(ImGui::CollapsingHeader("Body aura"))
            visuals::particle_chams::DrawMenu();
    }
    void DrawTargetMenu(Target target)
    {
        ConfigurationControls(target);
    }
}
