#pragma once

#include "frame_data.hpp"
#include "settings.hpp"

namespace visuals
{
	// Call once from the active ImGui frame after a game-state adapter has built
	// a snapshot. The renderer does not read process memory or game offsets.
	void render(const frame_snapshot& frame);
	enum class menu_section {players, items, hud};
	void draw_menu(menu_section section=menu_section::players);
	void draw_enemy_models_menu();
}
