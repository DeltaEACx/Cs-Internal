#pragma once
#include <cstdint>
#include <string>
#include <string_view>
namespace visuals {struct frame_snapshot;}

namespace custom::combat {
// Bootstrap resolves bindings only. Listener registration, unregistration and
// movement sampling occur at frame stage 7 on the game thread.
bool initialize();
void on_frame_stage(int stage);
void on_game_event(std::string_view name,std::uintptr_t event);
void draw_tracers(const visuals::frame_snapshot& frame);
void on_level_shutdown();
void request_shutdown() noexcept;
bool shutdown_complete() noexcept;
void set_visibility_hook(bool installed) noexcept;
// A true result means the current confirmed-dead owner should submit no model
// primitives. Native entities, materials and visibility flags remain untouched.
bool hide_scene_object(std::uintptr_t scene_object);
std::string status();
unsigned confirmed_kills() noexcept;
}
