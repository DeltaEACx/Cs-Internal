#include "features/visuals/engine.hpp"
#include "world.hpp"

namespace custom::world {
namespace {
constexpr sky_lighting_profile crimson_dawn_lighting{{1.0f,0.79f,0.62f},1.15f,{0.63f,0.60f,0.57f}};
constexpr std::array presets{
    skybox_entry{"Map default",""},
    skybox_entry{"Vertigo","materials/skybox/sky_de_vertigo_exr_c70a3937.vtex"},
    skybox_entry{"Mirage","materials/skybox/sky_de_mirage_exr_71e5f2a1.vtex"},
    skybox_entry{"Dust 2","materials/skybox/sky_de_dust2_exr_908a35ba.vtex"},
    skybox_entry{"Nuke","materials/skybox/sky_de_nuke_exr_f04e84b2.vtex"},
    skybox_entry{"Anubis","materials/skybox/sky_de_annubis_exr_2c5e0b53.vtex"},
    skybox_entry{"Overpass","materials/skybox/sky_de_overpass_01_exr_f8534391.vtex"},
    skybox_entry{"Train","materials/skybox/sky_de_train03_exr_4fdb8a38.vtex"},
    skybox_entry{"Aztec","materials/skybox/sky_hr_aztec_02_exr_f84f8de9.vtex"},
    skybox_entry{"Italy","materials/skybox/cs_italy_s2_skybox_sunset_2_exr_e56cedf6.vtex"},
    skybox_entry{"Office","materials/skybox/sky_cs_office_45_0_exr_d0152542.vtex"},
    skybox_entry{"Cloudy","materials/skybox/sky_csgo_cloudy01_cube_pfm_f9a0b177.vtex"},
    skybox_entry{"Rain Night","materials/skybox/sky_rain_night_01_exr_8d775aee.vtex"},
    skybox_entry{"Daylight","materials/skybox/sky_cs15_daylight01_hdr_cube_pfm_a4b050d1.vtex"},
    skybox_entry{"Jungle","materials/skybox/jungle_cube_pfm_bc16d813.vtex"},
    skybox_entry{"Sunset","materials/skybox/tests/src/lightingtest_sky_sunset_light_exr_f7b19a45.vtex"},
    skybox_entry{"Portrait","materials/custom/skybox/portrait_sky.vtex"},
    skybox_entry{"Crimson Dawn","materials/custom/skybox/crimson_dawn_sky.vtex",true,&crimson_dawn_lighting}
};
constexpr std::string_view scene_hash="040eb851fa7875523346b890f1ed6e158abc59e08bccbd8dc60539435277efa5";
constexpr std::string_view resource_hash="0004439e37b6ffd5a37683f9f288726584be2a3b4ccc6674af8303cde0343027";
constexpr std::string_view material_hash="b4d5c667eb65f13d9193d743a88eb1f9dfa623cc8f745cef764d9977969e38b6";
constexpr std::string_view tier0_hash="990ea56bf1626d299ba54b64b423f2e409bd592bb737c7a5edf7972ad5c7143e";
constexpr std::string_view engine_hash="6456df18d8f954394024c2c07da8c848df15a5ace74ce7390d78970ceb39ee21";
constexpr std::uint32_t gradient_fog=0x4B01FF63,gradient_fog_2=0x0AA49C2A,gradient_fog_3=0xFBF6448D;
constexpr std::uint32_t enable_gradient_fog=0x6E0FAD7E;

// One recursive lock also permits a sky callback to enter a light callback (or
// the reverse) without introducing a cross-lock ordering dependency.
std::recursive_mutex mutation_mutex;
std::mutex resource_mutex;
std::atomic<bool> scene_supported{},shader_supported{},resources_ready{};
std::atomic<std::uintptr_t> active_material{};
std::string load_state="World bindings not initialized",selected_path;
bool selection_recorded{};
struct resource_bindings {
    std::uintptr_t system{},init_path{},precache{},kv3_alloc{},kv3_destroy{},kv3_load{},material_create{};
} bindings;
struct sky_material {
    std::uintptr_t texture_binding{},material_binding{},material{};
};
// Native handles retain the generated material and its cubemap between maps.
// The engine owns their allocation; no unverified release ABI is invoked.
std::unordered_map<std::string,sky_material> materials;
constexpr std::size_t max_materials=32;
const auto material_session=GetTickCount64();
std::uint64_t next_material_id{}; // Protected by resource_mutex; never reset.

float bounded(float value,float fallback,float minimum,float maximum) noexcept {
    return std::isfinite(value) ? std::clamp(value,minimum,maximum) : fallback;
}
std::array<float,3> tint(const std::array<float,3>& color,float intensity) noexcept {
    const auto strength=bounded(intensity,1.0f,0.0f,10.0f);
    return {bounded(color[0],1.0f,0.0f,1.0f)*strength,
        bounded(color[1],1.0f,0.0f,1.0f)*strength,
        bounded(color[2],1.0f,0.0f,1.0f)*strength};
}
bool finite_color(const std::array<float,3>& color) noexcept {
    return std::ranges::all_of(color,[](float item){return std::isfinite(item);});
}
std::array<float,3> rainbow(float speed) noexcept {
    const auto seconds=static_cast<double>(GetTickCount64())*0.001;
    const auto phase=static_cast<float>(std::fmod(seconds*bounded(speed,0.2f,0.01f,3.0f),1.0));
    std::array<float,3> result{};
    for (std::size_t i=0;i<result.size();++i) {
        const auto position=std::fmod(phase*6.0f+static_cast<float>(i==0 ? 0 : i==1 ? 4 : 2),6.0f);
        result[i]=std::clamp(std::abs(position-3.0f)-1.0f,0.0f,1.0f);
    }
    return result;
}
bool valid_texture(std::string_view path) noexcept {
    // CUtlBufferString's known inline capacity is 0xC8. Keep paths inline so
    // this integration needs no speculative heap-buffer destructor.
    if (path.empty() || path.size()>=0xC8 || !path.starts_with("materials/")
        || !path.ends_with(".vtex") || path.find("..")!=std::string_view::npos) return false;
    return std::ranges::none_of(path,[](unsigned char c){
        return c<0x20 || c=='"' || c=='\'' || c=='\\' || c==':';
    });
}

struct path_buffer {
    std::uint32_t unknown1{},flags{0xc00000c8};
    std::array<std::uint8_t,0xc8> data{};
    std::uintptr_t unknown3{},extension{};
};
static_assert(sizeof(path_buffer)==0xe0);

bool preload_texture(const resource_bindings& api,const char* path,std::uintptr_t& binding) noexcept {
    __try {
        path_buffer buffer{};
        if (!memory::call<bool>(api.init_path,&buffer,path)) return false;
        memory::call<void>(api.precache,api.system,&buffer,"");
        if (!memory::call<bool>(api.init_path,&buffer,path)) return false;
        binding=memory::call_vfunc<std::uintptr_t>(api.system,79,&buffer,0ll);
        return binding && memory::read<std::uintptr_t>(binding);
    } __except(EXCEPTION_EXECUTE_HANDLER) {binding=0;return false;}
}
void destroy_parser(std::uintptr_t function,cstypes::key_values3* value) noexcept {
    __try {memory::call<void>(function,value,0u);}
    __except(EXCEPTION_EXECUTE_HANDLER) {}
}
bool create_material(const resource_bindings& api,const char* source,const char* name,sky_material& output) noexcept {
    constexpr cstypes::kv3_id id{"generic",0x41B818518343427E,0xB5F447C23C0CDF8C};
    cstypes::key_values3 tree{};
    cstypes::strong_handle handle{};
    bool allocated{},loaded{};
    __try {
        allocated=memory::call<cstypes::key_values3*>(api.kv3_alloc,&tree,1u,6u)==&tree;
        if (allocated) {
            loaded=memory::call<bool>(api.kv3_load,&tree,nullptr,source,&id,nullptr,0u);
            if (loaded) memory::call<void*>(api.material_create,nullptr,&handle,name,&tree,0,true);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {loaded=false;}
    if (allocated) destroy_parser(api.kv3_destroy,&tree);
    output.material_binding=reinterpret_cast<std::uintptr_t>(handle.binding);
    output.material=loaded ? memory::read<std::uintptr_t>(output.material_binding) : 0;
    return output.material!=0;
}

std::uintptr_t fog_enable_slot(std::uintptr_t output) {
    const auto inline_map=output+17*16;
    for (unsigned i=0;i<12;++i) {
        if (memory::read<std::uint32_t>(inline_map+i*4)==enable_gradient_fog) return inline_map+48+i*4;
    }
    auto node=memory::read<std::uintptr_t>(inline_map+104);
    for (unsigned nodes=0;node && nodes<64;++nodes) {
        for (unsigned i=0;i<12;++i) {
            if (memory::read<std::uint32_t>(node+16+i*4)==enable_gradient_fog) return node+64+i*4;
        }
        const auto next=memory::read<std::uintptr_t>(node);
        if (next==node) break;
        node=next;
    }
    return 0;
}
std::uint64_t integer_hash(std::uint32_t key,std::uint32_t value) noexcept {
    auto bits=(static_cast<std::uint64_t>(key)<<32)|value;
    bits=(bits^(bits>>30))*0xBF58476D1CE4E5B9ull;
    bits=(bits^(bits>>27))*0x94D049BB133111EBull;
    return bits^(bits>>31);
}
bool set_fog_parameters(set_shader_fn setter,void* output,const shader_vector* vectors) noexcept {
    __try {
        setter(output,gradient_fog,&vectors[0]);
        setter(output,gradient_fog_2,&vectors[1]);
        setter(output,gradient_fog_3,&vectors[2]);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
}

std::span<const skybox_entry> skyboxes() noexcept {return presets;}
bool sky_lighting_available(const configuration& value) noexcept {
    return value.skybox && value.sky_texture.front()=='\0' && value.skybox_index>=0
        && value.skybox_index<static_cast<int>(presets.size()) && presets[value.skybox_index].lighting;
}
bool sky_lighting_active(const configuration& value) noexcept {
    return value.enabled && value.match_sky_lighting && sky_lighting_available(value);
}
configuration effective_configuration(configuration value) noexcept {
    if (!sky_lighting_active(value)) return value;
    const auto& profile=*presets[value.skybox_index].lighting;
    value.lighting=true;
    value.rainbow_lighting=false;
    value.lighting_color=profile.light_color;
    value.lighting_intensity=profile.light_intensity;
    if(value.fog)value.fog_color=profile.fog_color;
    return value;
}
bool skybox_ready() noexcept {return resources_ready.load();}
bool lighting_ready() noexcept {return scene_supported.load();}
bool shaders_ready() noexcept {return shader_supported.load();}
std::string active_sky_path() {std::scoped_lock lock(resource_mutex);return active_material.load()?selected_path:std::string{};}
std::uintptr_t texture_binding(std::string_view path) {
    std::scoped_lock lock(resource_mutex);
    if(!resources_ready||!valid_texture(path))return 0;
    static std::unordered_map<std::string,std::uintptr_t> textures;
    const std::string key(path);if(auto found=textures.find(key);found!=textures.end())return found->second;
    if(textures.size()>=32)return 0;std::uintptr_t binding{};
    if(!preload_texture(bindings,key.c_str(),binding))return 0;
    textures.emplace(key,binding);return binding;
}
std::string status() {std::scoped_lock lock(resource_mutex);return load_state;}

bool initialize() {
    std::scoped_lock lock(resource_mutex);
    active_material.store(0);selected_path.clear();selection_recorded=false;bindings={};
    const bool client=game::module_matches("client.dll",hvh::client_sha256);
    scene_supported.store(client && game::module_matches("scenesystem.dll",scene_hash));
    shader_supported.store(client && game::module_matches("engine2.dll",engine_hash));
    resources_ready.store(false);
    if (!scene_supported.load()) {load_state="Sky / lighting: unsupported scene build";return shader_supported.load();}
    if (!game::module_matches("resourcesystem.dll",resource_hash)
        || !game::module_matches("materialsystem2.dll",material_hash)
        || !game::module_matches("tier0.dll",tier0_hash)) {
        load_state="Skybox loader: unsupported resource build";return true;
    }
    const auto create_interface=reinterpret_cast<void*(__cdecl*)(const char*,int*)>(
        GetProcAddress(GetModuleHandleA("resourcesystem.dll"),"CreateInterface"));
    bindings.system=create_interface ? reinterpret_cast<std::uintptr_t>(create_interface("ResourceSystem013",nullptr)) : 0;
    bindings.init_path=PATTERN(patterns::init_particle_path_buffer);
    bindings.precache=PATTERN(patterns::resource_system_precache);
    bindings.kv3_alloc=PATTERN(patterns::kv3_alloc);
    bindings.kv3_destroy=PATTERN(patterns::kv3_destroy);
    bindings.material_create=PATTERN(patterns::material_create);
    bindings.kv3_load=reinterpret_cast<std::uintptr_t>(GetProcAddress(GetModuleHandleA("tier0.dll"),
        "?LoadKV3@@YA_NPEAVKeyValues3@@PEAVCUtlString@@PEBDAEBUKV3ID_t@@2I@Z"));
    const auto table=memory::read<std::uintptr_t>(bindings.system);
    const auto texture_loader=memory::read<std::uintptr_t>(table+79*sizeof(void*));
    const bool ready=bindings.system && bindings.init_path && bindings.precache && bindings.kv3_alloc
        && bindings.kv3_destroy && bindings.kv3_load && bindings.material_create && texture_loader;
    resources_ready.store(ready);
    load_state=ready ? (shader_supported.load() ? "Native sky / lighting / shaders ready"
        : "Native sky / lighting ready; unsupported shader build") : "Skybox loader: required binding unavailable";
    return true;
}

void update(const configuration& value,bool in_world) {
    std::scoped_lock lock(resource_mutex);
    if (!in_world || !value.enabled || !value.skybox) {
        active_material.store(0);selected_path.clear();selection_recorded=false;return;
    }
    if (!resources_ready.load()) return;
    const auto end=std::find(value.sky_texture.begin(),value.sky_texture.end(),'\0');
    std::string path;
    if (end!=value.sky_texture.begin()) {
        if (end==value.sky_texture.end()) {active_material.store(0);selection_recorded=false;load_state="Skybox: texture path is not terminated";return;}
        path.assign(value.sky_texture.begin(),end);
    } else {
        const auto index=std::clamp(value.skybox_index,0,static_cast<int>(presets.size())-1);
        path=presets[index].resource_path;
    }
    if (selection_recorded && path==selected_path) return;
    active_material.store(0);selected_path=path;selection_recorded=true;
    if (path.empty()) {load_state="Skybox: map default";return;}
    if (!valid_texture(path)) {load_state="Skybox: use a materials/*.vtex path below 200 characters";return;}
    if (const auto found=materials.find(path);found!=materials.end()) {
        active_material.store(found->second.material);load_state="Custom skybox loaded";return;
    }
    if (materials.size()>=max_materials) {load_state="Skybox: material cache full; restart to load more textures";return;}
    sky_material material{};
    if (!preload_texture(bindings,path.c_str(),material.texture_binding)) {
        load_state="Skybox: cubemap resource unavailable";return;
    }
    const auto source=std::string{
        "<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} "
        "format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->\n{\n"
        "Shader = \"sky.vfx\"\ng_flBrightnessExposureBias = 0.0\ng_flRenderOnlyExposureBias = 0.0\n"
        "SkyTexture = resource:\""}+path+"\"\ng_tSkyTexture = resource:\""+path+"\"\n}\n";
    // Native material names must remain distinct even for colliding path hashes
    // or retries. Engine-owned names outlive our per-map and shutdown caches.
    const auto name="cs2_custom_sky_"+std::to_string(material_session)+"_"
        +std::to_string(reinterpret_cast<std::uintptr_t>(&next_material_id))+"_"+std::to_string(++next_material_id);
    if (!create_material(bindings,source.c_str(),name.c_str(),material)) {
        load_state="Skybox: native material creation failed";return;
    }
    materials.emplace(path,material);active_material.store(material.material);load_state="Custom skybox loaded";
}

void reset() {
    std::scoped_lock lock(resource_mutex);
    active_material.store(0);selected_path.clear();selection_recorded=false;
}
void shutdown() {
    std::scoped_lock lock(resource_mutex);
    active_material.store(0);selected_path.clear();selection_recorded=false;materials.clear();bindings={};
    resources_ready.store(false);scene_supported.store(false);shader_supported.store(false);
    load_state="World bindings stopped";
}

sky_draw_scope::sky_draw_scope(std::uintptr_t meshes,int count,const configuration& value)
    :lock_(mutation_mutex,std::defer_lock) {
    if (!value.enabled || !scene_supported.load() || !meshes || count<=0 || count>4096) return;
    const auto replacement=value.skybox ? active_material.load() : 0;
    if (!replacement && !value.tint_sky) return;
    lock_.lock();
    const auto end_offset=static_cast<std::size_t>(count)*0x70-0x58;
    if (meshes>UINTPTR_MAX-end_offset) return;
    const auto descriptor=memory::read<std::uintptr_t>(meshes+end_offset);
    if (!descriptor || descriptor>UINTPTR_MAX-0xf4) return;
    if (replacement) {
        const auto binding=memory::read<std::uintptr_t>(descriptor+0xd0);
        const auto original=memory::safe_read<std::uintptr_t>(binding);
        if (original && *original && memory::safe_write(binding,replacement)) {binding_=binding;material_=*original;}
    }
    if (value.tint_sky) {
        const auto original=memory::safe_read<std::array<float,3>>(descriptor+0xe8);
        if (original && finite_color(*original) && memory::safe_write(descriptor+0xe8,tint(value.sky_color,value.sky_intensity))) {
            descriptor_=descriptor;color_=*original;tinted_=true;
        }
    }
}
sky_draw_scope::~sky_draw_scope() {
    if (binding_) (void)memory::safe_write(binding_,material_);
    if (tinted_) (void)memory::safe_write(descriptor_+0xe8,color_);
}
light_draw_scope::light_draw_scope(std::uintptr_t object,const configuration& configured)
    :lock_(mutation_mutex,std::defer_lock) {
    const auto value=effective_configuration(configured);
    if (!value.enabled || !scene_supported.load() || !value.lighting || !object || object>UINTPTR_MAX-0xf0) return;
    lock_.lock();
    const auto original=memory::safe_read<std::array<float,3>>(object+0xe4);
    const auto color=value.rainbow_lighting ? rainbow(value.rainbow_speed) : value.lighting_color;
    if (original && finite_color(*original) && memory::safe_write(object+0xe4,tint(color,value.lighting_intensity))) {
        object_=object;color_=*original;
    }
}
light_draw_scope::~light_draw_scope() {if (object_) (void)memory::safe_write(object_+0xe4,color_);}

bool shader_override(std::uint32_t hash,const configuration& value,float elapsed,shader_vector& storage) noexcept {
    if (!value.enabled) return false;
    auto fill=[&](float scalar){storage.values.fill(scalar);return true;};
    if(value.depthOfField&&hash==0x2acab07c){storage.values=value.focusDistances;return true;}
    if (value.bloom) {
        const auto t=bounded(value.bloom_strength,0.65f,0.0f,3.0f);
        switch (hash) {
        case 0x565EAF76:return fill(0.3f+t*1.2f);
        case 0xBA98A9B0:return fill(std::max(0.01f,1.5f-t*1.2f));
        case 0x2AE72B37:return fill(0.5f+t*1.5f);
        case 0xB692902E:return fill(0.2f+t*0.6f);
        case 0x1313A424:return fill(0.1f+t*0.4f);
        default:break;
        }
    }
    if (value.gamma && hash==0x24470A87) return fill(bounded(value.gamma_value,1.0f,0.1f,4.0f));
    if (value.wind && hash==0x2A416C12) {
        const auto radians=bounded(value.wind_direction,90.0f,-360.0f,360.0f)*std::numbers::pi_v<float>/180.0f;
        storage.values={std::cos(radians),std::sin(radians),0.0f,0.0f};return true;
    }
    if (value.wind && hash==0xEB0D997E) {
        const auto strength=bounded(value.wind_strength,1.0f,0.0f,10.0f);
        const auto frequency=bounded(value.wind_frequency,1.0f,0.0f,10.0f);
        storage.values={strength,frequency,strength,frequency};return true;
    }
    if (value.wetness) {
        switch (hash) {
        case 0x374C1B3C:return fill(1.0f);
        case 0x0F592812:return fill(bounded(value.wetness_amount,1.0f,0.0f,2.0f));
        case 0x2DBEE393:return fill(bounded(elapsed,0.0f,0.0f,1.0e7f)*bounded(value.wetness_speed,1.0f,0.0f,10.0f));
        default:break;
        }
    }
    return false;
}

bool apply_fog(void* output,int* mode,set_shader_fn setter,const configuration& configured) {
    const auto value=effective_configuration(configured);
    if (!value.enabled || !value.fog || !shader_supported.load() || !output || !mode || !setter) return false;
    const auto address=reinterpret_cast<std::uintptr_t>(output);
    if (address>UINTPTR_MAX-0x390) return false;
    const auto enable=fog_enable_slot(address);
    const auto previous=memory::safe_read<std::uint32_t>(enable);
    if (!previous || !memory::safe_read<std::uint64_t>(address+0x388)) return false;
    const auto color=tint(value.fog_color,1.0f);
    const auto falloff=0.5f+bounded(value.fog_anisotropy,0.1f,0.0f,1.0f)*8.0f;
    const std::array<shader_vector,3> vectors{{
        {{{0.0f,bounded(value.fog_distance,2500.0f,1.0f,20000.0f),0.0f,0.0f}}},
        {{{bounded(value.fog_density,0.2f,0.0f,1.0f),falloff,0.0f,0.0f}}},
        {{{color[0],color[1],color[2],0.0f}}}
    }};
    if (!set_fog_parameters(setter,output,vectors.data())) return false;
    if (*previous!=1) {
        const auto old_checksum=memory::safe_read<std::uint64_t>(address+0x388);
        if (!old_checksum) return false;
        const auto checksum=*old_checksum^integer_hash(enable_gradient_fog,*previous)^integer_hash(enable_gradient_fog,1);
        if (!memory::safe_write(enable,std::uint32_t{1})) return false;
        if (!memory::safe_write(address+0x388,checksum)) {
            (void)memory::safe_write(enable,*previous);return false;
        }
    }
    return true;
}
}
