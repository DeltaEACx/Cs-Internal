#pragma once
#include "core/user_defaults.hpp"

#include <cstdint>

namespace visuals
{
	struct color
	{
		std::uint8_t r{}, g{}, b{}, a{ 255 };
	};

	enum class box_style : std::uint8_t
	{
		full,
		corners,
		rounded
	};
	enum class bar_style : std::uint8_t { solid, gradient, segmented };
	enum class text_style : std::uint8_t { plain, shadow, outline };
	enum class skeleton_style : std::uint8_t { lines, joints };
	enum class marker_style : std::uint8_t { circle, dot, square, diamond, cross };
	enum class line_style : std::uint8_t { solid, dashed };
	enum class arrow_style : std::uint8_t { triangle, chevron, dot };
	enum class flag_style : std::uint8_t { plain, badges };
	enum class hit_style : std::uint8_t { cross, plus, dot };

	struct settings
	{
		bool enabled{ true };
		bool show_teammates{};
		bool show_dormant{ true };
		bool show_offscreen{ true };
		bool player_box{ true };
		bool box_fill{};
		bool box_outline{ true };
		bool player_name{ true };
		bool health_bar{ true };
		bool armor_bar{ true };
        bool ammo_bar{};
		bool health_value{}, armor_value{}, ammo_value{};
		bool weapon_name{ true };
		bool distance{ true };
		bool skeleton{};
		bool head_marker{ true };
		bool player_flags{ true };
		bool snapline{};
		bool item_names{ true };
		bool item_distance{ true };
		bool dropped_weapons{ true };
		bool dropped_grenades{ true };
		bool dropped_objectives{ true };
		bool other_entities{};
		bool objective_timer{ true };
		bool projectile_names{ true };
		bool projectile_timer{ true };
		bool projectile_path{ true };
		bool fov_guide{};
		bool speed_widget{};
		bool bomb_widget{ true };
		bool hit_marker{ true };
		box_style boxes{ box_style::corners };
		bar_style health_style{ bar_style::solid }, armor_style{ bar_style::solid }, ammo_style{ bar_style::solid };
		text_style name_style{ text_style::plain }, weapon_style{ text_style::plain }, distance_style{ text_style::plain };
		skeleton_style skeleton_render{ skeleton_style::lines };
		marker_style head_style{ marker_style::circle };
		line_style snapline_style{ line_style::solid };
		arrow_style offscreen_style{ arrow_style::triangle };
		flag_style flags_style{ flag_style::plain };
		text_style item_style{ text_style::plain }, projectile_text_style{ text_style::plain }, speed_text_style{ text_style::plain };
		line_style trajectory_style{ line_style::solid }, fov_style{ line_style::solid };
		marker_style projectile_marker{ marker_style::dot };
		hit_style hit_marker_style{ hit_style::cross };
		float box_thickness{ 1.5f };
		float box_rounding{ 6.0f }, health_width{ 3.0f }, armor_width{ 3.0f }, ammo_width{ 3.0f };
		float skeleton_thickness{ 1.0f }, head_size{ 5.0f }, head_thickness{ 1.25f }, snapline_thickness{ 1.0f };
		float offscreen_size{ 14.0f }, offscreen_radius{ 0.39f };
		float trajectory_thickness{ 1.5f }, projectile_size{ 3.0f }, fov_thickness{ 1.0f };
		float hit_marker_size{ 8.0f }, hit_marker_thickness{ 1.5f };
		float text_size{ 13.0f };
		float corner_fraction{ 0.28f };
		float max_distance_meters{ 250.0f };
		float snapline_origin{ 0.5f };
		float guide_degrees{ 5.0f };
		color enemy_visible{ 50, 210, 235, 255 };
		color enemy_hidden{ 155, 85, 235, 255 };
		color teammate{ 95, 165, 255, 255 };
		color dormant{ 150, 150, 160, 220 };
		color box_fill_color{ 15, 18, 24, 55 };
		color skeleton_color{ 235, 235, 245, 230 };
		color name_color{ 245, 245, 250, 255 }, weapon_color{ 245, 245, 250, 255 }, distance_color{ 205, 210, 220, 255 };
		color armor_color{ 80, 155, 255, 255 }, ammo_color{ 110, 170, 255, 255 }, flag_color{ 245, 245, 250, 255 };
		color guide_color{ 230, 230, 240, 105 }, speed_color{ 235, 235, 245, 245 }, hit_color{ 255, 255, 255, 255 };
		color item_color{ 185, 150, 245, 255 };
		color projectile_color{ 100, 220, 245, 255 };
	};

	inline settings g_settings=user_defaults::Visuals(settings{});
}
