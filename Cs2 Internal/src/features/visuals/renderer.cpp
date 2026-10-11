#include "features/visuals/engine.hpp"

#include "renderer.hpp"
#include "detail/draw.hpp"

#include "imgui/imgui.h"

namespace visuals
{
	void render(const frame_snapshot& frame)
	{
		if (!g_settings.enabled)
			return;

		auto* draw = ImGui::GetBackgroundDrawList();
		if (!draw)
			return;

		detail::draw_players(draw, frame);
		detail::draw_world(draw, frame);
	}
}
