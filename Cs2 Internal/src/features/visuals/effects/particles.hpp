#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace custom::particles {
struct preset {
    const char* label;
    const char* resource;
    bool color_control;
    bool bursts;
};
enum class effect_kind {kill,impact,hit,headshot,spawn,jump,landing,speed_trail,reload,victory,gun,barrel,hands,inspect,gun_reload,muzzle,gun_smoke,body_aura};
enum class attachment {none,gun,barrel,left_hand,right_hand,body};
enum class burst_state {pending,spawned,failed,expired};
struct burst_request {
    std::string resource;
    std::array<float,3> origin{};
    float lifetime{2.5f};
    effect_kind kind{effect_kind::kill};
    attachment anchor{attachment::none};
    std::uint32_t pawn_handle{},weapon_handle{};
    unsigned body_anchor{};
};
struct kill_recipe {
    const char* label;
    std::array<const char*,3> resources{};
    unsigned layers{};
};
// These enqueue copied data only. A nonzero ticket means accepted, while a
// spawned receipt means every native layer was created and positioned.
std::uint64_t queue_burst(burst_request request);
std::uint64_t queue_kill_effect(int recipe,const std::array<float,3>& origin,float lifetime=2.5f);
std::uint64_t queue_feature_effect(effect_kind kind,const std::array<float,3>& origin,float lifetime=2.5f);
burst_state kill_effect_state(std::uint64_t ticket) noexcept;
bool kill_effect_spawned(std::uint64_t ticket) noexcept;
bool burst_started(std::uint64_t ticket) noexcept;
std::span<const kill_recipe> kill_recipes() noexcept;
void cancel_burst(std::uint64_t ticket);

// Initialize on the bootstrap thread. All native effect mutations happen at
// stage 7, or before the native level-shutdown callback on the game thread.
bool initialize();
void on_frame_stage(int stage);
void on_level_shutdown();
void request_shutdown() noexcept;
bool shutdown_complete() noexcept;
std::string status();
unsigned active_count() noexcept;
bool available() noexcept;
std::span<const preset> presets() noexcept;
}
