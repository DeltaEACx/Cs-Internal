#pragma once
#include "engine.hpp"
#include "settings.hpp"

namespace visuals::chams
{
    enum class Material : std::uint8_t
    {
        Liquid, Metallic, Matte, Flat, Bloom, Outlines, Glow, Electric, Distortion, Hologram, Pearl, Count
    };
    enum class Target : std::uint8_t
    {
        Enemy, Teammate, Local, EnemyRagdoll, TeamRagdoll, LocalRagdoll, Arms, ViewmodelWeapon, Weapon, Count, Unknown
    };
    enum class Animation : std::uint8_t {Static,Rainbow,Breathing,Gradient,Count};
    struct Layer
    {
        bool enabled{true};
        Material material{Material::Flat};
        color tint{80, 230, 130, 255};
        Animation animation{Animation::Static};
        float speed{1.0f};
        color secondary{165,80,255,255};
        bool reactive{};
    };
    struct Configuration
    {
        bool enabled{};
        Layer visible{};
        Layer hidden{true, Material::Flat, {245, 95, 105, 255}};
    };
    struct EntityInfo
    {
        std::uintptr_t owner{}, viewPawn{};
        std::uint32_t classHash{};
        int health{}, team{}, localTeam{};
        bool localAttachment{};
        bool previewWeapon{};
    };
    using GenerateFn = void(__fastcall *)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);
    Configuration GetConfiguration(Target target);
    void SetConfiguration(Target target, Configuration configuration);
    Target Classify(const EntityInfo &entity);
    bool IsWeaponPreview(std::string_view className, std::string_view modelPath);
    bool IsPreviewPlayer(std::string_view className);
    color AnimatedTint(const Layer &layer,double seconds,unsigned mesh=0);
    std::uintptr_t ResolveSceneOwner(std::uintptr_t list, std::uint32_t sceneHandle);
    bool Initialize();
    void Update(bool inWorld);
    void Reset();
    void Shutdown();
    bool Generate(std::uintptr_t self, std::uintptr_t scene, std::uintptr_t view,
                  std::uintptr_t output, GenerateFn original);
    bool ApplyImage(std::uintptr_t self, std::uintptr_t scene, std::uintptr_t view, std::uintptr_t output,
                    GenerateFn original, std::uintptr_t material, color tint);
    bool ApplyImageChecked(std::uintptr_t self,std::uintptr_t scene,std::uintptr_t view,std::uintptr_t output,
                           GenerateFn original,std::uintptr_t material,color tint,bool(*accept)(void*),void* context);
    bool Apply(const Configuration &configuration, std::uintptr_t self, std::uintptr_t scene,
               std::uintptr_t view, std::uintptr_t output, GenerateFn original);
    void DrawMenu();
    void DrawTargetMenu(Target target);
}
