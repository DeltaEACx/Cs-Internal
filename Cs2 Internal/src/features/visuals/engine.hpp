#pragma once
#include "platform/windows.h"
#include "game/game_access.h"
#include "game/sdk/SkinBindings.h"
#include "imgui/imgui.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <numbers>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
// Engine support retained only for the imported visual features.

namespace math {

	class vector2
	{
	public:
		float x, y;

		vector2( ) noexcept : x( 0.0f ), y( 0.0f ) {}
		constexpr vector2( float x, float y ) noexcept : x( x ), y( y ) {}

		[[nodiscard]] vector2 operator+( const vector2& v ) const noexcept;
		[[nodiscard]] vector2 operator-( const vector2& v ) const noexcept;
		[[nodiscard]] vector2 operator*( float scalar ) const noexcept;
		[[nodiscard]] vector2 operator/( float scalar ) const noexcept;
		[[nodiscard]] vector2 operator-( ) const noexcept;

		vector2& operator*=( float scalar ) noexcept;
		vector2& operator/=( float scalar ) noexcept;
		vector2& operator+=( const vector2& v ) noexcept;
		vector2& operator-=( const vector2& v ) noexcept;

		[[nodiscard]] bool operator==( const vector2& v ) const noexcept;
		[[nodiscard]] bool operator!=( const vector2& v ) const noexcept;

		[[nodiscard]] float dot( const vector2& v ) const noexcept;
		[[nodiscard]] float length_sqr( ) const noexcept;
		[[nodiscard]] float length( ) const noexcept;

		vector2& normalize( ) noexcept;
		[[nodiscard]] vector2 normalized( ) const noexcept;
	};

	class vector4 {
	public:
		float x, y, z, w;
	};

	class vector3
	{
	public:
		float x, y, z;

		vector3( ) noexcept : x( 0.0f ), y( 0.0f ), z( 0.0f ) {}
		constexpr vector3( float x, float y, float z ) noexcept : x( x ), y( y ), z( z ) {}

		[[nodiscard]] vector3 operator+( const vector3& v ) const noexcept;
		[[nodiscard]] vector3 operator-( const vector3& v ) const noexcept;
		[[nodiscard]] vector3 operator*( float scalar ) const noexcept;
		[[nodiscard]] vector3 operator/( float scalar ) const noexcept;
		[[nodiscard]] vector3 operator-( ) const noexcept;

		vector3& operator*=( float scalar ) noexcept;
		vector3& operator/=( float scalar ) noexcept;
		vector3& operator+=( const vector3& v ) noexcept;
		vector3& operator-=( const vector3& v ) noexcept;

		[[nodiscard]] bool operator==( const vector3& v ) const noexcept;
		[[nodiscard]] bool operator!=( const vector3& v ) const noexcept;

		[[nodiscard]] float dot( const vector3& v ) const noexcept;
		[[nodiscard]] vector3 cross( const vector3& v ) const noexcept;

		[[nodiscard]] float length_sqr( ) const noexcept;
		[[nodiscard]] float length( ) const noexcept;
		[[nodiscard]] float length_2d( ) const noexcept;

		vector3& normalize( ) noexcept;
		[[nodiscard]] vector3 normalized( ) const noexcept;
		[[nodiscard]] float normalize_2d( ) noexcept;
		[[nodiscard]] float distance( const vector3& v ) const noexcept;
		[[nodiscard]] float distance_sqr( const vector3& v ) const noexcept;
		[[nodiscard]] vector3 to_right_vector( ) const noexcept;
		void to_directions( vector3* forward, vector3* right, vector3* up ) const noexcept;
	};

	class quaternion
	{
	public:
		float x, y, z, w;

		quaternion( ) noexcept;
		constexpr quaternion( float x, float y, float z, float w ) noexcept;

		[[nodiscard]] static quaternion from_euler( const vector3& euler ) noexcept;
		[[nodiscard]] vector3 rotate_vector( const vector3& v ) const noexcept;
	};

	class matrix3x4
	{
	public:
		float mat[ 3 ][ 4 ];

		[[nodiscard]] const float* operator[]( int i ) const noexcept;
		[[nodiscard]] float* operator[]( int i ) noexcept;
	};

	class matrix4x4
	{
	public:
		float mat[ 4 ][ 4 ];

		[[nodiscard]] const float* operator[]( int index ) const noexcept;
		[[nodiscard]] float* operator[]( int index ) noexcept;
	};

	namespace helpers {

		void angle_vectors_left( const vector3& angles, vector3* forward = nullptr, vector3* left = nullptr, vector3* up = nullptr );
		void angle_vectors_2d( float yaw, math::vector3& forward, math::vector3& right );
		void normalize_angles( vector3& angles );
		void normalize_angle( float& angle );

		[[nodiscard]] vector3 vector_to_angle( const vector3& forward );
		[[nodiscard]] vector3 calculate_angle( const vector3& src, const vector3& dst );
		[[nodiscard]] float angle_distance( const vector3& from, const vector3& to );
		[[nodiscard]] float deg_to_rad( float degrees );
		[[nodiscard]] float rad_to_deg( float radians );
		[[nodiscard]] float normalize_yaw( float yaw );

	} // namespace helpers

} // namespace math
namespace fnv1a {

	constexpr std::uint32_t hash( const char* str, std::size_t length ) noexcept
	{
		std::uint32_t hash{ 2166136261u };

		for ( auto i = 0ull; i < length; ++i )
		{
			hash ^= static_cast< std::uint32_t >( str[ i ] );
			hash *= 16777619u;
		}

		return hash;
	}

	inline std::uint32_t runtime_hash( const char* str ) noexcept
	{
		if ( !str )
		{
			return 0;
		}

		std::uint32_t hash{ 2166136261u };

		while ( *str )
		{
			hash ^= static_cast< std::uint32_t >( *str++ );
			hash *= 16777619u;
		}

		return hash;
	}

} // namespace fnv1a

constexpr std::uint32_t operator""_hash( const char* str, std::size_t length ) noexcept
{
	return fnv1a::hash( str, length );
}

namespace memory {
bool copy(void* destination, std::uintptr_t address, std::size_t length) noexcept;
bool store(std::uintptr_t address,const void* value,std::size_t length) noexcept;
template<class T> bool safe_write(std::uintptr_t address,const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    return store(address,&value,sizeof(value));
}
template<class T> std::optional<T> safe_read(std::uintptr_t address) {
    static_assert(std::is_trivially_copyable_v<T>);
    T value{};
    if (!copy(&value,address,sizeof(value))) return std::nullopt;
    return value;
}
template<class T> T read(std::uintptr_t address) { return safe_read<T>(address).value_or(T{}); }
std::string read_string(std::uintptr_t address, std::size_t max_length=256);
std::uintptr_t resolve_pattern(std::string_view expression);
std::uintptr_t resolve_cached(std::string_view expression);
template<class T,class... A> T call(std::uintptr_t address,A... args) {
    if (!address) {
        if constexpr (std::is_void_v<T>) return;
        else return T{};
    }
    return reinterpret_cast<T(__fastcall*)(A...)>(address)(args...);
}
template<class T,class... A> T call_vfunc(std::uintptr_t instance,std::size_t slot,A... args) {
    const auto table=read<std::uintptr_t>(instance);
    const auto function=table ? read<std::uintptr_t>(table+slot*sizeof(void*)) : 0;
    return call<T>(function,instance,args...);
}
}
namespace cstypes {
inline constexpr float tick_interval=0.015625f;
	namespace bone_ids {

		constexpr auto head{ 7u };
		constexpr auto neck{ 6u };
		constexpr auto spine_1{ 2u };
		constexpr auto spine_2{ 3u };
		constexpr auto spine_3{ 4u };
		constexpr auto spine_4{ 23u };
		constexpr auto pelvis{ 1u };

		constexpr auto left_clavicle{ 8u };
		constexpr auto left_shoulder{ 9u };
		constexpr auto left_elbow{ 10u };
		constexpr auto left_hand{ 11u };

		constexpr auto right_clavicle{ 12u };
		constexpr auto right_shoulder{ 13u };
		constexpr auto right_elbow{ 14u };
		constexpr auto right_hand{ 15u };

		constexpr auto left_hip{ 17u };
		constexpr auto left_knee{ 18u };
		constexpr auto left_foot{ 19u };

		constexpr auto right_hip{ 20u };
		constexpr auto right_knee{ 21u };
		constexpr auto right_foot{ 22u };

	} // namespace bone_ids

	struct strong_handle
	{
		const void* binding;
	};

	struct key_values3
	{
		std::uint64_t metadata;
		std::uint64_t payload;
	};
	static_assert( sizeof( key_values3 ) == 0x10 );

	struct kv3_id
	{
		const char* format;
		std::uintptr_t guid_low;
		std::uintptr_t guid_high;
	};

	struct event_hash
	{
		std::uintptr_t hash;
		const char* str;
	};

}
namespace xdraw { inline std::pair<int,int> viewport_size() {const auto size=ImGui::GetIO().DisplaySize;return {static_cast<int>(size.x),static_cast<int>(size.y)};} }
namespace addresses::globals {
inline std::uintptr_t schema_system{}, entity_list{}, local_player_controller{};
inline std::uintptr_t view_matrix{}, global_vars{}, game_trace_manager_slot{}, planted_c4{};
}
namespace hvh { inline constexpr auto client_sha256=skin_bindings::kClientSha256; }
namespace systems {
class schemas
	{
	public:
		[[nodiscard]] static std::uint32_t lookup( const char* class_name, std::uint32_t field_hash );
	};
class entities
	{
	public:
		enum class type : std::uint8_t
		{
			unknown,
			player,
			item,
			projectile,
            pawn
		};

		struct cached
		{
			std::uintptr_t ptr{};
			std::uint32_t schema_hash{};
			std::int16_t index{};
			type type{};
		};

		void on_add_entity( std::uintptr_t entity, std::uint32_t handle );
		void on_remove_entity( std::uintptr_t entity, std::uint32_t handle );
		void force_update( );

		[[nodiscard]] bool exists( std::uintptr_t entity ) const;
		[[nodiscard]] const char* get_schema_name( std::uintptr_t entity ) const;
		[[nodiscard]] std::uintptr_t get_by_index( int index );
		[[nodiscard]] std::uintptr_t lookup( std::uint32_t handle ) const;
		[[nodiscard]] std::vector<cached> get_by_type( type type ) const;

		[[nodiscard]] bool is_empty( ) const;

	private:
		[[nodiscard]] type classify_entity( std::uint32_t schema_hash ) const;

		std::vector<cached> m_cached{};
		mutable std::shared_mutex m_cache_mtx{};
		std::array<std::uintptr_t, 64> m_cached_list_entries{};
		std::uintptr_t m_cached_entity_list{};
	};
class view
	{
	public:
		struct projection
		{
			math::vector2 screen{};
			float w{};
			bool on_screen{};
		};

		void update( std::uintptr_t view );
		void update_matrix( );

		[[nodiscard]] math::vector2 project( const math::vector3& world_pos );
		[[nodiscard]] projection project_full( const math::vector3& world_pos ) const;
		[[nodiscard]] bool projection_valid( const math::vector2& screen_pos ) { return screen_pos.x != this->k_invalid && screen_pos.y != this->k_invalid; }

		[[nodiscard]] bool has_camera( ) const { return this->m_matrix_valid.load( std::memory_order_acquire ); }
		[[nodiscard]] math::vector3 origin( ) const { return this->m_origin; }
		[[nodiscard]] math::vector3 angles( ) const { return this->m_angles; }
		[[nodiscard]] float fov( ) const { return this->m_fov; }
		[[nodiscard]] math::matrix4x4 matrix( ) const;

	private:
		static constexpr auto k_invalid{ 0xdead };

		math::matrix4x4 m_matrix{};
		mutable std::mutex m_matrix_mtx{};
		std::atomic<bool> m_matrix_valid{};
		math::vector3 m_origin{};
		math::vector3 m_angles{};
		float m_fov{ k_invalid };
	};
class bones
	{
	public:
		struct data
		{
			math::vector3 position{};
			float scale{};
			math::quaternion rotation{};
		};

		[[nodiscard]] data get( std::uintptr_t entity, std::uint32_t bone_id );
		[[nodiscard]] std::array<data, 27> get_skeleton( std::uintptr_t entity );
		// HVH requires verified names; ESP may retain its stock-index fallback.
		[[nodiscard]] std::array<data, 27> get_skeleton( std::uintptr_t entity, bool named_only );

	private:
		[[nodiscard]] std::uintptr_t get_bone_cache( std::uintptr_t entity, int* out_count = nullptr );
	};
class bounds
	{
	public:
		struct data
		{
			math::vector2 min{};
			math::vector2 max{};
			bool valid{};

			[[nodiscard]] float width( ) const { return this->max.x - this->min.x; }
			[[nodiscard]] float height( ) const { return this->max.y - this->min.y; }
			[[nodiscard]] math::vector2 center( ) const { return { this->min.x + this->width( ) * 0.5f, this->min.y + this->height( ) * 0.5f }; }
		};

		[[nodiscard]] data get( std::uintptr_t entity );
	};
class tracing
	{
	public:
		struct filter
		{
			std::uintptr_t vtable;
			std::uintptr_t mask;
			std::array<std::int64_t, 2> v1;
			std::array<int, 4> skip_handles;
			std::array<std::int16_t, 2> collisions;
			std::int16_t v2;
			std::uint8_t layer;
			std::uint8_t flags;
			std::uint8_t v5;
			std::uint8_t v6;
			std::byte pad0[ 0x6 ];
			char v7;
		};

		struct ray
		{
			math::vector3 mins;
			math::vector3 maxs;
			std::byte pad0[ 0x10 ];
			std::uint8_t type;
			std::byte pad1[ 0x7 ];
		};

		struct result
		{
			void* surface;
			std::uintptr_t hit_entity;
			void* hitbox_data;
			std::byte pad0[ 0x38 ];
			std::uint32_t contents;
			std::byte pad1[ 0x24 ];
			math::vector3 start_pos;
			math::vector3 end_pos;
			math::vector3 normal;
			math::vector3 position;
			std::byte pad2[ 0x4 ];
			float fraction;
			std::byte pad3[ 0xb ];
			bool all_solid;
			// Native point-ray finalization sets this when position at 0x9c
			// is cached. It is not a start-in-solid flag.
			bool contact_position_valid;
			std::byte pad4[ 0x3 ];
		};

		static_assert( offsetof( result, start_pos ) == 0x78 );
		static_assert( offsetof( result, fraction ) == 0xac );
		static_assert( offsetof( result, all_solid ) == 0xbb );
		static_assert( offsetof( result, contact_position_valid ) == 0xbc );
		static_assert( sizeof( result ) == 0xc0 );

        [[nodiscard]] bool is_visible(const math::vector3& start,const math::vector3& end,std::uintptr_t target_entity,std::uintptr_t skip_entity,std::uintptr_t mask=0x1c3003) const;
        [[nodiscard]] result trace(const math::vector3& start,const math::vector3& end,std::uintptr_t skip_entity,std::uintptr_t mask=0x1c3003,std::uint8_t layer=4) const;
        [[nodiscard]] result trace(const math::vector3& start,const math::vector3& end,const filter& filter) const;
        [[nodiscard]] filter make_filter(std::uintptr_t skip_entity,std::uintptr_t mask,std::uint8_t layer) const;
    };
inline entities g_entities{};
inline view g_view{};
inline bones g_bones{};
inline bounds g_bounds{};
inline tracing g_tracing{};
}
#define SCHEMA(class_name, field_hash) ([] { static const auto offset = systems::schemas::lookup(class_name, field_hash); return offset; }())

#include "frame_data.hpp"

namespace game {
bool initialize();
bool in_world() noexcept;
const std::string& status();
bool module_matches(const char* module,std::string_view expected_hash);
bool module_matches_handle(std::uintptr_t module,std::string_view expected_hash);
visuals::frame_snapshot capture(float width,float height);
void reset();
void report_hit(std::uintptr_t message);
}

namespace patterns {
inline constexpr std::string_view draw_skybox_array = "scenesystem.dll:4585C90F8E????????4C8BDC";
inline constexpr std::string_view frame_stage_notify = "client.dll:48895C241848896C2420574883EC40488BF933ED488B8960";
inline constexpr std::string_view game_event_get_controller = "client.dll:488D05*????????4D8BF8488901+80~";
inline constexpr std::string_view game_event_get_float = "client.dll:>E8????????0F28D8895C2420";
inline constexpr std::string_view game_event_get_int = "client.dll:>E8????????3D00800000";
inline constexpr std::string_view game_event_get_pawn = "client.dll:488D05*????????4D8BF8488901+88~";
inline constexpr std::string_view game_event_manager = "client.dll:488B0D*????????488B01FF50??FFC3~";
inline constexpr std::string_view game_rules = "client.dll:488B0D*????????4C897010";
// Keep the global's address; the manager itself can be replaced at runtime.
inline constexpr std::string_view game_trace_manager = "client.dll:488B0D*????????488D3452";
inline constexpr std::string_view generate_primitives = "scenesystem.dll:488BC4488958204C89401848895010488948085556574154415541564157488D";
inline constexpr std::string_view global_vars = "client.dll:488B05*????????448B4044";
// Refreshed against the 2026-10-06 client.dll analyzed in IDA.
inline constexpr std::string_view init_particle_path_buffer = "client.dll:48895C24??574883EC??8B41??488D79??488BD9A9FFFFFF3F76??";
inline constexpr std::string_view kv3_alloc = "tier0.dll:40534883EC3080FA060FB6C241B916";
inline constexpr std::string_view kv3_destroy = "tier0.dll:405741574883EC384C8B01448BFA498BC0488BF948C1E802";
inline constexpr std::string_view level_shutdown = "client.dll:4883EC??488B0D????????488D15????????4533C94533C0488B01FF50304885C074??488B0D????????488BD04C8B0141FF50404883C4??";
inline constexpr std::string_view light_scene_object = "scenesystem.dll:>E8????????440F285C2460";
inline constexpr std::string_view material_create = "materialsystem2.dll:48895C24??48896C24??48897424??48897C24??41564881EC????????488B05????????488BF2";
// CCSUsrMsg_ReportHit vtable reference, audited for the pinned client build.
inline constexpr std::string_view parse_report_hit = "client.dll:48895C24184889742420574883EC20488D05B24259018BF2488901488BD9";
inline constexpr std::string_view particle_create_effect = "client.dll:4C8BDC534881EC90000000F20F1005????????488BDA488B8424C000000033D2";
inline constexpr std::string_view particle_destroy_effect = "client.dll:83FAFF0F84????????4154415641574883EC??48895C24??";
inline constexpr std::string_view particle_manager = "client.dll:488B35*????????44896C24??4C896C24??C74424??C80000C0";
inline constexpr std::string_view particle_set_control_point = "client.dll:4883EC58F3410F105104F3410F1009F3410F1059084C8D4C2430";
inline constexpr std::string_view planted_c4 = "client.dll:488B1D*????????488BD34C8B81";
// Refreshed against the 2026-10-06 resourcesystem.dll analyzed in IDA.
inline constexpr std::string_view resource_system_load = "resourcesystem.dll:48895C24??48896C24??48897424??574883EC??488B01498BE8488BDA488BF1FF90????????85C0750433FFEB15488B064533C0488BD3";
inline constexpr std::string_view resource_system_precache = "resourcesystem.dll:405355574881EC80000000488B01498BE8488BFA488BD9FF";
inline constexpr std::string_view set_postprocess_vec = "engine2.dll:>E8????????440F289424";
inline constexpr std::string_view set_shader_param = "client.dll:4C8D442430BA63FF014B498BCF0F10000F29442430>E8????????";
inline constexpr std::string_view setup_fog = "client.dll:48895C24??48896C24??48897424??48894C24??5741544155415641574883EC20486302";
inline constexpr std::string_view trace_filter_init = "client.dll:48895C24??48897424??574883EC??0FB641??33FF24";
inline constexpr std::string_view trace_ray = "client.dll:488954241048894C240855535657415441564157488DAC24F0DBFFFFB810250000";
inline constexpr std::string_view view_matrix = "client.dll:488D0D*????????48C1E006";
}
#define PATTERN(value) memory::resolve_cached(value)
