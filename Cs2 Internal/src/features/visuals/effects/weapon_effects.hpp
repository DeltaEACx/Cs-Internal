#pragma once
#include "features/visuals/engine.hpp"
#include "features/visuals/settings.hpp"
#include "particles.hpp"

namespace custom::weapon_fx {
enum class action {shot,reload,inspect,hit,kill};
void initialize();
void capture_scene(std::uintptr_t scene);
void update();
void reset();
void notify(action event);
void draw_menu();
bool attachment_position(particles::attachment anchor,std::uint32_t pawn,std::uint32_t weapon,math::vector3& point);
bool reactive_enabled() noexcept;
bool local_weapon_model(std::uintptr_t entity);
visuals::color reactive_tint(visuals::color base);
std::span<const particles::preset> styles() noexcept;
}
