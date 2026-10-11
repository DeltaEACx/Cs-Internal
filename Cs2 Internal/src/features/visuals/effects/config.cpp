#include "core/user_defaults.hpp"
#include "features/visuals/engine.hpp"
#include "config.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace {
std::mutex mutex;
// Keep configuration{} neutral so callers can still reset every effect.
constexpr custom::configuration night_defaults() {
    custom::configuration result{};
    result.enabled=true;
    result.skybox=true;
    result.skybox_index=12; // Rain Night, from the installed sky presets.
    result.tint_sky=true;
    result.sky_color={0.58f,0.24f,1.0f};
    result.sky_intensity=0.75f;
    result.lighting=true;
    result.lighting_color={0.32f,0.84f,1.0f};
    result.lighting_intensity=1.05f;
    result.bloom=true;
    result.bloom_strength=0.20f;
    result.fog=true;
    result.fog_color={0.10f,0.065f,0.18f};
    result.fog_density=0.045f;
    result.fog_distance=6800.0f;
    result.fog_anisotropy=0.03f;
    return result;
}
constexpr auto defaults=user_defaults::World(night_defaults());
custom::configuration value=defaults;
std::atomic<bool> active{defaults.enabled};
float finite_clamp(float v,float low,float high,float fallback) {
    return std::isfinite(v) ? std::clamp(v,low,high) : fallback;
}
void color(std::array<float,3>& v) {for(auto& channel:v) channel=finite_clamp(channel,0,1,1);}
}
namespace custom {
configuration get_configuration(){std::scoped_lock lock(mutex);return value;}
void set_configuration(configuration next) {
    next.skybox_index=std::clamp(next.skybox_index,0,64);
    next.sky_texture.back()=0;next.particle_resource.back()=0;
    color(next.sky_color);color(next.lighting_color);color(next.fog_color);color(next.particle_color);
    for(auto& channel:next.tracer_color)channel=finite_clamp(channel,0,1,1);
    next.tracer_width=finite_clamp(next.tracer_width,.5f,6,2);
    next.tracer_lifetime=finite_clamp(next.tracer_lifetime,.1f,5,1.2f);
    next.tracer_style=std::clamp(next.tracer_style,0,2);
    next.sky_intensity=finite_clamp(next.sky_intensity,0,5,defaults.sky_intensity);
    next.lighting_intensity=finite_clamp(next.lighting_intensity,0,8,defaults.lighting_intensity);
    next.rainbow_speed=finite_clamp(next.rainbow_speed,0.01f,3,0.2f);
    next.bloom_strength=finite_clamp(next.bloom_strength,0,3,defaults.bloom_strength);
    for(unsigned i=0;i<4;++i)next.focusDistances[i]=std::max(i?next.focusDistances[i-1]:0.f,finite_clamp(next.focusDistances[i],0,10000,i?2000.f:0.f));
    next.gamma_value=finite_clamp(next.gamma_value,0.2f,3,1);
    next.fog_density=finite_clamp(next.fog_density,0,1,defaults.fog_density);
    next.fog_distance=finite_clamp(next.fog_distance,100,10000,defaults.fog_distance);
    next.fog_anisotropy=finite_clamp(next.fog_anisotropy,0,1,defaults.fog_anisotropy);
    next.wind_direction=finite_clamp(next.wind_direction,0,360,90);
    next.wind_strength=finite_clamp(next.wind_strength,0,5,1);
    next.wind_frequency=finite_clamp(next.wind_frequency,0.1f,5,1);
    next.wetness_amount=finite_clamp(next.wetness_amount,0,2,1);
    next.wetness_speed=finite_clamp(next.wetness_speed,0,5,1);
    next.particle_preset=std::clamp(next.particle_preset,0,64);
    next.particle_intensity=finite_clamp(next.particle_intensity,0.1f,5,1);
    next.particle_radius=finite_clamp(next.particle_radius,50,2000,600);
    next.particle_height=finite_clamp(next.particle_height,0,1200,300);
    next.particle_scale=finite_clamp(next.particle_scale,0.1f,5,1);
    next.particle_count=std::clamp(next.particle_count,1,8);
    next.kill_recipe=std::clamp(next.kill_recipe,0,64);
    next.kill_lifetime=finite_clamp(next.kill_lifetime,0.4f,8,2.5f);
    next.multikill_window=finite_clamp(next.multikill_window,1,10,4);
    next.trail_min_speed=finite_clamp(next.trail_min_speed,50,350,180);
    next.gun_style=std::clamp(next.gun_style,0,5);
    next.barrel_style=std::clamp(next.barrel_style,0,5);
    next.hand_style=std::clamp(next.hand_style,0,5);
    next.gun_interval=finite_clamp(next.gun_interval,0.25f,3.0f,0.7f);
    for(auto& channel:next.gun_offset) channel=finite_clamp(channel,-64,64,0);
    for(unsigned i=0;i<3;++i) next.barrel_offset[i]=finite_clamp(next.barrel_offset[i],-64,64,i==0?18.0f:0.0f);
    next.left_hand_bone=std::clamp(next.left_hand_bone,0,255);
    next.right_hand_bone=std::clamp(next.right_hand_bone,0,255);
    next.impact_style=std::clamp(next.impact_style,0,2);
    std::scoped_lock lock(mutex);value=next;active.store(next.enabled);
}
bool enabled() noexcept{return active.load(std::memory_order_relaxed);}
}
