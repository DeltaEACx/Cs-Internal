#pragma once

#include "imgui/imgui.h"
#include "../frame_data.hpp"
#include "../settings.hpp"

namespace visuals::detail
{
	inline ImU32 to_u32(const color& value)
	{
		return IM_COL32(value.r, value.g, value.b, value.a);
	}

	inline ImVec2 point(const screen_point& value)
	{
		return ImVec2(value.x, value.y);
	}

	inline ImU32 health_color(int health)
	{
		const auto clamped = health < 0 ? 0 : (health > 100 ? 100 : health);
		return IM_COL32(255 - clamped * 2, clamped * 2 + 35, 55, 255);
	}

	void draw_players(ImDrawList* draw, const frame_snapshot& frame);
	void draw_world(ImDrawList* draw, const frame_snapshot& frame);
}
