#include "features/visuals/engine.hpp"
#include "particles.hpp"
#include "config.hpp"
#include "weapon_effects.hpp"
#include "features/visuals/particle_chams.h"
#include "core/runtime_paths.h"
#include <filesystem>
#include <fstream>
#include <deque>
#include <numeric>
#include <unordered_set>

namespace custom::particles {
namespace {
constexpr std::uint32_t invalid_effect=0xffffffffu;
constexpr unsigned max_effects=8;
constexpr unsigned max_transients=32,max_pending_layers=32,max_frame_creations=8;
constexpr unsigned reserved_kill_slots=4;
constexpr std::array<preset,9> choices{{
    {"Rain","particles/rain_fx/rain.vpcf",false,false},
    {"Snow","particles/rain_fx/snow.vpcf",false,false},
    {"Sparks","particles/ambient_fx/ambient_sparks.vpcf",false,false},
    {"Confetti","particles/weapons/cs_weapon_fx/weapon_confetti_omni.vpcf",false,true},
    {"Fireworks","particles/inferno_fx/firework_crate_shower_01b.vpcf",false,false},
    {"Lightning","particles/environment/de_train/train_lightning.vpcf",false,false},
    {"Stars","particles/ambient_fx/stars_skybox.vpcf",false,false},
    {"Dust motes","particles/ambient_fx/dustmotes_64.vpcf",false,false},
    {"Custom resource",nullptr,false,true}
}};
constexpr std::array<kill_recipe,7> death_recipes{{
    {"Vaporize",{"particles/explosions_fx/explosion_hegrenade_brief_smoke_blasts.vpcf","particles/generic_fx/fx_electricspark_flare.vpcf"},2},
    {"Electric",{"particles/generic_fx/fx_electric_arc_spark.vpcf","particles/generic_fx/fx_electricspark_flare.vpcf"},2},
    {"Confetti",{"particles/weapons/cs_weapon_fx/weapon_confetti_omni.vpcf"},1},
    {"Fireworks",{"particles/inferno_fx/firework_crate_shower_01b.vpcf"},1},
    {"Dissolve",{"particles/explosions_fx/explosion_hegrenade_snow_flakes.vpcf","particles/generic_fx/fx_electricspark_glow.vpcf"},2},
    {"Shockwave",{"particles/explosions_fx/bumpmine_detonate_ring.vpcf","particles/explosions_fx/bumpmine_detonate_sparks.vpcf"},2},
    {"Celebration",{"particles/weapons/cs_weapon_fx/weapon_confetti_omni.vpcf","particles/ui/ui_gold_halo_sparkles.vpcf","particles/inferno_fx/firework_crate_shower_01b.vpcf"},3}
}};
constexpr std::array<const char*,10> feature_resources{{
    "particles/explosions_fx/explosion_hegrenade_brief_smoke_blasts.vpcf",
    "particles/ambient_fx/env_sparks_omni.vpcf",
    "particles/generic_fx/fx_electric_arc_spark.vpcf",
    "particles/generic_fx/fx_electricspark_flare.vpcf",
    "particles/ui/ui_gold_halo_sparkles.vpcf",
    "particles/ambient_fx/impact_dirt_child_smoke_puff.vpcf",
    "particles/explosions_fx/bumpmine_detonate_ring.vpcf",
    "particles/burning_fx/gas_cannister_trail_smoke.vpcf",
    "particles/explosions_fx/c4_light_pulse_single_light.vpcf",
    "particles/inferno_fx/firework_crate_shower_01b.vpcf"
}};
// Exact game DLLs audited with the particle signatures in engine.hpp.
constexpr auto client_sha256=skin_bindings::kClientSha256;
constexpr auto resource_sha256=skin_bindings::kResourceSha256;

struct path_buffer {
    std::uint32_t length{};
    std::uint32_t capacity{0xc00000c8};
    union {std::uintptr_t pointer; char inline_data[0xc8];} storage{};
    std::uintptr_t unknown{};
    std::uintptr_t resource_type{};
};
static_assert(sizeof(path_buffer)==0xe0);
static_assert(offsetof(path_buffer,resource_type)==0xd8);

using create_fn=int*(__fastcall*)(std::uintptr_t,std::uint32_t*,const char*,int,std::uintptr_t,std::uintptr_t,std::uintptr_t,int);
using point_fn=bool(__fastcall*)(std::uintptr_t,std::uint32_t,int,const math::vector3*,float);
using destroy_fn=void(__fastcall*)(std::uintptr_t,std::uint32_t,bool,bool);
using init_path_fn=bool(__fastcall*)(path_buffer*,const char*);
using load_fn=std::uintptr_t(__fastcall*)(std::uintptr_t,path_buffer*,const char*);
std::uintptr_t manager_slot{},rules_slot{},resource_system{};
create_fn create_effect{};
point_fn set_point{};
destroy_fn destroy_effect{};
init_path_fn init_path{};
load_fn load_resource{};
std::filesystem::path content_directory;
std::unordered_set<std::string> installed_particles;
std::unordered_set<std::string> loaded_transient_resources;
std::atomic<bool> ready{},stop_requested{},stopped{true},updating{};
std::atomic<unsigned> published_count{};
std::atomic<const char*> state{"Particles not initialized"};

struct effect {std::uint32_t index{invalid_effect};};
std::array<effect,max_effects> effects;
struct transient_effect {
    effect handle;
    std::uint64_t ticket{};
    ULONGLONG expires{};
    effect_kind kind{effect_kind::kill};
    attachment anchor{attachment::none};
    std::uint32_t pawn_handle{},weapon_handle{};
    unsigned body_anchor{};
    bool cancelled{};
};
std::array<transient_effect,max_transients> transients;
struct burst_job {
    std::uint64_t ticket{};
    std::array<burst_request,3> requests;
    unsigned layers{};
    ULONGLONG queued{};
    std::uintptr_t manager{},list{},rules{};
    float round{};
};
struct receipt {
    std::uint64_t ticket{};
    burst_state result{burst_state::expired};
    ULONGLONG expires{};
};
std::mutex queue_mutex;
std::deque<burst_job> pending_jobs;
std::unordered_set<std::uint64_t> cancelled_tickets;
unsigned pending_layers{};
std::array<receipt,64> receipts;
std::uint64_t next_ticket{1};
std::uintptr_t owner_manager{},last_list{},last_rules{};
float last_round{};
std::string current_resource,loaded_resource;
ULONGLONG next_retry{},next_burst{};
bool current_bursts{};

template<class T> std::optional<T> field(std::uintptr_t object,const char* type,const char* name) {
    if (!object) return std::nullopt;
    const auto offset=systems::schemas::lookup(type,fnv1a::runtime_hash(name));
    return offset ? memory::safe_read<T>(object+offset) : std::nullopt;
}
bool view_origin(math::vector3& origin) {
    const auto controller=memory::read<std::uintptr_t>(addresses::globals::local_player_controller);
    const auto handle=field<std::uint32_t>(controller,"CBasePlayerController","m_hPawn");
    auto pawn=handle ? systems::g_entities.lookup(*handle) : 0;
    if (!pawn) return false;
    if (field<int>(pawn,"C_BaseEntity","m_iHealth").value_or(0)<=0) {
        const auto observer_handle=field<std::uint32_t>(controller,"CCSPlayerController","m_hObserverPawn");
        const auto observer=observer_handle ? systems::g_entities.lookup(*observer_handle) : 0;
        const auto services=field<std::uintptr_t>(observer,"C_BasePlayerPawn","m_pObserverServices");
        const auto target_handle=field<std::uint32_t>(services.value_or(0),"CPlayer_ObserverServices","m_hObserverTarget");
        const auto target=target_handle ? systems::g_entities.lookup(*target_handle) : 0;
        if (!target) return false;
        pawn=target;
    }
    const auto node=field<std::uintptr_t>(pawn,"C_BaseEntity","m_pGameSceneNode");
    const auto position=field<math::vector3>(node.value_or(0),"CGameSceneNode","m_vecAbsOrigin");
    if (!position || !std::isfinite(position->x) || !std::isfinite(position->y) || !std::isfinite(position->z)) return false;
    origin=*position;
    return true;
}

// Keep SEH boundaries around native calls small; no C++ objects requiring
// unwinding live inside these functions. Failed native operations are reported
// while effect handles remain tracked until cleanup succeeds.
bool native_create(std::uintptr_t manager,const char* resource,std::uint32_t* index) noexcept {
    __try {create_effect(manager,index,resource,8,0,0,0,0);return *index!=invalid_effect;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool native_point(std::uintptr_t manager,std::uint32_t index,const math::vector3* origin) noexcept {
    __try {return set_point(manager,index,0,origin,0.0f);}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool native_destroy(std::uintptr_t manager,std::uint32_t index) noexcept {
    __try {destroy_effect(manager,index,true,true);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool native_load(const char* resource) noexcept {
    path_buffer buffer{};
    __try {
        if (!init_path(&buffer,resource)) return false;
        // MSVC's donor multi-character literal 'fcpv', made explicit.
        buffer.resource_type=0x66637076;
        return load_resource(resource_system,&buffer,"")!=0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}

unsigned count() {
    return static_cast<unsigned>(std::ranges::count_if(effects,[](const effect& item){return item.index!=invalid_effect;})
        +std::ranges::count_if(transients,[](const transient_effect& item){return item.handle.index!=invalid_effect;}));
}
void record_result(std::uint64_t ticket,burst_state result,ULONGLONG expires=0) {
    std::scoped_lock lock(queue_mutex);
    for (auto& entry:receipts) if (entry.ticket==ticket) {entry.result=result;entry.expires=expires;return;}
}
void discard_pending() {
    std::scoped_lock lock(queue_mutex);
    for (const auto& job:pending_jobs) for (auto& entry:receipts)
        if (entry.ticket==job.ticket) {entry.result=burst_state::failed;entry.expires=0;break;}
    pending_jobs.clear();pending_layers=0;
}
void discard_stale_pending(std::uintptr_t manager,std::uintptr_t list,std::uintptr_t rules,float round) {
    std::scoped_lock lock(queue_mutex);
    for (auto job=pending_jobs.begin();job!=pending_jobs.end();) {
        if (job->manager==manager && job->list==list && job->rules==rules && job->round==round) {++job;continue;}
        for (auto& entry:receipts) if (entry.ticket==job->ticket) {entry.result=burst_state::failed;break;}
        pending_layers-=job->layers;job=pending_jobs.erase(job);
    }
}
void forget_all() {
    for (auto& item:effects) item={};
    for (auto& item:transients) {
        if (item.handle.index!=invalid_effect) record_result(item.ticket,burst_state::expired);
        item={};
    }
    owner_manager=0;
    loaded_transient_resources.clear();
    published_count.store(0,std::memory_order_release);
}
bool release_one(effect& item,std::uintptr_t manager) {
    if (item.index==invalid_effect) return true;
    if (!manager || manager!=owner_manager) {item={};return true;}
    if (!native_destroy(manager,item.index)) return false;
    item={};
    return true;
}
bool release_all() {
    const auto manager=memory::read<std::uintptr_t>(manager_slot);
    bool complete=true;
    for (auto& item:effects) complete=release_one(item,manager) && complete;
    for (auto& item:transients) {
        if (item.handle.index==invalid_effect) continue;
        item.cancelled=true;
        if (release_one(item.handle,manager)) {record_result(item.ticket,burst_state::expired);item={};}
        else complete=false;
    }
    if (complete) owner_manager=0;
    published_count.store(count(),std::memory_order_release);
    if (!complete) state.store("Particle cleanup failed; DLL must remain loaded");
    return complete;
}
bool release_ambient() {
    const auto manager=memory::read<std::uintptr_t>(manager_slot);
    bool complete=true;
    for (auto& item:effects) complete=release_one(item,manager) && complete;
    published_count.store(count(),std::memory_order_release);
    if (!count()) owner_manager=0;
    if (!complete) state.store("Particle cleanup failed; DLL must remain loaded");
    return complete;
}

std::optional<std::string> clean_resource(std::string resource) {
    std::replace(resource.begin(),resource.end(),'\\','/');
    if (resource.ends_with(".vpcf_c")) resource.resize(resource.size()-2);
    if (!resource.starts_with("particles/") || !resource.ends_with(".vpcf")
        || resource.size()>=0xc8 || resource.find("..")!=std::string::npos
        || resource.find(':')!=std::string::npos || resource.find('\0')!=std::string::npos) return std::nullopt;
    for (const unsigned char c:resource) if (c<32 || c>=127) return std::nullopt;
    return resource;
}

bool read_particle_index() {
    std::array<wchar_t,32768> module_path{};
    const auto client=GetModuleHandleW(L"client.dll");
    const auto length=GetModuleFileNameW(client,module_path.data(),static_cast<DWORD>(module_path.size()));
    if (!length || length>=module_path.size()) return false;
    // client.dll lives at game/csgo/bin/win64/client.dll.
    content_directory=std::filesystem::path(module_path.data()).parent_path().parent_path().parent_path();
    std::ifstream input(runtime_paths::NativePath(content_directory/L"pak01_dir.vpk"),std::ios::binary|std::ios::ate);
    if (!input) return false;
    const auto length_bytes=input.tellg();
    if (length_bytes<28 || length_bytes>64*1024*1024) return false;
    std::vector<char> data(static_cast<std::size_t>(length_bytes));
    input.seekg(0);
    if (!input.read(data.data(),static_cast<std::streamsize>(data.size()))) return false;
    const auto u32=[&](std::size_t at){std::uint32_t value{};std::memcpy(&value,data.data()+at,4);return value;};
    if (u32(0)!=0x55aa1234 || (u32(4)!=1 && u32(4)!=2)) return false;
    std::size_t position=u32(4)==2 ? 28 : 12;
    const auto tree_end=position+u32(8);
    if (tree_end>data.size()) return false;
    const auto text=[&]() -> std::optional<std::string> {
        const auto start=position;
        while (position<tree_end && data[position]) ++position;
        if (position>=tree_end) return std::nullopt;
        return std::string(data.data()+start,position++-start);
    };
    for (;;) {
        const auto extension=text();
        if (!extension) return false;
        if (extension->empty()) break;
        for (;;) {
            const auto directory=text();
            if (!directory) return false;
            if (directory->empty()) break;
            for (;;) {
                const auto name=text();
                if (!name) return false;
                if (name->empty()) break;
                if (position+18>tree_end) return false;
                std::uint16_t preload{};std::memcpy(&preload,data.data()+position+4,2);
                std::uint16_t terminator{};std::memcpy(&terminator,data.data()+position+16,2);
                if (terminator!=0xffff || position+18+preload>tree_end) return false;
                if (*extension=="vpcf_c") installed_particles.insert(*directory+"/"+*name+".vpcf");
                position+=18+preload;
            }
        }
    }
    return !installed_particles.empty();
}

bool resource_exists(const std::string& resource) {
    if (installed_particles.contains(resource)) return true;
    std::error_code error;
    return std::filesystem::is_regular_file(runtime_paths::NativePath(content_directory/(resource+"_c")),error);
}
float bounded(float value,float minimum,float maximum,float fallback) {
    return std::isfinite(value) ? std::clamp(value,minimum,maximum) : fallback;
}
math::vector3 emitter_position(const math::vector3& origin,const configuration& config,unsigned index,unsigned total,ULONGLONG now) {
    const auto radius=std::min(2000.0f,bounded(config.particle_radius,50,2000,600)*bounded(config.particle_scale,0.1f,5,1));
    const auto height=bounded(config.particle_height,0,1200,300);
    // Spread multiplier affects emitter placement, never undocumented native
    // particle control points. Keep a ring around the followed view pawn.
    const auto phase=current_bursts ? static_cast<float>(now%20000)/20000.0f : 0.0f;
    const auto angle=(static_cast<float>(index)/std::max(1u,total)+phase)*2.0f*std::numbers::pi_v<float>;
    return {origin.x+radius*std::cos(angle),origin.y+radius*std::sin(angle),origin.z+height};
}
bool category_enabled(effect_kind kind,const configuration& config) {
    if(kind==effect_kind::body_aura)return visuals::particle_chams::Enabled();
    if (!config.enabled) return false;
    switch (kind) {
    case effect_kind::kill:return config.kill_particles;
    case effect_kind::impact:return config.impact_particles;
    case effect_kind::hit:return config.hit_particles;
    case effect_kind::headshot:return config.headshot_accent;
    case effect_kind::spawn:return config.spawn_particles;
    case effect_kind::jump:return config.jump_particles;
    case effect_kind::landing:return config.landing_particles;
    case effect_kind::speed_trail:return config.speed_trail;
    case effect_kind::reload:return config.reload_particles;
    case effect_kind::victory:return config.victory_particles;
    case effect_kind::gun:return config.weapon_effects&&config.gun_particles;
    case effect_kind::barrel:return config.weapon_effects&&config.barrel_particles;
    case effect_kind::hands:return config.weapon_effects&&config.hand_particles;
    case effect_kind::inspect:return config.weapon_effects&&config.inspect_burst;
    case effect_kind::gun_reload:return config.weapon_effects&&config.reload_burst;
    case effect_kind::muzzle:return config.weapon_effects&&config.muzzle_flash;
    case effect_kind::gun_smoke:return config.weapon_effects&&config.muzzle_smoke;
    case effect_kind::body_aura:return false;
    }
    return false;
}
std::uint64_t enqueue_job(burst_job job) {
    if (!ready.load(std::memory_order_acquire) || stop_requested.load(std::memory_order_acquire)
        || !job.layers || job.layers>job.requests.size()) return 0;
    const auto config=get_configuration();
    for (unsigned i=0;i<job.layers;++i) {
        auto& request=job.requests[i];
        if (!category_enabled(request.kind,config)
            || !std::ranges::all_of(request.origin,[](float value){return std::isfinite(value) && std::abs(value)<10000000;})) return 0;
        const auto resource=clean_resource(std::move(request.resource));
        if (!resource || !resource_exists(*resource)) return 0;
        request.resource=*resource;
        request.lifetime=bounded(request.lifetime,0.2f,8.0f,2.5f);
    }
    const auto now=GetTickCount64();
    job.manager=memory::read<std::uintptr_t>(manager_slot);
    job.list=memory::read<std::uintptr_t>(addresses::globals::entity_list);
    job.rules=memory::read<std::uintptr_t>(rules_slot);
    job.round=field<float>(job.rules,"C_CSGameRules","m_fRoundStartTime").value_or(0);
    if (!job.manager || !job.list) return 0;
    std::scoped_lock lock(queue_mutex);
    const bool primary=job.requests[0].kind==effect_kind::kill;
    const auto nonkill_layers=std::accumulate(pending_jobs.begin(),pending_jobs.end(),0u,[](unsigned total,const burst_job& pending){
        return total+(pending.requests[0].kind==effect_kind::kill ? 0u : pending.layers);
    });
    if (stop_requested.load(std::memory_order_acquire) || pending_layers+job.layers>max_pending_layers
        || (!primary && nonkill_layers+job.layers>max_pending_layers-reserved_kill_slots)) return 0;
    const auto available=std::ranges::find_if(receipts,[&](const receipt& item){
        return !item.ticket || item.result==burst_state::failed || item.result==burst_state::expired
            || (item.result==burst_state::spawned && now>=item.expires);
    });
    if (available==receipts.end()) return 0;
    job.ticket=next_ticket++;
    if (!job.ticket) job.ticket=next_ticket++;
    job.queued=now;
    *available={job.ticket,burst_state::pending,0};
    const auto ticket=job.ticket;
    pending_layers+=job.layers;
    if (primary) {
        const auto first_nonkill=std::ranges::find_if(pending_jobs,[](const burst_job& pending){return pending.requests[0].kind!=effect_kind::kill;});
        pending_jobs.insert(first_nonkill,std::move(job));
    } else pending_jobs.push_back(std::move(job));
    return ticket;
}
bool update_transients(std::uintptr_t manager,const configuration& config,ULONGLONG now) {
    std::unordered_set<std::uint64_t> cancelled;
    {std::scoped_lock lock(queue_mutex);cancelled.swap(cancelled_tickets);}
    // Retain every cancellation before a native destroy can fail and return.
    // A later frame must retry cleanup rather than revive unprocessed siblings.
    for(auto &item:transients)item.cancelled=item.cancelled||cancelled.contains(item.ticket);
    for (auto& item:transients) {
        if(item.handle.index==invalid_effect)continue;
        bool keep=now<item.expires&&category_enabled(item.kind,config)&&!item.cancelled;
        if(keep&&item.anchor!=attachment::none) {
            math::vector3 point;
            keep=(item.anchor==attachment::body
                ?visuals::particle_chams::AttachmentPosition(item.pawn_handle,item.body_anchor,point)
                :weapon_fx::attachment_position(item.anchor,item.pawn_handle,item.weapon_handle,point))&&native_point(manager,item.handle.index,&point);
        }
        if(keep)continue;
        item.cancelled=true;
        if (!release_one(item.handle,manager)) {
            state.store("Particle cleanup failed; DLL must remain loaded");published_count.store(count(),std::memory_order_release);return false;
        }
        record_result(item.ticket,burst_state::expired);item={};
    }
    unsigned frame_creations{};
    while (frame_creations<max_frame_creations) {
        burst_job job;
        {
            std::scoped_lock lock(queue_mutex);
            if (pending_jobs.empty()) break;
            const auto& front=pending_jobs.front();
            if (now-front.queued>1000 || !category_enabled(front.requests[0].kind,config)) {
                for (auto& entry:receipts) if (entry.ticket==front.ticket) {entry.result=burst_state::failed;break;}
                pending_layers-=front.layers;pending_jobs.pop_front();continue;
            }
            const auto first=transients.begin()+(front.requests[0].kind==effect_kind::kill ? 0 : reserved_kill_slots);
            const auto free_slots=std::count_if(first,transients.end(),[](const transient_effect& item){return item.handle.index==invalid_effect;});
            if (front.layers>free_slots || frame_creations+front.layers>max_frame_creations) break;
            job=std::move(pending_jobs.front());pending_layers-=job.layers;pending_jobs.pop_front();
        }
        bool successful=true;
        ULONGLONG deadline=now;
        for (unsigned i=0;i<job.layers;++i) {
            const auto& request=job.requests[i];
            math::vector3 origin{request.origin[0],request.origin[1],request.origin[2]};
            if(request.anchor!=attachment::none&&!(request.anchor==attachment::body
                ?visuals::particle_chams::AttachmentPosition(request.pawn_handle,request.body_anchor,origin)
                :weapon_fx::attachment_position(request.anchor,request.pawn_handle,request.weapon_handle,origin))) {successful=false;break;}
            const auto first=transients.begin()+(request.kind==effect_kind::kill ? 0 : reserved_kill_slots);
            auto slot=std::find_if(first,transients.end(),[](const transient_effect& item){return item.handle.index==invalid_effect;});
            if (slot==transients.end() || !resource_exists(request.resource)) {successful=false;break;}
            if (!loaded_transient_resources.contains(request.resource)) {
                if (!native_load(request.resource.c_str())) {successful=false;break;}
                loaded_transient_resources.insert(request.resource);
            }
            slot->ticket=job.ticket;slot->kind=request.kind;
            slot->anchor=request.anchor;slot->pawn_handle=request.pawn_handle;slot->weapon_handle=request.weapon_handle;
            slot->body_anchor=request.body_anchor;
            slot->expires=now+static_cast<ULONGLONG>(request.lifetime*1000);
            deadline=std::max(deadline,slot->expires);
            owner_manager=manager;
            ++frame_creations;
            if (!native_create(manager,request.resource.c_str(),&slot->handle.index)) {successful=false;break;}
            if (!native_point(manager,slot->handle.index,&origin)) {successful=false;break;}
        }
        if (!successful) {
            bool cleaned=true;
            for (auto& item:transients) if (item.ticket==job.ticket) {
                item.cancelled=true;
                if (release_one(item.handle,manager)) item={};else cleaned=false;
            }
            record_result(job.ticket,burst_state::failed);
            if (!cleaned) {state.store("Particle cleanup failed; DLL must remain loaded");published_count.store(count());return false;}
            state.store("Particle effect creation failed");
        } else record_result(job.ticket,burst_state::spawned,deadline);
        published_count.store(count(),std::memory_order_release);
    }
    published_count.store(count(),std::memory_order_release);
    if (!count()) owner_manager=0;
    return true;
}
}

bool initialize() {
    stop_requested.store(false);stopped.store(true);ready.store(false);
    if (!game::module_matches("client.dll",client_sha256)
        || !game::module_matches("resourcesystem.dll",resource_sha256)) {
        state.store("Particle bindings unavailable for this game build");return false;
    }
    manager_slot=PATTERN(patterns::particle_manager);
    rules_slot=PATTERN(patterns::game_rules);
    create_effect=reinterpret_cast<create_fn>(PATTERN(patterns::particle_create_effect));
    set_point=reinterpret_cast<point_fn>(PATTERN(patterns::particle_set_control_point));
    destroy_effect=reinterpret_cast<destroy_fn>(PATTERN(patterns::particle_destroy_effect));
    init_path=reinterpret_cast<init_path_fn>(PATTERN(patterns::init_particle_path_buffer));
    load_resource=reinterpret_cast<load_fn>(PATTERN(patterns::resource_system_load));
    const auto factory=reinterpret_cast<void*(__cdecl*)(const char*,int*)>(GetProcAddress(GetModuleHandleA("resourcesystem.dll"),"CreateInterface"));
    resource_system=factory ? reinterpret_cast<std::uintptr_t>(factory("ResourceSystem013",nullptr)) : 0;
    if (!manager_slot || !create_effect || !set_point || !destroy_effect || !init_path || !load_resource || !resource_system) {
        state.store("Required particle/resource signature unavailable");return false;
    }
    if (!read_particle_index()) {state.store("Cannot verify installed particle resources");return false;}
    ready.store(true,std::memory_order_release);
    state.store("Particles ready; disabled");return true;
}

void on_frame_stage(int stage) {
    if (stage!=7) return;
    updating.store(true,std::memory_order_release);
    struct update_guard {~update_guard(){updating.store(false,std::memory_order_release);}} guard;
    if (stop_requested.load(std::memory_order_acquire)) {
        visuals::particle_chams::Reset();
        discard_pending();
        stopped.store(release_all(),std::memory_order_release);return;
    }
    if (!ready.load(std::memory_order_acquire)) return;
    const auto config=get_configuration();
    if (!config.enabled && !visuals::particle_chams::Enabled()) {
        visuals::particle_chams::Reset();
        discard_pending();
        if (release_all()) state.store("Particles disabled");
        current_resource.clear();loaded_resource.clear();loaded_transient_resources.clear();next_retry=next_burst=0;return;
    }
    const auto now=GetTickCount64();
    const auto manager=memory::read<std::uintptr_t>(manager_slot);
    const auto list=memory::read<std::uintptr_t>(addresses::globals::entity_list);
    const auto rules=memory::read<std::uintptr_t>(rules_slot);
    const auto round=field<float>(rules,"C_CSGameRules","m_fRoundStartTime").value_or(0);
    if (manager!=owner_manager && owner_manager) {visuals::particle_chams::Reset();forget_all();}
    discard_stale_pending(manager,list,rules,round);
    if (list!=last_list || rules!=last_rules || round!=last_round) {
        visuals::particle_chams::Reset();
        if (!release_all()) return;
        loaded_resource.clear();loaded_transient_resources.clear();last_list=list;last_rules=rules;last_round=round;next_retry=next_burst=0;
    }
    if (!manager || !list) {
        visuals::particle_chams::Reset();
        discard_pending();
        if (release_all()) state.store("Particles waiting for a local player or spectator target");return;
    }
    visuals::particle_chams::Update();
    if (!update_transients(manager,config,now)) return;
    if (!config.enabled || !config.particles) {
        if (release_ambient()) state.store(active_count() ? "Particle effects active" : "Particles disabled");
        current_resource.clear();loaded_resource.clear();next_retry=next_burst=0;return;
    }
    math::vector3 origin{};
    if (!view_origin(origin)) {
        if (release_ambient()) state.store("Ambient particles waiting for a local player or spectator target");return;
    }
    const auto selected=std::clamp(config.particle_preset,0,static_cast<int>(choices.size()-1));
    const auto& choice=choices[selected];
    const auto end=std::find(config.particle_resource.begin(),config.particle_resource.end(),'\0');
    const auto resource=clean_resource(choice.resource ? choice.resource : std::string(config.particle_resource.begin(),end));
    if (!resource || !resource_exists(*resource)) {
        if (release_ambient()) state.store("Particle resource missing; select an installed particles/*.vpcf path");return;
    }
    if (*resource!=current_resource || choice.bursts!=current_bursts) {
        if (!release_ambient()) return;
        current_resource=*resource;loaded_resource.clear();current_bursts=choice.bursts;next_retry=next_burst=0;
    }
    if (now<next_retry) return;
    if (loaded_resource!=*resource) {
        if (!native_load(resource->c_str())) {state.store("Particle resource load failed; retrying in 5 seconds");next_retry=now+5000;return;}
        loaded_resource=*resource;
    }
    const auto desired=static_cast<unsigned>(std::clamp(config.particle_count,1,static_cast<int>(max_effects)));
    for (unsigned i=desired;i<max_effects;++i) if (!release_one(effects[i],manager)) {state.store("Particle cleanup failed; DLL must remain loaded");return;}
    const auto burst_interval=static_cast<ULONGLONG>(1500.0f/bounded(config.particle_intensity,0.1f,5,1));
    const bool replace_burst=current_bursts && now>=next_burst;
    if (replace_burst) {
        if (!release_ambient()) return;
        next_burst=now+burst_interval;
    }
    for (unsigned i=0;i<desired;++i) {
        auto& item=effects[i];
        const auto position=emitter_position(origin,config,i,desired,now);
        bool created=false;
        if (item.index==invalid_effect) {
            owner_manager=manager;
            if (!native_create(manager,resource->c_str(),&item.index)) {
                published_count.store(count(),std::memory_order_release);
                state.store("Particle creation failed; retrying in 5 seconds");next_retry=now+5000;return;
            }
            created=true;
            published_count.store(count(),std::memory_order_release);
        }
        // Burst resources can finish naturally between replacements. Initialize
        // their origin once; continuous effects follow the view pawn each stage.
        if (current_bursts && !created) continue;
        if (!native_point(manager,item.index,&position)) {
            if (release_ambient()) state.store("Particle origin update failed; retrying in 5 seconds");
            next_retry=now+5000;return;
        }
    }
    published_count.store(count(),std::memory_order_release);
    state.store(choice.resource ? "Native particles active; authored resource colors" : "Custom particles active; resource defines colors and control points");
}

void on_level_shutdown() {
    visuals::particle_chams::Reset();
    discard_pending();
    const auto cleaned=release_all();
    current_resource.clear();loaded_resource.clear();loaded_transient_resources.clear();last_list=last_rules=0;last_round=0;next_retry=next_burst=0;
    if (stop_requested.load(std::memory_order_acquire)) stopped.store(cleaned,std::memory_order_release);
    else if (cleaned) state.store("Particles waiting for next map");
}
void request_shutdown() noexcept {
    stop_requested.store(true,std::memory_order_release);
    discard_pending();
    stopped.store(!published_count.load(std::memory_order_acquire) && !updating.load(std::memory_order_acquire),std::memory_order_release);
}
bool shutdown_complete() noexcept {return stopped.load(std::memory_order_acquire) && !updating.load(std::memory_order_acquire);}
std::string status() {return state.load(std::memory_order_acquire);}
unsigned active_count() noexcept {return published_count.load(std::memory_order_acquire);}
bool available() noexcept {return ready.load(std::memory_order_acquire)&&!stop_requested.load(std::memory_order_acquire);}
std::span<const preset> presets() noexcept {return choices;}
std::uint64_t queue_burst(burst_request request) {
    burst_job job;job.layers=1;job.requests[0]=std::move(request);
    return enqueue_job(std::move(job));
}
std::uint64_t queue_kill_effect(int recipe,const std::array<float,3>& origin,float lifetime) {
    if (recipe<0 || recipe>=static_cast<int>(death_recipes.size())) return 0;
    const auto& selected=death_recipes[recipe];
    burst_job job;job.layers=selected.layers;
    for (unsigned i=0;i<selected.layers;++i) job.requests[i]={selected.resources[i],origin,lifetime,effect_kind::kill};
    return enqueue_job(std::move(job));
}
std::uint64_t queue_feature_effect(effect_kind kind,const std::array<float,3>& origin,float lifetime) {
    const auto index=static_cast<unsigned>(kind);
    if (index>=feature_resources.size()) return 0;
    if(kind==effect_kind::impact) {
        const char* resources[]={feature_resources[index],"particles/explosions_fx/bumpmine_detonate_ring.vpcf","particles/ambient_fx/impact_dirt_child_smoke_puff.vpcf"};
        return queue_burst({resources[std::clamp(get_configuration().impact_style,0,2)],origin,lifetime,kind});
    }
    return queue_burst({feature_resources[index],origin,lifetime,kind});
}
void cancel_burst(std::uint64_t ticket) {
    if(!ticket)return;
    std::scoped_lock lock(queue_mutex);
    for(auto job=pending_jobs.begin();job!=pending_jobs.end();) {
        if(job->ticket!=ticket){++job;continue;}
        pending_layers-=job->layers;job=pending_jobs.erase(job);
    }
    for(auto& receipt:receipts) if(receipt.ticket==ticket) {
        if(receipt.result==burst_state::spawned||receipt.result==burst_state::pending)cancelled_tickets.insert(ticket);
        receipt.result=burst_state::expired;receipt.expires=0;return;
    }
}
burst_state kill_effect_state(std::uint64_t ticket) noexcept {
    if (!ticket) return burst_state::expired;
    std::scoped_lock lock(queue_mutex);
    for (const auto& entry:receipts) if (entry.ticket==ticket) {
        if (entry.result==burst_state::spawned && GetTickCount64()>=entry.expires) return burst_state::expired;
        return entry.result;
    }
    return burst_state::expired;
}
bool kill_effect_spawned(std::uint64_t ticket) noexcept {return kill_effect_state(ticket)==burst_state::spawned;}
bool burst_started(std::uint64_t ticket) noexcept {return kill_effect_spawned(ticket);}
std::span<const kill_recipe> kill_recipes() noexcept {return death_recipes;}
}
