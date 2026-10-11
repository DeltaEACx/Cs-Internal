#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace visuals
{
	struct screen_point
	{
		float x{};
		float y{};
		bool valid{};
		bool on_screen{};
	};

	struct screen_rect
	{
		float left{};
		float top{};
		float right{};
		float bottom{};
		bool valid{};
	};

	struct bone_segment
	{
		screen_point from{};
		screen_point to{};
	};

	struct player_snapshot
	{
		std::string name{};
		std::string weapon{};
		std::vector<std::string> flags{};
		std::vector<bone_segment> skeleton{};
		screen_rect bounds{};
		screen_point head{};
		screen_point feet{};
		screen_point offscreen_direction{};
		float distance_meters{};
		int health{};
		int armor{};
        int ammo{},max_ammo{};
		int team{};
		bool alive{};
		bool enemy{};
		bool visible{};
		bool dormant{};
	};

	enum class world_entity_kind : std::uint8_t
	{
		weapon,
		grenade,
		objective,
		other
	};

	struct world_entity_snapshot
	{
		std::string label{};
		std::string detail{};
		screen_point position{};
		world_entity_kind kind{ world_entity_kind::other };
		float distance_meters{};
		float timer_seconds{};
		bool has_timer{};
	};

	struct projectile_snapshot
	{
		std::string label{};
		std::vector<screen_point> trajectory{};
		screen_point position{};
		float time_remaining{};
		bool has_timer{};
	};

	struct frame_snapshot
	{
		std::vector<player_snapshot> players{};
		std::vector<world_entity_snapshot> entities{};
		std::vector<projectile_snapshot> projectiles{};
		float screen_width{};
		float screen_height{};
		float fov_radius{};
        float horizontal_fov{90};
		float local_speed{};
		float bomb_time_remaining{};
		int local_team{};
		bool show_fov_guide{};
		bool show_hit_marker{};
		float hit_marker_time{};
		bool bomb_planted{};
	};
}
