#include "engine.hpp"
#include "settings.hpp"
#include "core/runtime_paths.h"
#include "core/module_digest.hpp"
#include "core/pattern_match.hpp"
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#pragma comment(lib,"bcrypt.lib")

namespace memory {
bool copy(void* destination,std::uintptr_t address,std::size_t length) noexcept {
    if (!destination || address < 0x10000 || length > 256 * 1024 * 1024) return false;
    __try { std::memcpy(destination,reinterpret_cast<const void*>(address),length); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool store(std::uintptr_t address,const void* value,std::size_t length) noexcept {
    if (!value || address<0x10000 || length>1024*1024) return false;
    __try {std::memcpy(reinterpret_cast<void*>(address),value,length);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}

std::string read_string(std::uintptr_t address,std::size_t max_length) {
    std::string value;
    for (std::size_t i=0;i<std::min(max_length,std::size_t{4096});++i) {
        const auto c=safe_read<char>(address+i);
        if (!c || !*c) break;
        value.push_back(*c);
    }
    return value;
}

namespace {
struct parsed_pattern {
    std::vector<int> bytes;
    char operation{};
    std::size_t operand{};
    std::int64_t adjustment{};
    bool dereference{};
};
int hex(char c) {
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    if (c>='A' && c<='F') return c-'A'+10;
    return -1;
}
// Retains HVH's compact/space-delimited syntax and hexadecimal suffix offsets.
std::optional<parsed_pattern> parse(std::string_view expression) {
    parsed_pattern result;
    for (std::size_t i=0;i<expression.size();) {
        const char c=expression[i++];
        if (c==' ' || c=='\t') continue;
        if (c=='~') { result.dereference=true; continue; }
        if (c=='>' || c=='*' || c=='^') {
            if (result.operation) return std::nullopt;
            result.operation=c; result.operand=result.bytes.size(); continue;
        }
        if (c=='+' || c=='-') {
            std::int64_t offset{};
            const auto start=i;
            while (i<expression.size() && hex(expression[i])>=0) {
                if (offset > 0x7fffffff) return std::nullopt;
                offset=offset*16+hex(expression[i++]);
            }
            if (start==i) return std::nullopt;
            result.adjustment += c=='-' ? -offset : offset;
            continue;
        }
        if (c=='?') {
            if (i<expression.size() && expression[i]=='?') ++i;
            result.bytes.push_back(-1); continue;
        }
        if (hex(c)<0 || i>=expression.size() || hex(expression[i])<0) return std::nullopt;
        result.bytes.push_back(hex(c)*16+hex(expression[i++]));
        if (result.bytes.size()>256) return std::nullopt;
    }
    if (result.bytes.empty() || std::ranges::all_of(result.bytes,[](int v){return v<0;})) return std::nullopt;
    return result;
}
void log(std::string_view message,std::string_view expression) {
    const auto line=std::string("[Cs2 cheat] ")+std::string(message)+": "+std::string(expression)+"\n";
    OutputDebugStringA(line.c_str());
}
}

std::uintptr_t resolve_pattern(std::string_view expression) {
    const auto colon=expression.find(':');
    if (colon==std::string_view::npos) return 0;
    const auto module=GetModuleHandleA(std::string(expression.substr(0,colon)).c_str());
    const auto pattern=parse(expression.substr(colon+1));
    if (!module || !pattern) return 0;
    const auto base=reinterpret_cast<std::uintptr_t>(module);
    const auto dos=read<IMAGE_DOS_HEADER>(base);
    if (dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<=0) return 0;
    const auto nt=read<IMAGE_NT_HEADERS64>(base+dos.e_lfanew);
    if (nt.Signature!=IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC
        || nt.FileHeader.NumberOfSections>96) return 0;
    const auto section_table=base+dos.e_lfanew+offsetof(IMAGE_NT_HEADERS64,OptionalHeader)+nt.FileHeader.SizeOfOptionalHeader;
    std::uintptr_t match{};
    const native_startup::PatternMatcher matcher(pattern->bytes);
    for (unsigned index=0;index<nt.FileHeader.NumberOfSections;++index) {
        const auto section=read<IMAGE_SECTION_HEADER>(section_table+index*sizeof(IMAGE_SECTION_HEADER));
        if (!(section.Characteristics&IMAGE_SCN_MEM_READ) || section.Characteristics&IMAGE_SCN_MEM_DISCARDABLE) continue;
        const auto length=static_cast<std::size_t>(section.Misc.VirtualSize);
        if (length<pattern->bytes.size() || section.VirtualAddress>=nt.OptionalHeader.SizeOfImage
            || length>nt.OptionalHeader.SizeOfImage-section.VirtualAddress) continue;
        std::vector<std::uint8_t> buffer(length);
        if (!copy(buffer.data(),base+section.VirtualAddress,length)) continue;
        if(!matcher.Scan(buffer,[&](std::size_t offset) {
            if(match)return false;
            match=base+section.VirtualAddress+offset;return true;
        })) {log("ambiguous signature refused",expression);return 0;}
    }
    if (!match) { log("signature unavailable",expression); return 0; }
    auto result=match;
    if (pattern->operation=='>' || pattern->operation=='*') {
        const auto operand=match+pattern->operand+(pattern->operation=='>' ? 1 : 0);
        const auto displacement=safe_read<std::int32_t>(operand);
        if (!displacement) return 0;
        result=operand+4+*displacement;
    } else if (pattern->operation=='^') result=read<std::uintptr_t>(match+pattern->operand);
    result+=pattern->adjustment;
    if (pattern->dereference) result=read<std::uintptr_t>(result);
    return result;
}

namespace {
struct resolver_cache {
    SRWLOCK mutex=SRWLOCK_INIT;
    std::unordered_map<std::string,std::uintptr_t> values;
};
constinit INIT_ONCE resolver_cache_once=INIT_ONCE_STATIC_INIT;

BOOL CALLBACK initialize_resolver_cache(PINIT_ONCE,PVOID,PVOID* context) noexcept {
    try {
        // The retained module can resolve signatures for the process lifetime.
        // Explicit initialization avoids a local-static CRT/TLS constructor guard.
        *context=new resolver_cache;
        return TRUE;
    } catch (...) {
        // No C++ exception may cross the Win32 initialization callback.
        return FALSE;
    }
}

struct resolver_cache_lock {
    SRWLOCK& mutex;
    explicit resolver_cache_lock(SRWLOCK& value) noexcept : mutex(value) {
        AcquireSRWLockExclusive(&mutex);
    }
    ~resolver_cache_lock() { ReleaseSRWLockExclusive(&mutex); }
    resolver_cache_lock(const resolver_cache_lock&)=delete;
    resolver_cache_lock& operator=(const resolver_cache_lock&)=delete;
};
}

std::uintptr_t resolve_cached(std::string_view expression) {
    PVOID context=nullptr;
    if (!InitOnceExecuteOnce(&resolver_cache_once,initialize_resolver_cache,nullptr,&context)) return 0;
    auto& state=*static_cast<resolver_cache*>(context);
    const resolver_cache_lock lock(state.mutex);
    auto& cache=state.values;
    const auto key=std::string(expression);
    if (const auto found=cache.find(key);found!=cache.end()) return found->second;
    const auto address=resolve_pattern(expression);
    // A module or native object may become available after installation starts.
    // Keep successful resolutions stable without making an early miss permanent.
    if (address) cache.emplace(key,address);
    return address;
}
}



namespace math {


	vector3 vector3::operator+( const vector3& v ) const noexcept { return { x + v.x, y + v.y, z + v.z }; }
	vector3 vector3::operator-( const vector3& v ) const noexcept { return { x - v.x, y - v.y, z - v.z }; }
	vector3 vector3::operator*( float scalar ) const noexcept { return { x * scalar, y * scalar, z * scalar }; }
	vector3 vector3::operator/( float scalar ) const noexcept { return { x / scalar, y / scalar, z / scalar }; }
	vector3 vector3::operator-( ) const noexcept { return { -x, -y, -z }; }

	vector3& vector3::operator*=( float scalar ) noexcept { x *= scalar; y *= scalar; z *= scalar; return *this; }
	vector3& vector3::operator/=( float scalar ) noexcept { x /= scalar; y /= scalar; z /= scalar; return *this; }
	vector3& vector3::operator+=( const vector3& v ) noexcept { x += v.x; y += v.y; z += v.z; return *this; }
	vector3& vector3::operator-=( const vector3& v ) noexcept { x -= v.x; y -= v.y; z -= v.z; return *this; }

	bool vector3::operator==( const vector3& v ) const noexcept { return x == v.x && y == v.y && z == v.z; }
	bool vector3::operator!=( const vector3& v ) const noexcept { return !( *this == v ); }

	float vector3::dot( const vector3& v ) const noexcept { return x * v.x + y * v.y + z * v.z; }
	vector3 vector3::cross( const vector3& v ) const noexcept { return { y * v.z - z * v.y, z * v.x - x * v.z, x * v.y - y * v.x }; }

	float vector3::length_sqr( ) const noexcept { return x * x + y * y + z * z; }

	float vector3::length( ) const noexcept
	{
		const auto len_sq = this->length_sqr( );
		if ( len_sq == 0.0f ) [[unlikely]]
		{
			return 0.0f;
		}

		return std::sqrtf( len_sq );
	}

	float vector3::length_2d( ) const noexcept
	{
		const auto len_sq_2d = x * x + y * y;
		if ( len_sq_2d == 0.0f ) [[unlikely]]
		{
			return 0.0f;
		}

		return std::sqrtf( len_sq_2d );
	}

	vector3& vector3::normalize( ) noexcept
	{
		const auto len = this->length( );
		if ( len > 0.0f ) [[likely]]
		{
			const float inv_len = 1.0f / len;
			*this *= inv_len;
		}

		return *this;
	}

	vector3 vector3::normalized( ) const noexcept
	{
		auto vec = *this;
		vec.normalize( );
		return vec;
	}

	float vector3::normalize_2d( ) noexcept
	{
		const auto len = this->length_2d( );
		if ( len > 0.0f ) [[likely]]
		{
			const float inv_len = 1.0f / len;
			x *= inv_len;
			y *= inv_len;
		}

		z = 0.0f;
		return len;
	}

	float vector3::distance( const vector3& v ) const noexcept { return ( *this - v ).length( ); }
	float vector3::distance_sqr( const vector3& v ) const noexcept { return ( *this - v ).length_sqr( ); }

	vector3 vector3::to_right_vector( ) const noexcept
	{
		auto right = this->cross( vector3( 0.0f, 0.0f, 1.0f ) );

		if ( right.length_sqr( ) < 1e-6f )
		{
			right = this->cross( vector3( 0.0f, -1.0f, 0.0f ) );
		}

		return right.normalize( );
	}

	void vector3::to_directions( vector3* forward, vector3* right, vector3* up ) const noexcept
	{
		constexpr auto deg_to_rad = std::numbers::pi_v<float> / 180.0f;

		const auto sp = std::sinf( x * deg_to_rad );
		const auto cp = std::cosf( x * deg_to_rad );
		const auto sy = std::sinf( y * deg_to_rad );
		const auto cy = std::cosf( y * deg_to_rad );
		const auto sr = std::sinf( z * deg_to_rad );
		const auto cr = std::cosf( z * deg_to_rad );

		if ( forward )
		{
			forward->x = cp * cy;
			forward->y = cp * sy;
			forward->z = -sp;
		}

		if ( right )
		{
			right->x = -1.0f * sr * sp * cy + -1.0f * cr * -sy;
			right->y = -1.0f * sr * sp * sy + -1.0f * cr * cy;
			right->z = -1.0f * sr * cp;
		}

		if ( up )
		{
			up->x = cr * sp * cy + -sr * -sy;
			up->y = cr * sp * sy + -sr * cy;
			up->z = cr * cp;
		}
	}


	vector2 vector2::operator+( const vector2& v ) const noexcept { return { x + v.x, y + v.y }; }
	vector2 vector2::operator-( const vector2& v ) const noexcept { return { x - v.x, y - v.y }; }
	vector2 vector2::operator*( float scalar ) const noexcept { return { x * scalar, y * scalar }; }
	vector2 vector2::operator/( float scalar ) const noexcept { return { x / scalar, y / scalar }; }
	vector2 vector2::operator-( ) const noexcept { return { -x, -y }; }

	vector2& vector2::operator*=( float scalar ) noexcept { x *= scalar; y *= scalar; return *this; }
	vector2& vector2::operator/=( float scalar ) noexcept { x /= scalar; y /= scalar; return *this; }
	vector2& vector2::operator+=( const vector2& v ) noexcept { x += v.x; y += v.y; return *this; }
	vector2& vector2::operator-=( const vector2& v ) noexcept { x -= v.x; y -= v.y; return *this; }

	bool vector2::operator==( const vector2& v ) const noexcept { return x == v.x && y == v.y; }
	bool vector2::operator!=( const vector2& v ) const noexcept { return !( *this == v ); }

	float vector2::dot( const vector2& v ) const noexcept { return x * v.x + y * v.y; }
	float vector2::length_sqr( ) const noexcept { return x * x + y * y; }

	float vector2::length( ) const noexcept
	{
		const auto len_sq = length_sqr( );
		if ( len_sq == 0.0f ) [[unlikely]]
		{
			return 0.0f;
		}

		return std::sqrtf( len_sq );
	}

	vector2& vector2::normalize( ) noexcept
	{
		const auto len = length( );
		if ( len > 0.0f ) [[likely]]
		{
			const float inv_len = 1.0f / len;
			*this *= inv_len;
		}

		return *this;
	}

	vector2 vector2::normalized( ) const noexcept
	{
		auto vec = *this;
		vec.normalize( );
		return vec;
	}

	// -------------------------------- //

	quaternion::quaternion( ) noexcept : x( 0.0f ), y( 0.0f ), z( 0.0f ), w( 1.0f ) {}
	constexpr quaternion::quaternion( float x, float y, float z, float w ) noexcept : x( x ), y( y ), z( z ), w( w ) {}

	quaternion quaternion::from_euler( const vector3& euler ) noexcept
	{
		const auto pitch = euler.x * ( std::numbers::pi_v<float> / 180.0f );
		const auto yaw = euler.y * ( std::numbers::pi_v<float> / 180.0f );
		const auto roll = euler.z * ( std::numbers::pi_v<float> / 180.0f );

		const auto cy = std::cos( yaw * 0.5f );
		const auto sy = std::sin( yaw * 0.5f );
		const auto cp = std::cos( pitch * 0.5f );
		const auto sp = std::sin( pitch * 0.5f );
		const auto cr = std::cos( roll * 0.5f );
		const auto sr = std::sin( roll * 0.5f );

		quaternion q;
		q.w = cr * cp * cy + sr * sp * sy;
		q.x = sr * cp * cy - cr * sp * sy;
		q.y = cr * sp * cy + sr * cp * sy;
		q.z = cr * cp * sy - sr * sp * cy;

		return q;
	}

	vector3 quaternion::rotate_vector( const vector3& v ) const noexcept
	{
		const auto q_vec = vector3( this->x, this->y, this->z );
		auto uv = q_vec.cross( v );
		auto uuv = q_vec.cross( uv );

		uv *= ( 2.0f * this->w );
		uuv *= 2.0f;

		return v + uv + uuv;
	}

	// -------------------------------- //

	const float* matrix3x4::operator[]( int i ) const noexcept
	{
		return mat[ i ];
	}

	float* matrix3x4::operator[]( int i ) noexcept
	{
		return mat[ i ];
	}

	const float* matrix4x4::operator[]( int index ) const noexcept
	{
		return mat[ index ];
	}

	float* matrix4x4::operator[]( int index ) noexcept
	{
		return mat[ index ];
	}

	// -------------------------------- //

	namespace helpers {

		void angle_vectors_left( const vector3& angles, vector3* forward, vector3* left, vector3* up )
		{
			const auto pitch = angles.x * ( std::numbers::pi_v<float> / 180.0f );
			const auto yaw = angles.y * ( std::numbers::pi_v<float> / 180.0f );

			const auto sin_pitch = std::sinf( pitch );
			const auto cos_pitch = std::cosf( pitch );
			const auto sin_yaw = std::sinf( yaw );
			const auto cos_yaw = std::cosf( yaw );

			if ( forward )
			{
				forward->x = cos_pitch * cos_yaw;
				forward->y = cos_pitch * sin_yaw;
				forward->z = -sin_pitch;
			}

			if ( left || up )
			{
				const auto roll = angles.z * ( std::numbers::pi_v<float> / 180.0f );
				const auto sin_roll = std::sinf( roll );
				const auto cos_roll = std::cosf( roll );

				if ( left )
				{
					left->x = -sin_roll * sin_pitch * cos_yaw + cos_roll * sin_yaw;
					left->y = -sin_roll * sin_pitch * sin_yaw - cos_roll * cos_yaw;
					left->z = -sin_roll * cos_pitch;
				}

				if ( up )
				{
					up->x = cos_roll * sin_pitch * cos_yaw + sin_roll * sin_yaw;
					up->y = cos_roll * sin_pitch * sin_yaw - sin_roll * cos_yaw;
					up->z = cos_roll * cos_pitch;
				}
			}
		}

		void angle_vectors_2d( float yaw, math::vector3& forward, math::vector3& right )
		{
			const auto rad = yaw * ( std::numbers::pi_v<float> / 180.0f );
			const auto sy = std::sinf( rad );
			const auto cy = std::cosf( rad );

			forward = { cy, sy, 0.0f };
			right = { -sy, cy, 0.0f };
		}

		void normalize_angles( vector3& angles )
		{
			while ( angles.y > 180.0f ) { angles.y -= 360.0f; }
			while ( angles.y < -180.0f ) { angles.y += 360.0f; }
			while ( angles.x > 89.0f ) { angles.x -= 180.0f; }
			while ( angles.x < -89.0f ) { angles.x += 180.0f; }

			angles.z = 0.0f;
		}

		void normalize_angle( float& angle )
		{
			while ( angle > 180.0f ) { angle -= 360.0f; }
			while ( angle < -180.0f ) { angle += 360.0f; }
		}

		vector3 vector_to_angle( const vector3& forward )
		{
			auto pitch = rad_to_deg( std::atan2( -forward.z, forward.length_2d( ) ) );
			auto yaw = rad_to_deg( std::atan2( forward.y, forward.x ) );
			return { pitch, yaw, 0.f };
		}

		vector3 calculate_angle( const vector3& src, const vector3& dst )
		{
			const auto delta = dst - src;
			const auto length = delta.length_2d( );

			vector3 angles;
			angles.x = rad_to_deg( std::atan2f( -delta.z, length ) );
			angles.y = rad_to_deg( std::atan2f( delta.y, delta.x ) );
			angles.z = 0.0f;

			return angles;
		}

		float angle_distance( const vector3& from, const vector3& to )
		{
			const auto pitch = to.x - from.x;
			const auto yaw = normalize_yaw( to.y - from.y );
			return std::sqrtf( pitch * pitch + yaw * yaw );
		}

		float deg_to_rad( float degrees )
		{
			return degrees * ( std::numbers::pi_v<float> / 180.0f );
		}

		float rad_to_deg( float radians )
		{
			return radians * ( 180.0f / std::numbers::pi_v<float> );
		}

		float normalize_yaw( float yaw )
		{
			while ( yaw > 180.0f ) { yaw -= 360.0f; }
			while ( yaw < -180.0f ) { yaw += 360.0f; }
			return yaw;
		}

	} // namespace helpers

} // namespace math


namespace systems {
// Source 2 lookup and field layout are taken from HVH schemas.cpp. Only positive
// results are cached, so fields can become available after client initialization.
std::uint32_t schemas::lookup(const char* class_name,std::uint32_t field_hash) {
    if (!class_name || !addresses::globals::schema_system) return 0;
    static std::mutex mutex;
    static std::unordered_map<std::string,std::uint32_t> offsets;
    std::scoped_lock lock(mutex);
    const auto key=std::string(class_name)+":"+std::to_string(field_hash);
    if (const auto found=offsets.find(key);found!=offsets.end()) return found->second;
    const auto scope=memory::call_vfunc<std::uintptr_t>(addresses::globals::schema_system,13,"client.dll",nullptr);
    if (!scope) return 0;
    std::uintptr_t class_info{};
    memory::call_vfunc<void>(scope,2,&class_info,class_name);
    const auto fields=memory::read<std::uintptr_t>(class_info+0x30);
    const auto count=memory::read<std::uint16_t>(class_info+0x24);
    if (!fields || count>4096) return 0;
    for (unsigned i=0;i<count;++i) {
        const auto field=fields+i*0x20;
        const auto name=memory::read_string(memory::read<std::uintptr_t>(field));
        if (fnv1a::runtime_hash(name.c_str())!=field_hash) continue;
        const auto offset=memory::read<std::uint32_t>(field+0x10);
        if (offset>0 && offset<0x40000) { offsets.emplace(key,offset); return offset; }
        return 0;
    }
    return 0;
}
}



namespace systems {

	void entities::on_add_entity( std::uintptr_t entity, std::uint32_t handle )
	{
		if ( !entity )
		{
			return;
		}

		const auto index = static_cast< std::int16_t >( handle & 0x7fff );
		if ( index < 0 || index > 0x7ffe )
		{
			return;
		}

		const auto schema_name = this->get_schema_name( entity );
		if ( !schema_name )
		{
			return;
		}

		const auto name_copy = memory::read_string(reinterpret_cast<std::uintptr_t>(schema_name));
		const auto hashed = fnv1a::runtime_hash(name_copy.c_str());
		const auto entity_type = this->classify_entity( hashed );

		if ( entity_type == type::unknown )
		{
			return;
		}

		cached entry{};
		entry.ptr = entity;
		entry.index = index;
		entry.schema_hash = hashed;
		entry.type = entity_type;

		std::unique_lock lock( this->m_cache_mtx );

		for ( const auto& c : this->m_cached )
		{
			if ( c.index == index )
			{
				return;
			}
		}

		this->m_cached.emplace_back( entry );
	}

	void entities::on_remove_entity( std::uintptr_t entity, std::uint32_t handle )
	{
		if ( !entity )
		{
			return;
		}

		const auto index = static_cast< std::int16_t >( handle & 0x7fff );
		if ( index < 0 || index > 0x3fff )
		{
			return;
		}

		std::unique_lock lock( this->m_cache_mtx );

		for ( auto it = this->m_cached.begin( ); it != this->m_cached.end( ); ++it )
		{
			if ( it->index == index )
			{
				if ( it != this->m_cached.end( ) - 1 )
				{
					*it = this->m_cached.back( );
				}

				this->m_cached.pop_back( );
				return;
			}
		}
	}

	void entities::force_update( )
	{
		{
			std::unique_lock lock( this->m_cache_mtx );
			this->m_cached.clear( );
			this->m_cached.reserve( 128 );
		}

		for ( auto i = 0; i < 32767; ++i )
		{
			const auto entity = this->get_by_index( i );
			if ( !entity )
			{
				continue;
			}

			this->on_add_entity( entity, static_cast< std::uint32_t >( i ) );
		}
	}

	bool entities::exists( std::uintptr_t entity_ptr ) const
	{
		std::shared_lock lock( this->m_cache_mtx );

		for ( const auto& c : this->m_cached )
		{
			if ( c.ptr == entity_ptr )
			{
				return true;
			}
		}

		return false;
	}

	const char* entities::get_schema_name( std::uintptr_t entity ) const
	{
		const auto identity = memory::safe_read<std::uintptr_t>( entity + 0x10 ).value_or( 0 );
		if ( !identity )
		{
			return nullptr;
		}

		const auto entity_class = memory::safe_read<std::uintptr_t>( identity + 0x8 ).value_or( 0 );
		if ( !entity_class )
		{
			return nullptr;
		}

		// CEntityClass owns the current SchemaClassInfo pointer at 0x58.
		const auto class_info = memory::safe_read<std::uintptr_t>( entity_class + 0x58 ).value_or( 0 );
		if ( !class_info )
		{
			return nullptr;
		}

		return memory::safe_read<const char*>( class_info + 0x8 ).value_or( nullptr );
	}

    std::uintptr_t entities::get_by_index(int index) {
        return index>0 ? game_access::Entity(memory::read<std::uintptr_t>(addresses::globals::entity_list),static_cast<unsigned>(index)) : 0;
    }

	std::uintptr_t entities::lookup( std::uint32_t handle ) const
	{
		if ( !handle || handle == 0xffffffff )
		{
			return 0;
		}

		const auto entity_list = memory::read<std::uintptr_t>( addresses::globals::entity_list );
		if ( !entity_list )
		{
			return 0;
		}

		const auto chunk_index = ( handle & 0x7fff ) >> 9;

		const auto list_entry = memory::read<std::uintptr_t>( entity_list + ( static_cast< std::uintptr_t >( chunk_index ) * 8 ) + 0x10 );
		if ( !list_entry || list_entry == 0xffffffffffffffff )
		{
			return 0;
		}

		const auto slot = list_entry + ( static_cast< std::uintptr_t >( handle & 0x1ff ) * 112 );

		const auto entity = memory::read<std::uintptr_t>( slot );
		if ( !entity || entity == 0xffffffffffffffff || entity < 0x10000 )
		{
			return 0;
		}

		const auto stored_handle = memory::read<std::uint32_t>( slot + 0x10 );
		if (stored_handle != handle)
		{
			return 0;
		}

		return entity;
	}

	std::vector<entities::cached> entities::get_by_type( type type ) const
	{
		std::shared_lock lock( this->m_cache_mtx );

		std::vector<cached> result{};
		result.reserve( this->m_cached.size( ) );

		for ( const auto& c : this->m_cached )
		{
			if ( c.type == type )
			{
				result.emplace_back( c );
			}
		}

		return result;
	}

	bool entities::is_empty( ) const
	{
		std::shared_lock lock( this->m_cache_mtx );
		return this->m_cached.empty( );
	}

	entities::type entities::classify_entity( std::uint32_t schema_hash ) const
	{
		switch ( schema_hash )
		{
		case "CCSPlayerController"_hash:
			return type::player;
        case "C_CSPlayerPawn"_hash:
            return type::pawn;

		case "C_AK47"_hash:
		case "C_WeaponM4A1"_hash:
		case "C_WeaponM4A1Silencer"_hash:
		case "C_WeaponAWP"_hash:
		case "C_WeaponAug"_hash:
		case "C_WeaponFamas"_hash:
		case "C_WeaponGalilAR"_hash:
		case "C_WeaponSG556"_hash:
		case "C_WeaponG3SG1"_hash:
		case "C_WeaponSCAR20"_hash:
		case "C_WeaponSSG08"_hash:
		case "C_WeaponMAC10"_hash:
		case "C_WeaponMP5SD"_hash:
		case "C_WeaponMP7"_hash:
		case "C_WeaponMP9"_hash:
		case "C_WeaponBizon"_hash:
		case "C_WeaponP90"_hash:
		case "C_WeaponUMP45"_hash:
		case "C_WeaponNOVA"_hash:
		case "C_WeaponSawedoff"_hash:
		case "C_WeaponXM1014"_hash:
		case "C_WeaponMag7"_hash:
		case "C_WeaponM249"_hash:
		case "C_WeaponNegev"_hash:
		case "C_DEagle"_hash:
		case "C_WeaponElite"_hash:
		case "C_WeaponFiveSeven"_hash:
		case "C_WeaponGlock"_hash:
		case "C_WeaponHKP2000"_hash:
		case "C_WeaponUSPSilencer"_hash:
		case "C_WeaponP250"_hash:
		case "C_WeaponCZ75a"_hash:
		case "C_WeaponTec9"_hash:
		case "C_WeaponRevolver"_hash:
		case "C_WeaponTaser"_hash:
		case "C_Knife"_hash:
		case "C_PlantedC4"_hash:
		case "C_C4"_hash:
		case "C_Hostage"_hash:
		case "C_Chicken"_hash:
		case "C_Item_Healthshot"_hash:
		case "C_HEGrenade"_hash:
		case "C_Flashbang"_hash:
		case "C_SmokeGrenade"_hash:
		case "C_MolotovGrenade"_hash:
		case "C_IncendiaryGrenade"_hash:
		case "C_DecoyGrenade"_hash:
			return type::item;

		case "C_HEGrenadeProjectile"_hash:
		case "C_FlashbangProjectile"_hash:
		case "C_SmokeGrenadeProjectile"_hash:
		case "C_MolotovProjectile"_hash:
		case "C_Inferno"_hash:
		case "C_DecoyProjectile"_hash:
			return type::projectile;

		default:
			return type::unknown;
		}
	}

} // namespace systems



namespace systems {

	void view::update( std::uintptr_t view )
	{
		this->update_matrix( );

		if ( view )
		{
			this->m_origin = memory::read<math::vector3>( view + 0x0 );
			this->m_angles = memory::read<math::vector3>( view + 0xc );
			this->m_fov = memory::read<float>( view + 0x18 );
		}
		else
		{
			this->m_origin = { k_invalid, k_invalid, k_invalid };
			this->m_angles = { k_invalid, k_invalid, k_invalid };
			this->m_fov = k_invalid;
		}
	}

	void view::update_matrix( )
	{
		if ( !addresses::globals::view_matrix )
		{
			this->m_matrix_valid.store( false, std::memory_order_release );
			return;
		}

		const auto matrix = memory::read<math::matrix4x4>( addresses::globals::view_matrix );
		auto valid = true;
		auto magnitude = 0.0f;

		for ( auto row = 0; row < 4; ++row )
		{
			for ( auto column = 0; column < 4; ++column )
			{
				const auto value = matrix[ row ][ column ];
				valid = valid && std::isfinite( value );
				magnitude += std::fabsf( value );
			}
		}

		valid = valid && magnitude > 0.001f;
		if ( valid )
		{
			std::scoped_lock lock( this->m_matrix_mtx );
			this->m_matrix = matrix;
		}

		this->m_matrix_valid.store( valid, std::memory_order_release );
	}

	math::vector2 view::project( const math::vector3& world_pos )
	{
		if ( !this->has_camera( ) )
		{
			return { this->k_invalid, this->k_invalid };
		}

		const auto m = this->matrix( );

		const auto w = m[ 3 ][ 0 ] * world_pos.x + m[ 3 ][ 1 ] * world_pos.y + m[ 3 ][ 2 ] * world_pos.z + m[ 3 ][ 3 ];

		if ( w < 0.001f )
		{
			return { this->k_invalid, this->k_invalid };
		}

		const auto x = m[ 0 ][ 0 ] * world_pos.x + m[ 0 ][ 1 ] * world_pos.y + m[ 0 ][ 2 ] * world_pos.z + m[ 0 ][ 3 ];
		const auto y = m[ 1 ][ 0 ] * world_pos.x + m[ 1 ][ 1 ] * world_pos.y + m[ 1 ][ 2 ] * world_pos.z + m[ 1 ][ 3 ];

		const auto display = xdraw::viewport_size( );
		const auto inv_w = 1.0f / w;

		return
		{
			display.first * 0.5f * ( 1.0f + x * inv_w ),
			display.second * 0.5f * ( 1.0f - y * inv_w )
		};
	}

	view::projection view::project_full( const math::vector3& world_pos ) const
	{
		if ( !this->has_camera( ) )
		{
			return {};
		}

		const auto m = this->matrix( );

		const auto w = m[ 3 ][ 0 ] * world_pos.x + m[ 3 ][ 1 ] * world_pos.y + m[ 3 ][ 2 ] * world_pos.z + m[ 3 ][ 3 ];
		if ( w < 0.001f )
		{
			return {};
		}

		const auto x = m[ 0 ][ 0 ] * world_pos.x + m[ 0 ][ 1 ] * world_pos.y + m[ 0 ][ 2 ] * world_pos.z + m[ 0 ][ 3 ];
		const auto y = m[ 1 ][ 0 ] * world_pos.x + m[ 1 ][ 1 ] * world_pos.y + m[ 1 ][ 2 ] * world_pos.z + m[ 1 ][ 3 ];

		const auto [sw, sh] = xdraw::viewport_size( );
		const auto inv_w = 1.0f / w;

		const math::vector2 screen
		{
			sw * 0.5f * ( 1.0f + x * inv_w ),
			sh * 0.5f * ( 1.0f - y * inv_w )
		};

		projection r{};
		r.screen = screen;
		r.w = w;
		r.on_screen = w > 0.01f&& screen.x >= 0.0f && screen.x < static_cast< float >( sw )&& screen.y >= 0.0f && screen.y < static_cast< float >( sh );
		return r;
	}

	math::matrix4x4 view::matrix( ) const
	{
		std::scoped_lock lock( this->m_matrix_mtx );
		return this->m_matrix;
	}

} // namespace systems



namespace systems {

	namespace {
	constexpr std::size_t max_model_bones = 1024;
	constexpr std::size_t max_bone_name_buckets = 4096;

	// Source 2's bone-name tokens use case-insensitive MurmurHash2, rather
	// than the FNV schema hashes used elsewhere in this file.
	constexpr std::uint32_t bone_name_token(std::string_view name) noexcept {
		constexpr std::uint32_t multiplier = 0x5bd1e995u;
		auto lower = [](char value) constexpr -> std::uint32_t {
			return static_cast<unsigned char>(value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value);
		};
		std::uint32_t hash = 0x31415926u ^ static_cast<std::uint32_t>(name.size());
		std::size_t index{};
		for (; index + 4 <= name.size(); index += 4) {
			auto value = lower(name[index]) | (lower(name[index + 1]) << 8)
				| (lower(name[index + 2]) << 16) | (lower(name[index + 3]) << 24);
			value *= multiplier; value ^= value >> 24; value *= multiplier;
			hash *= multiplier; hash ^= value;
		}
		for (std::size_t tail{}; index + tail < name.size(); ++tail)
			hash ^= lower(name[index + tail]) << (tail * 8);
		if (index < name.size()) hash *= multiplier;
		hash ^= hash >> 13; hash *= multiplier; hash ^= hash >> 15;
		return hash;
	}
	static_assert(bone_name_token("head_0") == 1462898209u);
	static_assert(bone_name_token("clavicle_L") == bone_name_token("clavicle_l"));

	struct anatomical_bone { std::uint32_t slot, token; };
	constexpr std::array<anatomical_bone, 21> anatomical_bones{{
		{cstypes::bone_ids::pelvis, bone_name_token("pelvis")},
		{cstypes::bone_ids::spine_1, bone_name_token("spine_0")},
		{cstypes::bone_ids::spine_2, bone_name_token("spine_1")},
		{cstypes::bone_ids::spine_3, bone_name_token("spine_2")},
		{cstypes::bone_ids::spine_4, bone_name_token("spine_3")},
		{cstypes::bone_ids::neck, bone_name_token("neck_0")},
		{cstypes::bone_ids::head, bone_name_token("head_0")},
		{cstypes::bone_ids::left_clavicle, bone_name_token("clavicle_l")},
		{cstypes::bone_ids::left_shoulder, bone_name_token("arm_upper_l")},
		{cstypes::bone_ids::left_elbow, bone_name_token("arm_lower_l")},
		{cstypes::bone_ids::left_hand, bone_name_token("hand_l")},
		{cstypes::bone_ids::right_clavicle, bone_name_token("clavicle_r")},
		{cstypes::bone_ids::right_shoulder, bone_name_token("arm_upper_r")},
		{cstypes::bone_ids::right_elbow, bone_name_token("arm_lower_r")},
		{cstypes::bone_ids::right_hand, bone_name_token("hand_r")},
		{cstypes::bone_ids::left_hip, bone_name_token("leg_upper_l")},
		{cstypes::bone_ids::left_knee, bone_name_token("leg_lower_l")},
		{cstypes::bone_ids::left_foot, bone_name_token("ankle_l")},
		{cstypes::bone_ids::right_hip, bone_name_token("leg_upper_r")},
		{cstypes::bone_ids::right_knee, bone_name_token("leg_lower_r")},
		{cstypes::bone_ids::right_foot, bone_name_token("ankle_r")}
	}};

	bool readable_span(std::uintptr_t address, std::size_t length) noexcept {
		return address >= 0x10000 && length && address <= UINTPTR_MAX - (length - 1);
	}
	std::uintptr_t add_address(std::uintptr_t address, std::size_t offset) noexcept {
		return address >= 0x10000 && address <= UINTPTR_MAX - offset ? address + offset : 0;
	}
	struct bone_source {
		std::uintptr_t scene{}, state{}, cache{}, binding{}, model{};
		int count{}, model_count{};
		std::uint16_t cache_count{};
		bool operator==(const bone_source&) const = default;
	};
	bone_source read_bone_source(std::uintptr_t entity) {
		bone_source source;
		const auto scene_offset = SCHEMA("C_BaseEntity", "m_pGameSceneNode"_hash);
		const auto state_offset = SCHEMA("CSkeletonInstance", "m_modelState"_hash);
		if (!scene_offset || !state_offset) return source;
		source.scene = memory::read<std::uintptr_t>(add_address(entity, scene_offset));
		source.state = add_address(source.scene, state_offset);
		if (!source.state) return {};
		source.cache = memory::read<std::uintptr_t>(add_address(source.state, 0x80));
		if (const auto handle_offset = SCHEMA("CModelState", "m_hModel"_hash)) {
			source.binding = memory::read<std::uintptr_t>(add_address(source.state, handle_offset));
			source.model = memory::read<std::uintptr_t>(source.binding);
		}
		// Native CModel::GetBoneCount (client+17747B0) reads this field.
		// ModelState+88 is another pointer; +8C is its upper half, not a count.
		if (!source.model) return {};
		source.model_count = memory::read<int>(add_address(source.model, 0x178));
		// Native pose snapshot (client+AA0F88) copies this uint16 live count.
		source.cache_count = memory::read<std::uint16_t>(add_address(source.state, 0x94));
		if (source.model_count <= 0 || source.model_count > max_model_bones
			|| source.cache_count == 0 || source.cache_count > max_model_bones) return {};
		source.count = std::min(source.model_count, static_cast<int>(source.cache_count));
		if (!readable_span(source.cache, static_cast<std::size_t>(source.count) * sizeof(bones::data))) return {};
		return source;
	}

	// Audited native lookup at client.dll+17718A0 uses CModel+130. The
	// map stores a token and a uint16 model bone index in each 12-byte entry.
	struct bone_map_header {
		std::uint32_t grow{}, allocation{};
		std::uintptr_t entries{};
		std::int32_t active{}, buckets{};
		bool operator==(const bone_map_header&) const = default;
	};
	struct bone_map_entry { std::int32_t hash{}; std::uint32_t token{}; std::uint16_t index{}, padding{}; };
	static_assert(sizeof(bone_map_header) == 0x18 && sizeof(bone_map_entry) == 12);
	using bone_mapping = std::array<std::uint16_t, anatomical_bones.size()>;
	constexpr std::uint16_t missing_bone = UINT16_MAX;

	std::optional<bone_mapping> resolve_model_bones(const bone_source& source) {
		const auto map_address = add_address(source.model, 0x130);
		const auto header = memory::safe_read<bone_map_header>(map_address);
		if (!header || header->active <= 0 || header->buckets <= 0
			|| header->active > header->buckets || header->buckets > max_bone_name_buckets
			|| (header->buckets & (header->buckets - 1))
			|| (header->allocation & 0x7fffffffu) < static_cast<std::uint32_t>(header->buckets)) return std::nullopt;
		const auto bytes = static_cast<std::size_t>(header->buckets) * sizeof(bone_map_entry);
		if (!readable_span(header->entries, bytes)) return std::nullopt;
		std::array<bone_map_entry, max_bone_name_buckets> entries;
		if (!memory::copy(entries.data(), header->entries, bytes)
			|| memory::safe_read<bone_map_header>(map_address) != header) return std::nullopt;

		// Include the map contents in the cache key: a reloaded model can retain
		// its resource address and allocation while changing the skeleton order.
		struct cached_mapping {
			std::uintptr_t model{};
			int count{};
			bone_map_header header{};
			std::vector<bone_map_entry> entries;
			bone_mapping mapping{};
		};
		static thread_local std::array<cached_mapping, 32> cache;
		static thread_local std::size_t replacement{};
		for (const auto& cached : cache)
			if (cached.model == source.model && cached.count == source.count && cached.header == *header
				&& cached.entries.size() == static_cast<std::size_t>(header->buckets)
				&& std::memcmp(cached.entries.data(), entries.data(), bytes) == 0) return cached.mapping;

		bone_mapping mapping;
		mapping.fill(missing_bone);
		int active{};
		for (int index{}; index < header->buckets; ++index) {
			const auto& entry = entries[index];
			if (entry.hash < 0) continue;
			++active;
			if ((static_cast<std::uint32_t>(entry.hash) ^ entry.token) & 0x3fffffffu) continue;
			for (std::size_t bone{}; bone < anatomical_bones.size(); ++bone)
				if (entry.token == anatomical_bones[bone].token && entry.index < source.count) mapping[bone] = entry.index;
		}
		if (active != header->active) return std::nullopt;
		auto& destination = cache[replacement++ % cache.size()];
		destination.model = source.model; destination.count = source.count; destination.header = *header;
		destination.entries.assign(entries.begin(), entries.begin() + header->buckets);
		destination.mapping = mapping;
		return mapping;
	}
	}

	bones::data bones::get( std::uintptr_t entity, std::uint32_t bone_id )
	{
		data result{};

		const auto source = read_bone_source(entity);
		if ( !source.cache || bone_id >= static_cast<std::uint32_t>(source.count) )
		{
			return result;
		}

		const auto bone = memory::safe_read<data>(source.cache + static_cast<std::size_t>(bone_id) * sizeof(data));
		if (!bone || read_bone_source(entity) != source || !std::isfinite(bone->position.x) ||
			!std::isfinite(bone->position.y) || !std::isfinite(bone->position.z)) return result;
		result.position = bone->position;
		result.scale = bone->scale;
		result.rotation = bone->rotation;

		return result;
	}

	std::array<bones::data, 27> bones::get_skeleton( std::uintptr_t entity )
	{
		return get_skeleton(entity, false);
	}
	std::array<bones::data, 27> bones::get_skeleton( std::uintptr_t entity, bool named_only )
	{
		std::array<data, 27> skeleton{};
		const auto source = read_bone_source(entity);
		if (!source.cache) return skeleton;
		const auto mapping = resolve_model_bones(source);
		if (named_only && !mapping) return skeleton;
		std::array<data, max_model_bones> snapshot;
		if (!memory::copy(snapshot.data(), source.cache, static_cast<std::size_t>(source.count) * sizeof(data))
			|| read_bone_source(entity) != source) return skeleton;
		if (named_only && resolve_model_bones(source) != mapping) return skeleton;

		for (std::size_t bone{}; bone < anatomical_bones.size(); ++bone) {
			const auto slot = anatomical_bones[bone].slot;
			const auto native_index = mapping ? (*mapping)[bone] : slot;
			if (native_index >= static_cast<std::uint32_t>(source.count)) continue;
			const auto& value = snapshot[native_index];
			if (!std::isfinite(value.position.x) || !std::isfinite(value.position.y) || !std::isfinite(value.position.z)) continue;
			skeleton[slot] = value;
		}

		return skeleton;
	}

	std::uintptr_t bones::get_bone_cache( std::uintptr_t entity, int* out_count )
	{
		const auto source = read_bone_source(entity);
		if (out_count) *out_count = source.count;
		return source.cache;
	}

} // namespace systems


namespace systems {

	bounds::data bounds::get( std::uintptr_t entity )
	{
		const auto collision = memory::read<std::uintptr_t>( entity + SCHEMA( "C_BaseEntity", "m_pCollision"_hash ) );
		const auto game_scene_node = memory::read<std::uintptr_t>( entity + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );

		if ( !collision || !game_scene_node )
		{
			return {};
		}

		// m_vecOrigin is a quantized network-origin type in current CS2 builds,
		// not a plain vector3. Use the evaluated world-space origin instead.
		const auto origin = memory::read<math::vector3>( game_scene_node + SCHEMA( "CGameSceneNode", "m_vecAbsOrigin"_hash ) );
		const auto mins = memory::read<math::vector3>( collision + SCHEMA( "CCollisionProperty", "m_vecMins"_hash ) ) + origin;
		const auto maxs = memory::read<math::vector3>( collision + SCHEMA( "CCollisionProperty", "m_vecMaxs"_hash ) ) + origin;

		const auto [viewport_w, viewport_h] = xdraw::viewport_size( );
		const auto sw = static_cast< float >( viewport_w );
		const auto sh = static_cast< float >( viewport_h );

		data result{};
		result.min = { FLT_MAX, FLT_MAX };
		result.max = { -FLT_MAX, -FLT_MAX };

		constexpr auto edge_margin = 384.0f;
		auto had_valid_projection = false;

		for ( std::size_t i = 0; i < 8; ++i )
		{
			const math::vector3 corner
			{
				( i & 1 ) ? maxs.x : mins.x,
				( i & 2 ) ? maxs.y : mins.y,
				( i & 4 ) ? maxs.z : mins.z
			};

			const auto proj = g_view.project_full( corner );
			if ( proj.w <= 0.001f || !std::isfinite( proj.screen.x ) || !std::isfinite( proj.screen.y ) )
			{
				continue;
			}

			const auto sx = std::clamp( proj.screen.x, -edge_margin, sw + edge_margin );
			const auto sy = std::clamp( proj.screen.y, -edge_margin, sh + edge_margin );

			result.min.x = std::min( result.min.x, sx );
			result.min.y = std::min( result.min.y, sy );
			result.max.x = std::max( result.max.x, sx );
			result.max.y = std::max( result.max.y, sy );
			had_valid_projection = true;
		}

		if ( !had_valid_projection || result.min.x >= result.max.x || result.min.y >= result.max.y )
		{
			return {};
		}

		result.valid = true;
		return result;
	}

} // namespace systems



namespace systems {

	bool tracing::is_visible( const math::vector3& start, const math::vector3& end, std::uintptr_t target_entity, std::uintptr_t skip_entity, std::uintptr_t mask ) const
	{
		auto current_start = start;
		auto entity_to_skip = skip_entity;

		constexpr auto max_penetrations{ 3 };

		for ( auto i = 0; i < max_penetrations; ++i )
		{
			const auto result = this->trace( current_start, end, entity_to_skip, mask );

			if ( !result.all_solid && (result.hit_entity == target_entity || result.fraction > 0.97f) )
			{
				return true;
			}

			if ( !result.hit_entity )
			{
				break;
			}

			const auto hit_health = memory::read<int>( result.hit_entity + SCHEMA( "C_BaseEntity", "m_iHealth"_hash ) );
			if ( hit_health > 0 && hit_health <= 100 )
			{
				entity_to_skip = result.hit_entity;
				current_start = result.end_pos + ( end - current_start ).normalized( );
				continue;
			}

			break;
		}

		return false;
	}

	tracing::result tracing::trace( const math::vector3& start, const math::vector3& end, std::uintptr_t skip_entity, std::uintptr_t mask, std::uint8_t layer ) const
	{
		auto filter = this->make_filter( skip_entity, mask, layer );
		return this->trace( start, end, filter );
	}

	tracing::result tracing::trace( const math::vector3& start, const math::vector3& end, const filter& filter ) const
	{
        result failed{};failed.all_solid=true;failed.start_pos=failed.end_pos=failed.position=start;
        const auto finite=[](const math::vector3& p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);};
        if(!finite(start)||!finite(end)||!filter.vtable)return failed;
        const auto slot=addresses::globals::game_trace_manager_slot;
        const auto manager=memory::read<std::uintptr_t>(slot),function=PATTERN(patterns::trace_ray);
        if(!manager||!function)return failed;
        ray ray{};result hit{};hit.fraction=std::numeric_limits<float>::quiet_NaN();
        __try {
            // A clear native ray may return false; validate its output instead.
            memory::call<bool>(function,manager,&ray,&start,&end,&filter,&hit);
        } __except(EXCEPTION_EXECUTE_HANDLER) {return failed;}
        if(memory::read<std::uintptr_t>(slot)!=manager||!std::isfinite(hit.fraction)||hit.fraction<0||hit.fraction>1||
           !finite(hit.end_pos)||!finite(hit.normal))return failed;
        return hit;
	}

	tracing::filter tracing::make_filter( std::uintptr_t skip_entity, std::uintptr_t mask, std::uint8_t layer ) const
	{
		filter filter{};

        const auto function=PATTERN(patterns::trace_filter_init);
        if(!function)return filter;
        __try {memory::call<void>(function,&filter,skip_entity,mask,layer);}
        __except(EXCEPTION_EXECUTE_HANDLER) {return {};}

		return filter;
	}

}



namespace {
std::atomic<bool> ready{};
std::recursive_mutex capture_mutex;
bool trace_ready{};
std::string state="Waiting for CS2 modules";
std::atomic<ULONGLONG> hit_time{};
ULONGLONG refresh_time{};
std::uintptr_t last_list{};
struct trail { std::vector<math::vector3> points; ULONGLONG seen{}; };
std::unordered_map<std::uint32_t,trail> trails;

template<class T> T field(std::uintptr_t object,const char* type,const char* name,T fallback={}) {
    if (!object) return fallback;
    const auto offset=systems::schemas::lookup(type,fnv1a::runtime_hash(name));
    if (!offset) return fallback;
    return memory::safe_read<T>(object+offset).value_or(fallback);
}
bool position(std::uintptr_t entity,math::vector3& result) {
    const auto node=field<std::uintptr_t>(entity,"C_BaseEntity","m_pGameSceneNode");
    if (!node) return false;
    result=field<math::vector3>(node,"CGameSceneNode","m_vecAbsOrigin");
    return std::isfinite(result.x) && std::isfinite(result.y) && std::isfinite(result.z);
}
visuals::screen_point project(const math::vector3& value) {
    const auto point=systems::g_view.project_full(value);
    return {point.screen.x,point.screen.y,point.w>0.001f && std::isfinite(point.screen.x) && std::isfinite(point.screen.y),point.on_screen};
}
std::string entity_name(std::uintptr_t entity) {
    const auto identity=memory::read<std::uintptr_t>(entity+0x10);
    auto result=memory::read_string(memory::read<std::uintptr_t>(identity+0x20),64);
    if (result.empty()) result=memory::read_string(reinterpret_cast<std::uintptr_t>(systems::g_entities.get_schema_name(entity)),64);
    if (result.starts_with("weapon_")) result.erase(0,7);
    if (result.starts_with("C_Weapon")) result.erase(0,8);
    else if (result.starts_with("C_")) result.erase(0,2);
    return result;
}
std::string schema_name(std::uintptr_t entity) {
    return memory::read_string(reinterpret_cast<std::uintptr_t>(systems::g_entities.get_schema_name(entity)),64);
}
visuals::screen_point direction(const math::vector3& value,float width,float height) {
    const auto matrix=systems::g_view.matrix();
    const float x=matrix[0][0]*value.x+matrix[0][1]*value.y+matrix[0][2]*value.z+matrix[0][3];
    const float y=matrix[1][0]*value.x+matrix[1][1]*value.y+matrix[1][2]*value.z+matrix[1][3];
    return {width*0.5f+x,height*0.5f-y,std::isfinite(x) && std::isfinite(y),false};
}
}

namespace game {
bool module_matches_handle(std::uintptr_t module,std::string_view expected_hash) {
    return native_startup::MatchesModule(module,expected_hash);
}
bool module_matches(const char* module,std::string_view expected_hash) {
    return module&&module_matches_handle(reinterpret_cast<std::uintptr_t>(GetModuleHandleA(module)),expected_hash);
}

bool initialize() {
    using namespace addresses::globals;
    if (!module_matches("client.dll",hvh::client_sha256)) {state="Unsupported client build: refresh HVH bindings";return false;}
    const auto create_interface=reinterpret_cast<void*(__cdecl*)(const char*,int*)>(GetProcAddress(GetModuleHandleA("schemasystem.dll"),"CreateInterface"));
    schema_system=create_interface ? reinterpret_cast<std::uintptr_t>(create_interface("SchemaSystem_001",nullptr)) : 0;
    const auto client=reinterpret_cast<std::uintptr_t>(GetModuleHandleA("client.dll"));
    // HVH's verified entity-list slot supersedes the older signature, which
    // resolves to another global.
    entity_list=client+cs2_dumper::offsets::client_dll::dwEntityList;
    local_player_controller=client+cs2_dumper::offsets::client_dll::dwLocalPlayerController;
    view_matrix=PATTERN(patterns::view_matrix);
    global_vars=PATTERN(patterns::global_vars);
    game_trace_manager_slot=PATTERN(patterns::game_trace_manager);
    planted_c4=PATTERN(patterns::planted_c4);
    trace_ready=game_trace_manager_slot && PATTERN(patterns::trace_ray) && PATTERN(patterns::trace_filter_init);
    ready=schema_system && view_matrix;
    for (const auto& requirement:std::array<std::pair<const char*,const char*>,9>{{
        {"CBasePlayerController","m_hPawn"},{"C_BaseEntity","m_iHealth"},
        {"C_BaseEntity","m_iTeamNum"},{"C_BaseEntity","m_pGameSceneNode"},
        {"CGameSceneNode","m_vecAbsOrigin"},{"C_BaseEntity","m_pCollision"},
        {"CCollisionProperty","m_vecMins"},{"CCollisionProperty","m_vecMaxs"},
        {"CSkeletonInstance","m_modelState"}}})
        ready=ready && systems::schemas::lookup(requirement.first,fnv1a::runtime_hash(requirement.second));
    state=ready ? "HVH SDK connected" : "Required client schema or signature unavailable";
    return ready;
}
const std::string& status() {return state;}
bool in_world() noexcept {
    return ready && memory::read<std::uintptr_t>(addresses::globals::entity_list)
        && memory::read<std::uintptr_t>(addresses::globals::local_player_controller);
}
void reset() {std::scoped_lock lock(capture_mutex);trails.clear();refresh_time=0;last_list=0;hit_time.store(0);}

void report_hit(std::uintptr_t message) {
    // Capture the contact point before the deleting destructor runs, as in HVH.
    const auto position=memory::read<math::vector3>(message+0x18);
    if (std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z)
        && position.length_sqr()>1.0f) hit_time.store(GetTickCount64());
}

visuals::frame_snapshot capture(float width,float height) {
    std::scoped_lock lock(capture_mutex);
    visuals::frame_snapshot frame{};
    frame.screen_width=width; frame.screen_height=height;
    if (!ready || width<=0 || height<=0) return frame;
    const auto list=memory::read<std::uintptr_t>(addresses::globals::entity_list);
    const auto controller=memory::read<std::uintptr_t>(addresses::globals::local_player_controller);
    if (!list || !controller) {reset();return frame;}
    const auto pawn=systems::g_entities.lookup(field<std::uint32_t>(controller,"CBasePlayerController","m_hPawn",0xffffffff));
    if (!pawn) {reset();return frame;}
    auto view_pawn=pawn;
    if (field<int>(pawn,"C_BaseEntity","m_iHealth")<=0) {
        const auto observer=systems::g_entities.lookup(field<std::uint32_t>(controller,"CCSPlayerController","m_hObserverPawn",0xffffffff));
        const auto services=field<std::uintptr_t>(observer,"C_BasePlayerPawn","m_pObserverServices");
        const auto target=systems::g_entities.lookup(field<std::uint32_t>(services,"CPlayer_ObserverServices","m_hObserverTarget",0xffffffff));
        if (target) view_pawn=target;
    }
    math::vector3 origin{};
    if (!position(view_pawn,origin)) return frame;
    const auto eye=origin+field<math::vector3>(view_pawn,"C_BaseModelEntity","m_vecViewOffset");
    frame.local_team=field<std::uint8_t>(view_pawn,"C_BaseEntity","m_iTeamNum");
    const auto velocity=field<math::vector3>(pawn,"C_BaseEntity","m_vecAbsVelocity");
    frame.local_speed=velocity.length_2d();
    systems::g_view.update_matrix();
    if (!systems::g_view.has_camera()) return frame;
    const auto now=GetTickCount64();
    if (list!=last_list || now-refresh_time>=500) {
        if (list!=last_list) trails.clear();
        systems::g_entities.force_update();refresh_time=now;last_list=list;
    }
    const auto matrix=systems::g_view.matrix();
    const float projection_scale=std::sqrt(matrix[0][0]*matrix[0][0]+matrix[0][1]*matrix[0][1]+matrix[0][2]*matrix[0][2]);
    frame.horizontal_fov=projection_scale>.001f ? 2*std::atan(1/projection_scale)*180/std::numbers::pi_v<float>:90;
    frame.show_fov_guide=true;
    frame.fov_radius=width*0.5f*projection_scale*std::tan(visuals::g_settings.guide_degrees*std::numbers::pi_v<float>/180.0f);
    const auto hit=hit_time.load();
    frame.hit_marker_time=hit && now>=hit ? std::max(0.0f,0.25f-static_cast<float>(now-hit)/1000.0f) : 0;
    frame.show_hit_marker=frame.hit_marker_time>0;

    constexpr std::array<std::pair<unsigned,unsigned>,20> connections{{
        {1,2},{2,3},{3,4},{4,23},{23,6},{6,7},{6,8},{8,9},{9,10},{10,11},
        {6,12},{12,13},{13,14},{14,15},{1,17},{17,18},{18,19},{1,20},{20,21},{21,22}}};
    for (const auto& cached:systems::g_entities.get_by_type(systems::entities::type::player)) {
        if (systems::g_entities.get_by_index(cached.index)!=cached.ptr) continue;
        const auto player_pawn=systems::g_entities.lookup(field<std::uint32_t>(cached.ptr,"CBasePlayerController","m_hPawn",0xffffffff));
        if (!player_pawn || player_pawn==view_pawn) continue;
        visuals::player_snapshot player{};
        player.health=field<int>(player_pawn,"C_BaseEntity","m_iHealth");
        player.alive=player.health>0 && field<std::uint8_t>(player_pawn,"C_BaseEntity","m_lifeState",255)==0;
        if (!player.alive) continue;
        math::vector3 player_origin{};
        if (!position(player_pawn,player_origin)) continue;
        player.distance_meters=origin.distance(player_origin)*0.01905f;
        if (player.distance_meters>visuals::g_settings.max_distance_meters) continue;
        player.team=field<std::uint8_t>(player_pawn,"C_BaseEntity","m_iTeamNum");
        player.enemy=player.team!=frame.local_team;
        player.armor=field<int>(player_pawn,"C_CSPlayerPawn","m_ArmorValue");
        player.name=memory::read_string(field<std::uintptr_t>(cached.ptr,"CCSPlayerController","m_sSanitizedPlayerName"),128);
        const auto node=field<std::uintptr_t>(player_pawn,"C_BaseEntity","m_pGameSceneNode");
        player.dormant=field<bool>(node,"CGameSceneNode","m_bDormant");
        const auto box=systems::g_bounds.get(player_pawn);
        player.bounds={box.min.x,box.min.y,box.max.x,box.max.y,box.valid && box.max.x>=0 && box.min.x<=width && box.max.y>=0 && box.min.y<=height};
        const auto bones=systems::g_bones.get_skeleton(player_pawn);
        auto head=bones[cstypes::bone_ids::head].position;
        if (head.length_sqr()<0.001f) head=player_origin+math::vector3{0,0,64};
        player.head=project(head);player.feet=project(player_origin);
        player.offscreen_direction=direction(player_origin,width,height);
        player.visible=trace_ready && !player.dormant && systems::g_tracing.is_visible(eye,head,player_pawn,view_pawn);
        if (visuals::g_settings.skeleton)
            for (const auto [from,to]:connections)
                if (bones[from].position.length_sqr()>0.001f && bones[to].position.length_sqr()>0.001f)
                    player.skeleton.push_back({project(bones[from].position),project(bones[to].position)});
        const auto services=field<std::uintptr_t>(player_pawn,"C_BasePlayerPawn","m_pWeaponServices");
        const auto weapon=systems::g_entities.lookup(field<std::uint32_t>(services,"CPlayer_WeaponServices","m_hActiveWeapon",0xffffffff));
        if (weapon){
            player.weapon=entity_name(weapon);
            player.ammo=field<int>(weapon,"C_BasePlayerWeapon","m_iClip1",-1);
            const auto data=memory::read<std::uintptr_t>(weapon+game_access::field::C_BaseEntity::m_nSubclassID+8);
            player.max_ammo=field<int>(data,"CBasePlayerWeaponVData","m_iMaxClip1");
        }
        if (field<bool>(player_pawn,"C_CSPlayerPawn","m_bIsScoped")) player.flags.emplace_back("SCOPED");
        if (field<bool>(player_pawn,"C_CSPlayerPawn","m_bIsDefusing")) player.flags.emplace_back("DEFUSING");
        if (field<float>(player_pawn,"C_CSPlayerPawnBase","m_flFlashBangTime")>0) player.flags.emplace_back("FLASHED");
        const auto items=field<std::uintptr_t>(player_pawn,"C_BasePlayerPawn","m_pItemServices");
        if (field<bool>(items,"CCSPlayer_ItemServices","m_bHasHelmet")) player.flags.emplace_back("HELMET");
        if (field<bool>(items,"CCSPlayer_ItemServices","m_bHasDefuser")) player.flags.emplace_back("KIT");
        frame.players.emplace_back(std::move(player));
    }
    const auto globals=memory::read<std::uintptr_t>(addresses::globals::global_vars);
    const auto current_time=memory::read<float>(globals+0x30);
    for (const auto& cached:systems::g_entities.get_by_type(systems::entities::type::item)) {
        if (systems::g_entities.get_by_index(cached.index)!=cached.ptr) continue;
        const auto type=schema_name(cached.ptr);
        const bool planted=type=="C_PlantedC4";
        if (!planted && systems::g_entities.lookup(field<std::uint32_t>(cached.ptr,"C_BaseEntity","m_hOwnerEntity",0xffffffff))) continue;
        math::vector3 point{}; if (!position(cached.ptr,point)) continue;
        visuals::world_entity_snapshot item{};
        item.label=entity_name(cached.ptr); item.position=project(point);
        item.distance_meters=origin.distance(point)*0.01905f;
        item.kind=(type=="C_Hostage" || type=="C_Chicken") ? visuals::world_entity_kind::other :
            (planted || type=="C_C4") ? visuals::world_entity_kind::objective :
            type.find("Grenade")!=std::string::npos || type=="C_Flashbang" || type=="C_DecoyGrenade" ? visuals::world_entity_kind::grenade : visuals::world_entity_kind::weapon;
        if (planted) {
            if (!field<bool>(cached.ptr,"C_PlantedC4","m_bBombTicking") || field<bool>(cached.ptr,"C_PlantedC4","m_bBombDefused")) continue;
            item.label="C4";item.has_timer=globals!=0;
            item.timer_seconds=std::max(0.0f,field<float>(cached.ptr,"C_PlantedC4","m_flC4Blow")-current_time);
            frame.bomb_planted=true;frame.bomb_time_remaining=item.timer_seconds;
            if (field<bool>(cached.ptr,"C_PlantedC4","m_bBeingDefused")) item.detail="DEFUSING";
        }
        frame.entities.emplace_back(std::move(item));
    }
    for (const auto& cached:systems::g_entities.get_by_type(systems::entities::type::projectile)) {
        if (systems::g_entities.get_by_index(cached.index)!=cached.ptr) continue;
        math::vector3 point{}; if (!position(cached.ptr,point)) continue;
        const auto identity=memory::read<std::uintptr_t>(cached.ptr+0x10);
        const auto handle=memory::read<std::uint32_t>(identity+0x10);
        auto& history=trails[handle]; history.seen=now;
        if (history.points.empty() || history.points.back().distance_sqr(point)>4.0f) history.points.push_back(point);
        if (history.points.size()>96) history.points.erase(history.points.begin());
        visuals::projectile_snapshot projectile{};
        projectile.label=entity_name(cached.ptr);projectile.position=project(point);
        for (const auto& sample:history.points) projectile.trajectory.push_back(project(sample));
        const auto type=schema_name(cached.ptr);
        if (type=="C_SmokeGrenadeProjectile") {
            const auto tick=field<int>(cached.ptr,"C_SmokeGrenadeProjectile","m_nSmokeEffectTickBegin");
            if (tick>0 && globals) {projectile.has_timer=true;projectile.time_remaining=std::max(0.0f,18.0f+tick*cstypes::tick_interval-current_time);}
        }
        frame.projectiles.emplace_back(std::move(projectile));
    }
    std::erase_if(trails,[now](const auto& entry){return now-entry.second.seen>1000;});
    return frame;
}
}
