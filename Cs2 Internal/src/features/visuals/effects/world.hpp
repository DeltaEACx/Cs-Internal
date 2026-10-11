#pragma once

#include "config.hpp"
#include <array>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

namespace custom::world {
struct sky_lighting_profile {
    std::array<float,3> light_color;
    float light_intensity;
    std::array<float,3> fog_color;
};
struct skybox_entry {
    std::string_view label;
    std::string_view resource_path;
    bool reset_tint_on_select{};
    const sky_lighting_profile* lighting{};
};
struct alignas(16) shader_vector {
    std::array<float,4> values{};
};
using set_shader_fn=std::uintptr_t(__fastcall*)(void*,std::uint32_t,const void*);

bool initialize();
// Call from the render-start frame stage, before native scene submission.
void update(const configuration& value,bool in_world);
void reset();
// Call only after native hook callbacks have drained.
void shutdown();
std::span<const skybox_entry> skyboxes() noexcept;
bool sky_lighting_available(const configuration& value) noexcept;
bool sky_lighting_active(const configuration& value) noexcept;
// Resolve per draw, leaving the user's manual light and fog settings intact.
configuration effective_configuration(configuration value) noexcept;
bool skybox_ready() noexcept;
bool lighting_ready() noexcept;
bool shaders_ready() noexcept;
std::string status();
std::string active_sky_path();
std::uintptr_t texture_binding(std::string_view path);

// A callback owns its snapshot and holds the mutation lock until the original
// draw returns. Nested callbacks and simultaneous render jobs restore correctly.
class sky_draw_scope final {
public:
    sky_draw_scope(std::uintptr_t meshes,int count,const configuration& value);
    ~sky_draw_scope();
    sky_draw_scope(const sky_draw_scope&)=delete;
    sky_draw_scope& operator=(const sky_draw_scope&)=delete;
private:
    std::unique_lock<std::recursive_mutex> lock_;
    std::uintptr_t binding_{},material_{},descriptor_{};
    std::array<float,3> color_{};
    bool tinted_{};
};

class light_draw_scope final {
public:
    light_draw_scope(std::uintptr_t object,const configuration& value);
    ~light_draw_scope();
    light_draw_scope(const light_draw_scope&)=delete;
    light_draw_scope& operator=(const light_draw_scope&)=delete;
private:
    std::unique_lock<std::recursive_mutex> lock_;
    std::uintptr_t object_{};
    std::array<float,3> color_{};
};

// Storage belongs to the current hook invocation; the native setter copies it.
bool shader_override(std::uint32_t hash,const configuration& value,float elapsed,shader_vector& storage) noexcept;
// Apply after native fog setup so it retains responsibility for map cleanup.
bool apply_fog(void* output,int* mode,set_shader_fn original_setter,const configuration& value);
}
