#include "features/visuals/engine.hpp"
#include "combat.hpp"
#include "config.hpp"
#include "particles.hpp"
#include "weapon_effects.hpp"
#include "features/visuals/detail/bullet_tracers.hpp"

namespace custom::combat {
namespace {
using particles::effect_kind;
constexpr std::string_view scene_hash="040eb851fa7875523346b890f1ed6e158abc59e08bccbd8dc60539435277efa5";
constexpr std::string_view engine_hash="6456df18d8f954394024c2c07da8c848df15a5ace74ce7390d78970ceb39ee21";
constexpr std::array names{"player_death","bullet_impact","player_hurt","player_spawned",
    "player_jump","weapon_reload","round_end","round_start","weapon_fire"};
struct listener {
    void** table{};
    int debug_id{};
    unsigned index{};
    bool registered{};
};
std::array<listener,names.size()> listeners;
std::atomic<bool> ready{},stopping{},visibility_hook{},scene_supported{};
std::atomic<unsigned> registered_count{},event_callbacks{},kill_count{};
std::uintptr_t manager_slot{},registered_manager{},controller_getter{},pawn_getter{},int_getter{},float_getter{};
std::mutex state_mutex;
bullet_tracers::Store tracers;
std::string state="Combat effects not initialized";
ULONGLONG registration_attempt{};
struct death_record {
    std::uint64_t id{},ticket{};
    std::uintptr_t controller{},pawn{};
    std::uint32_t handle{},scene_handle{};
    ULONGLONG created{},expires{};
    bool primary{},spawned{},dead_observed{};
};
std::vector<death_record> deaths;
std::uint64_t next_record_id{};
unsigned chain{};
ULONGLONG last_kill{};
std::array<ULONGLONG,10> last_feature{};
struct movement_sample {bool valid{},grounded{};std::uint32_t pawn_handle{};};
movement_sample movement;
struct spawn_request {
    std::uintptr_t controller{};
    std::uint32_t controller_handle{},expected_pawn{};
    ULONGLONG expires{};
} pending_spawn;
constexpr std::size_t max_deaths=32;
struct callback_scope {
    callback_scope(){event_callbacks.fetch_add(1);}
    ~callback_scope(){event_callbacks.fetch_sub(1);}
};

template<class T> std::optional<T> field(std::uintptr_t entity,const char* type,const char* name) {
    if (!entity) return {};
    const auto offset=systems::schemas::lookup(type,fnv1a::runtime_hash(name));
    return offset ? memory::safe_read<T>(entity+offset) : std::nullopt;
}
struct identity_handle {std::uint32_t raw{},scene{};};
identity_handle entity_identity(std::uintptr_t entity) {
    if (!entity) return {};
    const auto identity=memory::read<std::uintptr_t>(entity+0x10);
    const auto handle=memory::read<std::uint32_t>(identity+0x10);
    const auto flags=memory::safe_read<std::uint32_t>(identity+0x30);
    if (!flags || !handle || handle==0xffffffff || systems::g_entities.lookup(handle)!=entity) return {};
    const auto serial=(handle>>15)-(*flags&1u);
    return {handle,(serial<<15)|(handle&0x7fffu)};
}
std::uint32_t entity_handle(std::uintptr_t entity) {return entity_identity(entity).raw;}
std::uintptr_t local_controller() {return memory::read<std::uintptr_t>(addresses::globals::local_player_controller);}
std::uintptr_t pawn_from_controller(std::uintptr_t controller) {
    const auto handle=field<std::uint32_t>(controller,"CBasePlayerController","m_hPawn");
    return handle ? systems::g_entities.lookup(*handle) : 0;
}
bool position(std::uintptr_t pawn,std::array<float,3>& output) {
    const auto node=field<std::uintptr_t>(pawn,"C_BaseEntity","m_pGameSceneNode");
    const auto origin=node ? field<math::vector3>(*node,"CGameSceneNode","m_vecAbsOrigin") : std::nullopt;
    if (!origin || !std::isfinite(origin->x) || !std::isfinite(origin->y) || !std::isfinite(origin->z)
        || std::abs(origin->x)>131072 || std::abs(origin->y)>131072 || std::abs(origin->z)>131072) return false;
    output={origin->x,origin->y,origin->z};return true;
}
bool tracer_origin(std::uintptr_t controller,std::array<float,3>& output,std::uint32_t& handle){
    const auto pawn=pawn_from_controller(controller);handle=entity_handle(pawn);
    const auto health=field<int>(pawn,"C_BaseEntity","m_iHealth");
    std::array<float,3> origin{};
    const auto offset=field<math::vector3>(pawn,"C_BaseModelEntity","m_vecViewOffset");
    if(!handle||!health||*health<=0||!offset||!position(pawn,origin)||
       !bullet_tracers::Valid({offset->x,offset->y,offset->z}))return false;
    const math::vector3 eye{origin[0]+offset->x,origin[1]+offset->y,origin[2]+offset->z};
    const auto services=field<std::uintptr_t>(pawn,"C_BasePlayerPawn","m_pWeaponServices").value_or(0);
    const auto weapon=field<std::uint32_t>(services,"CPlayer_WeaponServices","m_hActiveWeapon").value_or(0xffffffffu);
    math::vector3 muzzle{};
    if(weapon_fx::attachment_position(particles::attachment::barrel,handle,weapon,muzzle)&&
       (muzzle-eye).length_sqr()<256.f*256.f){output={muzzle.x,muzzle.y,muzzle.z};return true;}
    // A short right/down muzzle offset keeps first-person tracers visible;
    // an eye-to-impact line otherwise projects to a single crosshair point.
    if(!systems::g_view.has_camera())return false;
    const auto matrix=systems::g_view.matrix();
    const auto axis=[&](unsigned row)->std::optional<math::vector3>{
        const math::vector3 v{matrix[row][0],matrix[row][1],matrix[row][2]};
        const float length=std::sqrt(v.length_sqr());
        return std::isfinite(length)&&length>.0001f?std::optional{v*(1.f/length)}:std::nullopt;
    };
    const auto forward=axis(3),right=axis(0),up=axis(1);
    if(!forward||!right||!up)return false;
    const auto start=eye+*forward*12.f+*right*5.f-*up*4.f;
    output={start.x,start.y,start.z};return bullet_tracers::Valid({start.x,start.y,start.z});
}
std::uintptr_t event_entity(std::uintptr_t function,std::uintptr_t event,const char* name) noexcept {
    __try {
        const cstypes::event_hash key{0,name};
        return memory::call<std::uintptr_t>(function,event,&key);
    } __except(EXCEPTION_EXECUTE_HANDLER) {return 0;}
}
int event_integer(std::uintptr_t event,const char* name,int fallback=0) noexcept {
    __try {return memory::call<int>(int_getter,event,name,fallback);}
    __except(EXCEPTION_EXECUTE_HANDLER) {return fallback;}
}
float event_float(std::uintptr_t event,const char* name) noexcept {
    __try {return memory::call<float>(float_getter,event,name,0.0f);}
    __except(EXCEPTION_EXECUTE_HANDLER) {return std::numeric_limits<float>::quiet_NaN();}
}
bool event_boolean(std::uintptr_t event,const char* name) noexcept {
    __try {
        const cstypes::event_hash key{0,name};
        return memory::call_vfunc<bool>(event,6,&key,false);
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool add_listener(std::uintptr_t manager,listener* entry,const char* name) noexcept {
    __try {return memory::call_vfunc<bool>(manager,3,entry,name,false);}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool remove_listener(std::uintptr_t manager,listener* entry) noexcept {
    __try {memory::call_vfunc<void>(manager,5,entry);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
void* __fastcall listener_destroy(void* self,unsigned) {return self;}
int __fastcall listener_debug(void* self) {return static_cast<listener*>(self)->debug_id;}
void __fastcall fire_event(void* self,void* event) {
    callback_scope scope;
    const auto* entry=static_cast<listener*>(self);
    if (!stopping.load() && entry && entry->index<names.size()) {
        try {on_game_event(names[entry->index],reinterpret_cast<std::uintptr_t>(event));}
        catch (...) {std::scoped_lock lock(state_mutex);state="Combat event processing failed";}
    }
}
void* listener_table[]{reinterpret_cast<void*>(listener_destroy),reinterpret_cast<void*>(fire_event),reinterpret_cast<void*>(listener_debug)};

void clear_records() {
    std::scoped_lock lock(state_mutex);
    deaths.clear();tracers.Clear();movement={};pending_spawn={};chain=0;last_kill=0;last_feature.fill(0);
}
void unregister_all() {
    // The manager is a current-build global object. Do not call an old instance
    // if its pointer slot has changed during engine teardown.
    const auto current=memory::read<std::uintptr_t>(manager_slot);
    unsigned remaining{};
    for (auto& entry:listeners) {
        if (!entry.registered) continue;
        if (!current || current!=registered_manager || remove_listener(current,&entry)) entry.registered=false;
        else ++remaining;
    }
    registered_count.store(remaining);
    if (!remaining) registered_manager=0;
}
void register_all(ULONGLONG now) {
    if (registration_attempt && now-registration_attempt<1000) return;
    registration_attempt=now;
    const auto manager=memory::read<std::uintptr_t>(manager_slot);
    if (!manager) {std::scoped_lock lock(state_mutex);state="Combat events: manager unavailable";return;}
    if (registered_manager && registered_manager!=manager) unregister_all();
    registered_manager=manager;
    unsigned registered{};
    for (unsigned i=0;i<listeners.size();++i) {
        auto& entry=listeners[i];entry.table=listener_table;entry.debug_id=0x4c00+static_cast<int>(i);entry.index=i;
        if (!entry.registered) entry.registered=add_listener(manager,&entry,names[i]);
        if (entry.registered) ++registered;
    }
    registered_count.store(registered);
    std::scoped_lock lock(state_mutex);
    state=registered==names.size() ? "Combat event listeners connected"
        : "Combat events: "+std::to_string(registered)+" / "+std::to_string(names.size())+" listeners connected";
}
bool cooldown(effect_kind kind,ULONGLONG now,ULONGLONG interval) {
    const auto index=static_cast<std::size_t>(kind);
    std::scoped_lock lock(state_mutex);
    if (last_feature[index] && now-last_feature[index]<interval) return false;
    last_feature[index]=now;return true;
}
std::uint64_t emit(effect_kind kind,const std::array<float,3>& origin,float lifetime,ULONGLONG interval=0) {
    if (!cooldown(kind,GetTickCount64(),interval)) return 0;
    return particles::queue_feature_effect(kind,origin,lifetime);
}
void death(std::uintptr_t event,const configuration& cfg) {
    const auto local=local_controller();
    const auto attacker=event_entity(controller_getter,event,"attacker");
    const auto victim=event_entity(controller_getter,event,"userid");
    if (!local || attacker!=local || !victim || victim==local) return;
    auto pawn=event_entity(pawn_getter,event,"userid");
    if (!pawn) pawn=pawn_from_controller(victim);
    const auto identity=entity_identity(pawn);
    const auto handle=identity.raw;
    std::array<float,3> origin{};
    if (!handle || !position(pawn,origin)) return;
    const bool headshot=cfg.headshot_accent && event_boolean(event,"headshot");
    if (!cfg.kill_particles && !headshot) return;
    const auto now=GetTickCount64();
    std::uint64_t id{};
    unsigned crescendo{};
    {
        std::scoped_lock lock(state_mutex);
        if (std::ranges::any_of(deaths,[&](const death_record& item){return item.handle==handle && item.pawn==pawn && now<item.expires;})) return;
        if (deaths.size()>=max_deaths) return;
        const auto window=std::isfinite(cfg.multikill_window) ? std::clamp(cfg.multikill_window,0.5f,10.0f) : 4.0f;
        chain=last_kill && now-last_kill<=static_cast<ULONGLONG>(window*1000) ? std::min(chain+1,4u) : 1;
        last_kill=now;crescendo=cfg.multikill_crescendo ? chain-1 : 0;
        id=++next_record_id;
        deaths.push_back({id,0,victim,pawn,handle,identity.scene,now,now+120000,cfg.kill_particles,false});
    }
    std::uint64_t ticket{};
    if (cfg.kill_particles) ticket=particles::queue_kill_effect(cfg.kill_recipe,origin,cfg.kill_lifetime);
    if (headshot) {
        auto crown=origin;crown[2]+=64;
        const auto accent=emit(effect_kind::headshot,crown,std::min(cfg.kill_lifetime,3.0f));
        if (!cfg.kill_particles) ticket=accent;
    }
    if (ticket && crescendo) {
        for (unsigned i=0;i<crescendo;++i) {
            auto point=origin;point[2]+=24.0f*static_cast<float>(i+1);
            (void)particles::queue_kill_effect(2,point,cfg.kill_lifetime);
        }
    }
    {
        std::scoped_lock lock(state_mutex);
        const auto found=std::ranges::find_if(deaths,[&](const death_record& item){return item.id==id;});
        if (found!=deaths.end()) {
            if (ticket) {found->ticket=ticket;kill_count.fetch_add(1);}
            else {deaths.erase(found);state="Combat effects: native burst queue unavailable";}
        }
    }
}
void sample_movement(const configuration& cfg) {
    const auto pawn=pawn_from_controller(local_controller());
    const auto handle=entity_handle(pawn);
    const auto health=field<int>(pawn,"C_BaseEntity","m_iHealth");
    const auto flags=field<std::uint32_t>(pawn,"C_BaseEntity","m_fFlags");
    const auto velocity=field<math::vector3>(pawn,"C_BaseEntity","m_vecAbsVelocity");
    std::array<float,3> origin{};
    if (!handle || !health || *health<=0 || !flags || !position(pawn,origin)) {
        std::scoped_lock lock(state_mutex);movement={};return;
    }
    const bool grounded=(*flags&1u)!=0;
    bool landing{};
    {
        std::scoped_lock lock(state_mutex);
        landing=movement.valid && movement.pawn_handle==handle && !movement.grounded && grounded;
        movement={true,grounded,handle};
    }
    if (cfg.landing_particles && landing) (void)emit(effect_kind::landing,origin,1.0f,250);
    if (cfg.speed_trail && grounded && velocity && std::isfinite(velocity->x) && std::isfinite(velocity->y)) {
        const auto speed=std::hypot(velocity->x,velocity->y);
        const auto minimum=std::isfinite(cfg.trail_min_speed) ? std::clamp(cfg.trail_min_speed,0.0f,500.0f) : 180.0f;
        if (speed>=minimum) (void)emit(effect_kind::speed_trail,origin,0.8f,150);
    }
}
void process_spawn(const configuration& cfg,ULONGLONG now) {
    spawn_request request;
    {std::scoped_lock lock(state_mutex);request=pending_spawn;}
    if (!request.controller) return;
    if (!cfg.spawn_particles || now>=request.expires || local_controller()!=request.controller
        || entity_handle(request.controller)!=request.controller_handle) {
        std::scoped_lock lock(state_mutex);pending_spawn={};return;
    }
    const auto pawn=pawn_from_controller(request.controller);
    const auto handle=entity_handle(pawn);
    const auto health=field<int>(pawn,"C_BaseEntity","m_iHealth");
    std::array<float,3> origin{};
    if (!handle || (request.expected_pawn && request.expected_pawn!=handle)
        || !health || *health<=0 || !position(pawn,origin)) return;
    const auto ticket=emit(effect_kind::spawn,origin,2.5f,500);
    if (ticket) {std::scoped_lock lock(state_mutex);pending_spawn={};}
}
}

bool initialize() {
    stopping.store(false);ready.store(false);visibility_hook.store(false);
    scene_supported.store(false);kill_count.store(0);clear_records();registration_attempt=0;
    const bool client=game::module_matches("client.dll",hvh::client_sha256);
    if (!client || !game::module_matches("engine2.dll",engine_hash)) {
        std::scoped_lock lock(state_mutex);state="Combat effects: unsupported event build";return false;
    }
    scene_supported.store(game::module_matches("scenesystem.dll",scene_hash));
    auto expression=patterns::game_event_manager;
    if (expression.ends_with('~')) expression.remove_suffix(1);
    manager_slot=memory::resolve_pattern(expression);
    controller_getter=PATTERN(patterns::game_event_get_controller);
    pawn_getter=PATTERN(patterns::game_event_get_pawn);
    int_getter=PATTERN(patterns::game_event_get_int);
    float_getter=PATTERN(patterns::game_event_get_float);
    const bool connected=manager_slot && controller_getter && pawn_getter && int_getter && float_getter;
    ready.store(connected);
    std::scoped_lock lock(state_mutex);
    state=connected ? "Combat events: waiting for game-thread registration" : "Combat events: required binding unavailable";
    return connected;
}
void on_frame_stage(int stage) {
    if (stage!=7) return;
    if (stopping.load()) {unregister_all();clear_records();return;}
    if (!ready.load()) return;
    const auto now=GetTickCount64();
    if (registered_manager && memory::read<std::uintptr_t>(manager_slot)!=registered_manager) {
        unregister_all();registration_attempt=0;clear_records();
    }
    if (registered_count.load()!=names.size()) register_all(now);
    const auto cfg=get_configuration();
    if (!cfg.enabled) {clear_records();return;}
    if (!game::in_world()) {
        // A player_spawned notification may precede its new pawn's list entry.
        // Keep only this bounded request until the next valid game-thread tick.
        std::scoped_lock lock(state_mutex);deaths.clear();tracers.Clear();movement={};
        if (now>=pending_spawn.expires) pending_spawn={};
        return;
    }
    {
        std::scoped_lock lock(state_mutex);
        if(!cfg.bullet_tracers)tracers.Clear();
        else (void)tracers.Items(now);
        std::erase_if(deaths,[&](death_record& item){
            if ((item.primary && !cfg.kill_particles) || (!item.primary && !cfg.headshot_accent)) return true;
            if (now>=item.expires || systems::g_entities.lookup(item.handle)!=item.pawn) return true;
            const auto health=field<int>(item.pawn,"C_BaseEntity","m_iHealth");
            if (health && *health>0 && item.dead_observed) return true;
            if (health && *health<=0) item.dead_observed=true;
            if (!item.ticket) return now-item.created>1500;
            if (!item.spawned) {
                const auto receipt=particles::kill_effect_state(item.ticket);
                item.spawned=receipt==particles::burst_state::spawned;
                if (receipt==particles::burst_state::failed || receipt==particles::burst_state::expired) return true;
            }
            return false;
        });
    }
    process_spawn(cfg,now);
    if (cfg.landing_particles || cfg.speed_trail) sample_movement(cfg);
    else {std::scoped_lock lock(state_mutex);movement={};}
}
void on_game_event(std::string_view name,std::uintptr_t event) {
    if (!event || stopping.load() || !ready.load()) return;
    const auto local=local_controller();
    if(name=="round_start")weapon_fx::reset();
    if(local) {
        const auto user=event_entity(controller_getter,event,"userid");
        if(user==local) {
            if(name=="weapon_fire") weapon_fx::notify(weapon_fx::action::shot);
            if(name=="weapon_reload")weapon_fx::notify(weapon_fx::action::reload);
        }
        if((name=="player_death"||name=="player_hurt")&&user&&user!=local&&event_entity(controller_getter,event,"attacker")==local)
            weapon_fx::notify(name=="player_death"?weapon_fx::action::kill:weapon_fx::action::hit);
    }
    const auto cfg=get_configuration();
    if (!cfg.enabled) return;
    if (name=="round_start") {clear_records();return;}
    if (name=="player_death") {death(event,cfg);return;}
    if (!local) return;
    if (name=="player_hurt") {
        if (!cfg.hit_particles || event_entity(controller_getter,event,"attacker")!=local) return;
        const auto victim=event_entity(controller_getter,event,"userid");
        if (!victim || victim==local) return;
        auto pawn=event_entity(pawn_getter,event,"userid");
        if (!pawn) pawn=pawn_from_controller(victim);
        std::array<float,3> origin{};
        if (position(pawn,origin)) {origin[2]+=40;(void)emit(effect_kind::hit,origin,0.8f,50);}
        return;
    }
    if (name=="round_end") {
        if (!cfg.victory_particles) return;
        const auto pawn=pawn_from_controller(local);
        const auto team=field<std::uint8_t>(pawn,"C_BaseEntity","m_iTeamNum");
        const auto winner=event_integer(event,"winner");
        std::array<float,3> origin{};
        if (team && (*team==2 || *team==3) && winner==*team && position(pawn,origin)) {
            origin[2]+=80;(void)emit(effect_kind::victory,origin,4.0f,4000);
        }
        return;
    }
    const auto user=event_entity(controller_getter,event,"userid");
    if (!user) return;
    if (name=="player_spawned") {
        {std::scoped_lock lock(state_mutex);std::erase_if(deaths,[&](const death_record& item){return item.controller==user;});
            if (user==local) movement={};}
        if (user==local && cfg.spawn_particles) {
            const auto handle=entity_handle(local);
            const auto pawn=event_entity(pawn_getter,event,"userid");
            const auto health=field<int>(pawn,"C_BaseEntity","m_iHealth");
            const auto expected=health && *health>0 ? entity_handle(pawn) : 0;
            if (handle) {
                std::scoped_lock lock(state_mutex);pending_spawn={local,handle,expected,GetTickCount64()+1000};
            }
        }
    }
    if (user!=local) return;
    if(name=="weapon_fire"){
        if(cfg.bullet_tracers){
            std::array<float,3> origin{};std::uint32_t handle{};
            if(tracer_origin(local,origin,handle)){
                std::scoped_lock lock(state_mutex);tracers.Shot({origin[0],origin[1],origin[2]},handle,GetTickCount64());
            }
        }
        return;
    }
    if (name=="bullet_impact") {
        if (!cfg.impact_particles&&!cfg.bullet_tracers) return;
        const std::array<float,3> origin{event_float(event,"x"),event_float(event,"y"),event_float(event,"z")};
        if(!bullet_tracers::Valid({origin[0],origin[1],origin[2]}))return;
        if(cfg.bullet_tracers){
            std::array<float,3> start{};std::uint32_t handle{};
            if(tracer_origin(local,start,handle)){
                std::scoped_lock lock(state_mutex);
                (void)tracers.Impact({start[0],start[1],start[2]},{origin[0],origin[1],origin[2]},handle,GetTickCount64(),cfg.tracer_lifetime);
            }
        }
        if(cfg.impact_particles)
            (void)emit(effect_kind::impact,origin,0.65f,35);
        return;
    }
    std::array<float,3> origin{};
    if (!position(pawn_from_controller(local),origin)) return;
    if (name=="player_jump" && cfg.jump_particles) (void)emit(effect_kind::jump,origin,0.8f,150);
    if (name=="weapon_reload" && cfg.reload_particles) {origin[2]+=40;(void)emit(effect_kind::reload,origin,1.0f,300);}
}
void draw_tracers(const visuals::frame_snapshot& frame){
    if(stopping.load()||!ready.load())return;
    const auto cfg=get_configuration();
    if(!cfg.enabled||!cfg.bullet_tracers){std::scoped_lock lock(state_mutex);tracers.Clear();return;}
    if(!game::in_world()||!systems::g_view.has_camera())return;
    const auto now=GetTickCount64();std::vector<bullet_tracers::Line> lines;
    {std::scoped_lock lock(state_mutex);const auto items=tracers.Items(now);lines.assign(items.begin(),items.end());}
    const auto matrix=systems::g_view.matrix();auto *draw=ImGui::GetBackgroundDrawList();
    for(const auto& line:lines){
        const auto projected=bullet_tracers::Project(line.from,line.to,matrix.mat,frame.screen_width,frame.screen_height);
        if(!projected)continue;
        const ImVec2 from{projected->from.x,projected->from.y},to{projected->to.x,projected->to.y};
        const float alpha=bullet_tracers::Alpha(line,now,cfg.tracer_fade)*cfg.tracer_color[3];
        if(alpha<=0)continue;
        const auto color=[&](float scale){return ImGui::ColorConvertFloat4ToU32({cfg.tracer_color[0],cfg.tracer_color[1],cfg.tracer_color[2],alpha*scale});};
        if(cfg.tracer_style==1)for(int pass=3;pass>0;--pass)draw->AddLine(from,to,color(.09f),cfg.tracer_width+pass*3.f);
        if(cfg.tracer_style==2){
            const float dx=to.x-from.x,dy=to.y-from.y,length=std::hypot(dx,dy);
            if(length<.1f)continue;
            const float step=std::max(18.f,length/256.f);
            for(float at=0;at<length;at+=step){const float end=std::min(at+step*.6f,length);
                draw->AddLine({from.x+dx*at/length,from.y+dy*at/length},
                    {from.x+dx*end/length,from.y+dy*end/length},color(1),cfg.tracer_width);}
        }else draw->AddLine(from,to,color(1),cfg.tracer_width);
    }
}
void set_visibility_hook(bool installed) noexcept {visibility_hook.store(installed && scene_supported.load());}
bool hide_scene_object(std::uintptr_t scene_object) {
    if (stopping.load() || !visibility_hook.load() || !scene_object) return false;
    const auto cfg=get_configuration();
    if (!cfg.enabled || !cfg.kill_particles || !cfg.vanish_victim) return false;
    const auto handle=memory::read<std::uint32_t>(scene_object+0xc0);
    if (!handle || handle==0xffffffff) return false;
    std::scoped_lock lock(state_mutex);
    const auto now=GetTickCount64();
    for (auto& item:deaths) {
        if (!item.primary || !item.ticket || item.scene_handle!=handle || now>=item.expires
            || systems::g_entities.lookup(item.handle)!=item.pawn) continue;
        const auto identity=entity_identity(item.pawn);
        const auto health=field<int>(item.pawn,"C_BaseEntity","m_iHealth");
        if (identity.raw!=item.handle || identity.scene!=item.scene_handle || !health) continue;
        if (*health>0) {if (item.dead_observed) item.expires=0;continue;}
        item.dead_observed=true;
        if (!item.spawned) item.spawned=particles::kill_effect_spawned(item.ticket);
        if (item.spawned) return true;
    }
    return false;
}
void on_level_shutdown() {unregister_all();clear_records();registration_attempt=0;}
void request_shutdown() noexcept {stopping.store(true);}
bool shutdown_complete() noexcept {return registered_count.load()==0 && event_callbacks.load()==0;}
std::string status() {
    std::scoped_lock lock(state_mutex);
    return state+(visibility_hook.load() ? "; confirmed-victim removal connected" : "; victim removal unavailable");
}
unsigned confirmed_kills() noexcept {return kill_count.load();}
}
