#include "engine.hpp"
#include "visuals.h"
#include "renderer.hpp"
#include "chams.h"
#include "backtrack_chams.h"
#include "features/combat/backtrack.h"
#include "enhancements.hpp"
#include "gun_images.h"
#include "autowall_crosshair.h"
#include "penetration_overlay.h"
#include "features/combat/detail/hvh_input.hpp"
#include "features/view/view_settings.h"
#include "game/sdk/VisualBindings.h"
#include "effects/config.hpp"
#include "effects/world.hpp"
#include "effects/particles.hpp"
#include "effects/combat.hpp"
#include "effects/weapon_effects.hpp"
#include "minhook/include/MinHook.h"
#include <emmintrin.h>

namespace visual_features
{
    namespace
    {
        using ScopeFn=void(__fastcall*)(std::uintptr_t,std::uintptr_t);
        ScopeFn originalScope{};
        using StageFn = void(__fastcall *)(void *, int);
        using ShutdownFn = std::uintptr_t(__fastcall *)(std::uintptr_t);
        using SkyFn = void(__fastcall *)(std::uintptr_t, std::uintptr_t, std::uintptr_t, int, int,
                                         std::uintptr_t, std::uintptr_t, std::uintptr_t);
        using LightFn = std::uintptr_t(__fastcall *)(std::uintptr_t, std::uintptr_t, std::uintptr_t);
        using ShaderFn = std::uintptr_t(__fastcall *)(__m128i *, std::uint32_t, __m128i *);
        using FogFn = std::uintptr_t(__fastcall *)(__m128i *, int *);
        using PrimitivesFn = void(__fastcall *)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);
        using HitFn = std::uintptr_t(__fastcall *)(std::uintptr_t, std::uint8_t);
        StageFn originalStage{};
        ShutdownFn originalShutdown{};
        SkyFn originalSky{};
        LightFn originalLight{};
        ShaderFn originalShader{}, originalPostprocess{};
        FogFn originalFog{};
        PrimitivesFn originalPrimitives{};
        HitFn originalHit{};
        std::vector<void *> hooks;
        std::atomic<bool> running{false}, stopping{true}, stagesReady{false};
        std::atomic<unsigned> callbacks{0};
        struct Scope
        {
            Scope() { ++callbacks; }
            ~Scope() { --callbacks; }
        };
        void SetStatus(std::string value)
        {
            const auto line = "[Cs2 Internal] " + value + "\n";
            OutputDebugStringA(line.c_str());
        }
        void __fastcall FrameStage(void *self, int stage)
        {
            BeforeStage(stage);
            originalStage(self, stage);
            AfterStage(stage);
        }
        std::uintptr_t __fastcall LevelShutdown(std::uintptr_t self)
        {
            Scope scope;
            view_settings::OnLevelShutdown();
            hvh_input::LevelChanged();
            custom::combat::on_level_shutdown();
            custom::weapon_fx::reset();
            custom::particles::on_level_shutdown();
            custom::world::reset();
            visuals::autowall_crosshair::Reset();
            visuals::penetration_overlay::Reset();
            visuals::enhancements::Reset();
            visuals::gun_images::Reset();
            visuals::chams::Reset();
            game::reset();
            return originalShutdown(self);
        }
        void __fastcall DrawScope(std::uintptr_t self,std::uintptr_t output){
            Scope scope;originalScope(self,output);
            const auto settings=visuals::enhancements::Get();
            if(!stopping&&(settings.removeScope||settings.scope))memory::safe_write(output+4,std::uint8_t{});
        }
        void __fastcall DrawSky(std::uintptr_t self, std::uintptr_t a2, std::uintptr_t meshes, int count,
                                int a5, std::uintptr_t a6, std::uintptr_t a7, std::uintptr_t a8)
        {
            Scope scope;
            if (!stopping && custom::enabled())
            {
                const custom::world::sky_draw_scope changes(meshes, count, custom::get_configuration());
                originalSky(self, a2, meshes, count, a5, a6, a7, a8);
                return;
            }
            originalSky(self, a2, meshes, count, a5, a6, a7, a8);
        }
        std::uintptr_t __fastcall LightScene(std::uintptr_t self, std::uintptr_t light, std::uintptr_t a3)
        {
            Scope scope;
            if (!stopping)
            {
                const custom::world::light_draw_scope changes(light, custom::get_configuration());
                if(const auto own=visuals::enhancements::LightColor(light)){
                    const auto before=memory::safe_read<math::vector3>(light+0xe4);
                    if(before&&memory::safe_write(light+0xe4,*own)){
                        const auto result=originalLight(self,light,a3);memory::safe_write(light+0xe4,*before);return result;
                    }
                }
                return originalLight(self, light, a3);
            }
            return originalLight(self, light, a3);
        }
        std::uintptr_t ApplyShader(ShaderFn original, __m128i *map, std::uint32_t hash, __m128i *value)
        {
            custom::world::shader_vector replacement;
            if (!stopping && custom::enabled() && custom::world::shaders_ready() &&
                custom::world::shader_override(hash, custom::get_configuration(),
                                               static_cast<float>(GetTickCount64() * .001), replacement))
                value = reinterpret_cast<__m128i *>(replacement.values.data());
            return original(map, hash, value);
        }
        std::uintptr_t __fastcall Shader(__m128i *map, std::uint32_t hash, __m128i *value)
        {
            Scope scope;
            return ApplyShader(originalShader, map, hash, value);
        }
        std::uintptr_t __fastcall Postprocess(__m128i *map, std::uint32_t hash, __m128i *value)
        {
            Scope scope;
            return ApplyShader(originalPostprocess, map, hash, value);
        }
        std::uintptr_t __fastcall Fog(__m128i *output, int *mode)
        {
            Scope scope;
            const auto result = originalFog(output, mode);
            if (!stopping && custom::enabled() && originalShader)
                custom::world::apply_fog(output, mode,
                                         reinterpret_cast<custom::world::set_shader_fn>(originalShader),
                                         custom::get_configuration());
            return result;
        }
        void __fastcall GeneratePrimitives(std::uintptr_t self, std::uintptr_t scene,
                                           std::uintptr_t view, std::uintptr_t buffer)
        {
            Scope scope;
            if(!stopping)custom::weapon_fx::capture_scene(scene);
            if (!stopping && custom::enabled() && custom::combat::hide_scene_object(scene))
                return;
            if (!stopping && visuals::chams::Generate(self, scene, view, buffer, originalPrimitives))
                return;
            originalPrimitives(self, scene, view, buffer);
        }
        std::uintptr_t __fastcall ReportHit(std::uintptr_t message, std::uint8_t deleting)
        {
            Scope scope;
            if (!stopping)
                game::report_hit(message);
            return originalHit(message, deleting);
        }
        template <class T> bool AddHook(std::uintptr_t address, void *detour, T &original)
        {
            if (!address)
                return false;
            auto *target = reinterpret_cast<void *>(address);
            if (std::find(hooks.begin(), hooks.end(), target) != hooks.end())
                return true; // Retain and reuse disabled trampolines across restarts.
            if (MH_CreateHook(target, detour, reinterpret_cast<void **>(&original)) != MH_OK)
                return false;
            hooks.push_back(target);
            return true;
        }
    }

    void Install(bool sharedFrameHook)
    {
        running = false;
        stopping = false;
        stagesReady = false;
        if (!game::initialize())
        {
            SetStatus(game::status());
            return;
        }
        const bool stage = sharedFrameHook ||
            AddHook(PATTERN(patterns::frame_stage_notify), reinterpret_cast<void *>(FrameStage), originalStage);
        const bool shutdown = AddHook(PATTERN(patterns::level_shutdown),
                                      reinterpret_cast<void *>(LevelShutdown), originalShutdown);
        bool sky = false, light = false, shader = false, post = false, fog = false, particles = false, events = false;
        if (stage && shutdown)
        {
            custom::world::initialize();
            const bool chams = visuals::chams::Initialize();
            custom::weapon_fx::initialize();
            visuals::enhancements::Initialize();
            visuals::autowall_crosshair::Initialize();
            visuals::penetration_overlay::Initialize();
            const auto base=game_access::verifiedClient.load();std::array<std::uint8_t,24> scopeEntry{};
            if(memory::copy(scopeEntry.data(),base+visual_bindings::scope.rva,scopeEntry.size())&&scopeEntry==visual_bindings::scope.entry)
                AddHook(base+visual_bindings::scope.rva,reinterpret_cast<void*>(DrawScope),originalScope);
            particles = custom::particles::initialize();
            events = custom::combat::initialize();
            const bool primitives = (chams || (events && custom::world::lighting_ready())) &&
                AddHook(PATTERN(patterns::generate_primitives), reinterpret_cast<void *>(GeneratePrimitives), originalPrimitives);
            if (events) custom::combat::set_visibility_hook(primitives && custom::world::lighting_ready());
            if (custom::world::lighting_ready())
            {
                sky = AddHook(PATTERN(patterns::draw_skybox_array), reinterpret_cast<void *>(DrawSky), originalSky);
                light = AddHook(PATTERN(patterns::light_scene_object), reinterpret_cast<void *>(LightScene), originalLight);
            }
            if (custom::world::shaders_ready())
            {
                shader = AddHook(PATTERN(patterns::set_shader_param), reinterpret_cast<void *>(Shader), originalShader);
                post = AddHook(PATTERN(patterns::set_postprocess_vec), reinterpret_cast<void *>(Postprocess), originalPostprocess);
                fog = shader && AddHook(PATTERN(patterns::setup_fog), reinterpret_cast<void *>(Fog), originalFog);
            }
            stagesReady = true;
        }
        const bool hit = AddHook(PATTERN(patterns::parse_report_hit), reinterpret_cast<void *>(ReportHit), originalHit);
        bool enabled = true;
        for (auto *target : hooks)
        {
            enabled = (MH_QueueEnableHook(target) == MH_OK) && enabled;
        }
        // Suspend and resume process threads once for the complete visual group.
        // Individual enables repeat that work for every hook during WebView startup.
        if (enabled && !hooks.empty()) enabled = MH_ApplyQueued() == MH_OK;
        if (!enabled)
        {
            for (auto *target : hooks) MH_QueueDisableHook(target);
            stopping = true;
            visuals::enhancements::RequestStop();
            visuals::autowall_crosshair::RequestStop();
            visuals::penetration_overlay::RequestStop();
            custom::combat::request_shutdown();
            custom::particles::request_shutdown();
            SetStatus("Could not enable all visual hooks; stop and restart the overlay.");
            return;
        }
        SetStatus(std::string("ESP ready; ") +
                  (stage && shutdown ? "world/effects callbacks connected." : "world/effects callbacks unavailable.") +
                  " Sky " + (sky ? "ready" : "unavailable") + "; lighting " + (light ? "ready" : "unavailable") +
                  "; shaders " + (shader && post ? "ready" : "unavailable") + "; fog " + (fog ? "ready" : "unavailable") +
                  "; particles " + (particles ? "ready" : "unavailable") + "; events " + (events ? "ready" : "unavailable") +
                  "; hit marker " + (hit ? "ready" : "unavailable") + ".");
        running = true;
    }

    void BeforeStage(int stage)
    {
        Scope scope;
        if (!stagesReady)
            return;
        // FRAME_START still runs while the main menu has no local pawn.
        if (stage == skin_bindings::kAgentStage && !stopping && !game::in_world())
            visuals::gun_images::Update(false);
        if (stage != 7)
            return;
        if (!stopping)
        {
            const bool inWorld = game::in_world();
            custom::world::update(custom::get_configuration(), inWorld);
            visuals::gun_images::Update(inWorld);
            custom::weapon_fx::update();
            visuals::chams::Update(inWorld);
        }
        visuals::enhancements::Frame();
        custom::combat::on_frame_stage(stage);
        custom::particles::on_frame_stage(stage);
    }
    void AfterStage(int stage)
    {
        Scope scope;
        if(stagesReady && stage==7) {
            backtrack::Frame(!stopping && game::in_world());
            visuals::backtrack_chams::Frame(!stopping && game::in_world());
            visuals::autowall_crosshair::Frame(!stopping && game::in_world());
            visuals::penetration_overlay::Frame(!stopping && game::in_world());
        }
    }
    void Render()
    {
        Scope scope;
        if (!running || stopping)
            return;
        const auto size = ImGui::GetIO().DisplaySize;
        const auto frame=game::capture(size.x,size.y);
        custom::combat::draw_tracers(frame);
        visuals::penetration_overlay::Render(frame);
        if(visuals::g_settings.enabled){visuals::render(frame);visuals::enhancements::Render(frame);}
        visuals::autowall_crosshair::Render(frame);
    }
    void DrawMenu(Page page)
    {
        switch(page)
        {
        case Page::Players: visuals::draw_menu(visuals::menu_section::players); break;
        case Page::Items: visuals::draw_menu(visuals::menu_section::items); break;
        case Page::Hud: visuals::draw_menu(visuals::menu_section::hud); ImGui::Separator();visuals::enhancements::Menu(visuals::enhancements::Page::Widgets);break;
        case Page::Chams: visuals::chams::DrawMenu(); break;
        case Page::WeaponParticles: custom::weapon_fx::draw_menu(); break;
        case Page::EventEffects: custom::draw_effects_menu(); break;
        case Page::Skybox: custom::draw_menu(custom::world_page::skybox); break;
        case Page::Lighting: custom::draw_menu(custom::world_page::lighting); break;
        case Page::Weather: custom::draw_menu(custom::world_page::weather); break;
        case Page::WorldParticles: custom::draw_menu(custom::world_page::particles); break;
        case Page::Crosshair:
            visuals::autowall_crosshair::DrawMenu();
            visuals::enhancements::Menu(visuals::enhancements::Page::Hud); break;
        case Page::Movement: visuals::enhancements::Menu(visuals::enhancements::Page::Movement); break;
        case Page::Glow: visuals::enhancements::Menu(visuals::enhancements::Page::Models); break;
        case Page::Removals: visuals::enhancements::Menu(visuals::enhancements::Page::Removals); break;
        case Page::GrenadeEffects: visuals::enhancements::Menu(visuals::enhancements::Page::Grenades); break;
        case Page::GunImages: visuals::gun_images::Menu(); break;
        case Page::EnemyModels: visuals::draw_enemy_models_menu(); break;
        case Page::PenetrableSurfaces: visuals::penetration_overlay::DrawMenu(); break;
        case Page::Local:
            visuals::enhancements::Menu(visuals::enhancements::Page::Local);
            ImGui::Separator();
            if(ImGui::CollapsingHeader("Local player chams",ImGuiTreeNodeFlags_DefaultOpen)) visuals::chams::DrawTargetMenu(visuals::chams::Target::Local);
            if(ImGui::CollapsingHeader("Local ragdoll chams",ImGuiTreeNodeFlags_DefaultOpen)) visuals::chams::DrawTargetMenu(visuals::chams::Target::LocalRagdoll);
            if(ImGui::CollapsingHeader("Arm chams",ImGuiTreeNodeFlags_DefaultOpen)) visuals::chams::DrawTargetMenu(visuals::chams::Target::Arms);
            break;
        }
    }
    bool Stop()
    {
        stopping = true;
        running = false;
        visuals::enhancements::RequestStop();
        visuals::autowall_crosshair::RequestStop();
        visuals::penetration_overlay::RequestStop();
        custom::combat::request_shutdown();
        custom::particles::request_shutdown();
        const auto deadline = GetTickCount64() + 2000;
        while ((!custom::combat::shutdown_complete() || !custom::particles::shutdown_complete() || !visuals::enhancements::Stopped() || !visuals::autowall_crosshair::Stopped() || !visuals::penetration_overlay::Stopped()) &&
               GetTickCount64() < deadline)
            Sleep(1);
        if (!custom::combat::shutdown_complete() || !custom::particles::shutdown_complete() || !visuals::enhancements::Stopped() || !visuals::autowall_crosshair::Stopped() || !visuals::penetration_overlay::Stopped())
        {
            SetStatus("Waiting for game-thread effect cleanup; keep the DLL loaded.");
            return false;
        }
        for (auto *target : hooks)
        {
            const auto result = MH_DisableHook(target);
            if (result != MH_OK && result != MH_ERROR_DISABLED)
                return false;
        }
        stagesReady = false;
        while (callbacks)
            Sleep(1);
        custom::world::shutdown();
        visuals::chams::Shutdown();
        custom::weapon_fx::reset();
        game::reset();
        SetStatus("Visuals stopped");
        return true;
    }
}
