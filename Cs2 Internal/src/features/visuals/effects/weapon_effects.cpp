#include "weapon_effects.hpp"
#include "config.hpp"
#include "features/visuals/chams.h"
#include "features/visuals/chams_materials.h"
#include "game/sdk/ChamsBindings.h"
#include <deque>

namespace custom::weapon_fx {
namespace {
using Address=std::uintptr_t;
using particles::attachment;
using particles::effect_kind;
constexpr std::array<particles::preset,6> choices{{
    {"Fire","particles/burning_fx/env_fire_small.vpcf",false,false},
    {"Electric","particles/generic_fx/fx_electric_arc_spark.vpcf",false,true},
    {"Sparks","particles/ambient_fx/env_sparks_omni.vpcf",false,true},
    {"Smoke","particles/burning_fx/gas_cannister_trail_smoke.vpcf",false,false},
    {"Icy mist","particles/explosions_fx/explosion_hegrenade_snow_flakes.vpcf",false,true},
    {"Stars","particles/ui/ui_gold_halo_sparkles.vpcf",false,true}
}};
constexpr const char* muzzle_resource="particles/unified_weapon_fx/uweapon_muzzleflash_pist_fire.vpcf";
constexpr const char* smoke_resource="particles/unified_weapon_fx/uweapon_muzzleflash_rifl_smoke.vpcf";
Address client{};
std::atomic<bool> active{},reactive{};
struct held_weapon {
    game_access::LocalPlayer local;
    Address weapon{},arms{};
    std::uint32_t handle{0xffffffffu};
    bool valid() const {return local.state==game_access::PlayerState::Alive && weapon && arms;}
};
template<class T> std::optional<T> field(Address object,const char* type,const char* name) {
    if(!object) return {};
    const auto offset=systems::schemas::lookup(type,fnv1a::runtime_hash(name));
    return offset ? memory::safe_read<T>(object+offset) : std::nullopt;
}
held_weapon read_held() {
    held_weapon held;held.local=game_access::ReadLocalPlayer(client);
    if(held.local.state!=game_access::PlayerState::Alive) return held;
    const auto services=field<Address>(held.local.pawn,"C_BasePlayerPawn","m_pWeaponServices").value_or(0);
    held.handle=field<std::uint32_t>(services,"CPlayer_WeaponServices","m_hActiveWeapon").value_or(0xffffffffu);
    held.weapon=game_access::Resolve(held.local.list,held.handle);
    const auto owner=field<std::uint32_t>(held.weapon,"C_BaseEntity","m_hOwnerEntity").value_or(0xffffffffu);
    if(game_access::Resolve(held.local.list,owner)!=held.local.pawn) {held.weapon=0;return held;}
    const auto arms=field<std::uint32_t>(held.local.pawn,"C_CSPlayerPawn","m_hHudModelArms").value_or(0xffffffffu);
    held.arms=game_access::Resolve(held.local.list,arms);
    return held;
}
bool finite_point(const math::vector3& point) {
    return std::isfinite(point.x)&&std::isfinite(point.y)&&std::isfinite(point.z)&&
        std::abs(point.x)<131072&&std::abs(point.y)<131072&&std::abs(point.z)<131072;
}
std::optional<math::vector3> eye_position(const held_weapon& held) {
    const auto node=field<Address>(held.local.pawn,"C_BaseEntity","m_pGameSceneNode").value_or(0);
    const auto origin=field<math::vector3>(node,"CGameSceneNode","m_vecAbsOrigin");
    const auto offset=field<math::vector3>(held.local.pawn,"C_BaseModelEntity","m_vecViewOffset");
    if(!origin||!offset||!finite_point(*origin)||!finite_point(*offset)) return {};
    return *origin+*offset;
}
math::vector3 transform(const math::matrix3x4& matrix,const std::array<float,3>& offset) {
    return {matrix[0][0]*offset[0]+matrix[0][1]*offset[1]+matrix[0][2]*offset[2]+matrix[0][3],
            matrix[1][0]*offset[0]+matrix[1][1]*offset[1]+matrix[1][2]*offset[2]+matrix[1][3],
            matrix[2][0]*offset[0]+matrix[2][1]*offset[1]+matrix[2][2]*offset[2]+matrix[2][3]};
}
struct anchors {
    math::matrix3x4 gun{};
    math::vector3 left{},right{};
    Address weapon_model{};
    ULONGLONG gun_time{},hand_time{};
    bool left_valid{},right_valid{};
};
struct signal {action kind;std::uint32_t pawn{},weapon{};ULONGLONG created{};unsigned emitted{};};
struct channel {std::uint64_t ticket{};ULONGLONG next{};int style{-1};};
std::mutex state_mutex;
held_weapon current;
anchors points;
std::deque<signal> signals;
std::array<ULONGLONG,5> pulses{};
std::array<channel,4> channels;
bool inspecting{},reloading{};
void clear_locked() {
    current={};points={};signals.clear();pulses.fill(0);
    inspecting=reloading=false;
}
bool same(const held_weapon& a,const held_weapon& b) {
    return a.local.list==b.local.list&&a.local.pawn==b.local.pawn&&a.local.handle==b.local.handle&&
        a.weapon==b.weapon&&a.handle==b.handle&&a.arms==b.arms;
}
void synchronize(const held_weapon& held) {
    if(!same(current,held)) {clear_locked();current=held;}
}
bool point_locked(attachment anchor,const configuration& cfg,ULONGLONG now,math::vector3& point) {
    switch(anchor) {
    case attachment::gun:case attachment::barrel:
        if(!points.gun_time||now<points.gun_time||now-points.gun_time>250) return false;
        point=transform(points.gun,anchor==attachment::gun?cfg.gun_offset:cfg.barrel_offset);break;
    case attachment::left_hand:case attachment::right_hand:
        if(!points.hand_time||now<points.hand_time||now-points.hand_time>250) return false;
        if(anchor==attachment::left_hand) {if(!points.left_valid)return false;point=points.left;}
        else {if(!points.right_valid)return false;point=points.right;}
        break;
    default:return false;
    }
    return finite_point(point);
}
std::uint64_t attached_burst(const char* resource,effect_kind kind,attachment anchor,float lifetime,const held_weapon& held) {
    math::vector3 point;
    if(!attachment_position(anchor,held.local.handle,held.handle,point)) return 0;
    particles::burst_request request{resource,{point.x,point.y,point.z},lifetime,kind};
    request.anchor=anchor;request.pawn_handle=held.local.handle;request.weapon_handle=held.handle;
    return particles::queue_burst(std::move(request));
}
void stop_channels() {
    for(auto& item:channels) {particles::cancel_burst(item.ticket);item={};}
}
void style_control(const char* label,int& selected,bool& changed) {
    selected=std::clamp(selected,0,static_cast<int>(choices.size()-1));
    if(ImGui::BeginCombo(label,choices[selected].label)) {
        for(unsigned i=0;i<choices.size();++i)
            if(ImGui::Selectable(choices[i].label,selected==static_cast<int>(i))) {selected=static_cast<int>(i);changed=true;}
        ImGui::EndCombo();
    }
}

}
void initialize() {client=reinterpret_cast<Address>(GetModuleHandleW(L"client.dll"));reset();}
void reset() {active=false;reactive=false;stop_channels();std::lock_guard lock(state_mutex);clear_locked();}
std::span<const particles::preset> styles() noexcept {return choices;}
bool reactive_enabled() noexcept {return reactive.load(std::memory_order_relaxed);}
void capture_scene(Address scene) {
    if(!active||!scene)return;
    const auto held=read_held();if(!held.valid())return;
    const auto owner=visuals::chams::ResolveSceneOwner(held.local.list,memory::read<std::uint32_t>(scene+chams_bindings::kSceneOwner));
    if(!owner)return;
    const auto name=memory::read_string(reinterpret_cast<Address>(systems::g_entities.get_schema_name(owner)),64);
    const bool arms=owner==held.arms&&name=="C_CS2HudModelArms";
    const auto parent=field<std::uint32_t>(owner,"C_BaseEntity","m_hOwnerEntity").value_or(0xffffffffu);
    const bool gun=name=="C_CS2HudModelWeapon"&&game_access::Resolve(held.local.list,parent)==held.weapon;
    if(!arms&&!gun)return;
    const auto eye=eye_position(held);if(!eye)return;
    const auto now=GetTickCount64();const auto cfg=get_configuration();
    anchors captured;
    if(gun) {
        const auto matrix=memory::safe_read<math::matrix3x4>(scene+chams_bindings::kSceneTransform);
        if(!matrix)return;
        const auto origin=transform(*matrix,{});
        if(!finite_point(origin)||(origin-*eye).length_sqr()>256*256)return;
        for(const auto& row:matrix->mat)for(float value:row)if(!std::isfinite(value)||std::abs(value)>131072)return;
        captured.gun=*matrix;captured.gun_time=now;captured.weapon_model=owner;
    } else {
        const auto count=memory::read<int>(scene+chams_bindings::kRenderBoneCount);
        const auto bones=memory::read<Address>(scene+chams_bindings::kRenderBones);
        if(count<=0||count>256||!bones)return;
        const auto bone_point=[&](int index,math::vector3& point){
            if(index<0||index>=count)return false;
            // Renderer bones are 48-byte matrices, unlike the 32-byte entity pose cache.
            const auto bone=memory::safe_read<math::matrix3x4>(bones+index*sizeof(math::matrix3x4));
            if(!bone)return false;
            point=transform(*bone,{});
            return finite_point(point)&&(point-*eye).length_sqr()<=256*256;
        };
        captured.left_valid=bone_point(cfg.left_hand_bone,captured.left);
        captured.right_valid=bone_point(cfg.right_hand_bone,captured.right);captured.hand_time=now;
    }
    std::lock_guard lock(state_mutex);synchronize(held);
    if(gun) {points.gun=captured.gun;points.gun_time=now;points.weapon_model=owner;}
    else {points.left=captured.left;points.right=captured.right;points.left_valid=captured.left_valid;points.right_valid=captured.right_valid;points.hand_time=now;}
}
bool attachment_position(attachment anchor,std::uint32_t pawn,std::uint32_t weapon,math::vector3& point) {
    const auto held=read_held();if(!held.valid()||held.local.handle!=pawn||held.handle!=weapon)return false;
    const auto cfg=get_configuration();if(!cfg.enabled||!cfg.weapon_effects)return false;
    std::lock_guard lock(state_mutex);
    return same(current,held)&&point_locked(anchor,cfg,GetTickCount64(),point);
}
bool local_weapon_model(Address entity) {
    const auto held=read_held();if(!held.valid())return false;
    const auto owner=field<std::uint32_t>(entity,"C_BaseEntity","m_hOwnerEntity").value_or(0xffffffffu);
    return entity==held.weapon||game_access::Resolve(held.local.list,owner)==held.weapon;
}
void notify(action event) {
    const auto index=static_cast<unsigned>(event);if(index>=pulses.size())return;
    const auto held=read_held();if(!held.valid())return;
    const auto cfg=get_configuration();if(!cfg.enabled||!cfg.weapon_effects)return;
    const auto now=GetTickCount64();
    std::lock_guard lock(state_mutex);synchronize(held);
    if(pulses[index]&&now-pulses[index]<static_cast<ULONGLONG>(event==action::shot?15:250))return;
    pulses[index]=now;
    if(signals.size()==32)signals.pop_front();
    signals.push_back({event,held.local.handle,held.handle,now});
}

visuals::color reactive_tint(visuals::color base) {
    const auto now=GetTickCount64();std::array<ULONGLONG,5> events;
    {std::lock_guard lock(state_mutex);events=pulses;}
    constexpr std::array<visuals::color,5> colors{{{255,190,65,255},{75,160,255,255},{140,100,255,255},{80,255,150,255},{225,90,255,255}}};
    constexpr std::array<float,5> duration{0.35f,1.2f,0.8f,0.45f,1.8f};
    float strength=0;unsigned selected=0;
    for(unsigned i=0;i<events.size();++i) {
        if(!events[i]||now<events[i])continue;
        const auto value=std::max(0.f,1-static_cast<float>(now-events[i])/(duration[i]*1000));
        if(value>strength){strength=value;selected=i;}
    }
    const auto blend=[&](std::uint8_t a,std::uint8_t b){return static_cast<std::uint8_t>(a+(b-a)*strength);};
    return {blend(base.r,colors[selected].r),blend(base.g,colors[selected].g),blend(base.b,colors[selected].b),base.a};
}
void update() {
    const auto cfg=get_configuration();active=cfg.enabled&&cfg.weapon_effects;reactive=active&&cfg.reactive_weapon_glow;
    const auto held=read_held();
    if(!active||!held.valid()) {reactive=false;stop_channels();std::lock_guard lock(state_mutex);clear_locked();return;}
    const bool inspect=field<bool>(held.weapon,"C_CSWeaponBase","m_bInspectPending").value_or(false)||
        field<bool>(held.weapon,"C_CSWeaponBase","m_bInspectShouldLoop").value_or(false);
    const bool reload=field<bool>(held.weapon,"C_CSWeaponBase","m_bInReload").value_or(false);
    bool inspect_edge{},reload_edge{};
    {std::lock_guard lock(state_mutex);synchronize(held);inspect_edge=inspect&&!inspecting;reload_edge=reload&&!reloading;inspecting=inspect;reloading=reload;}
    if(inspect_edge)notify(action::inspect);if(reload_edge)notify(action::reload);
    if(reactive)visuals::chams::materials::Ensure(visuals::chams::Material::Glow,false);
    const auto now=GetTickCount64();
    const bool enabled[]={cfg.gun_particles,cfg.barrel_particles,cfg.hand_particles,cfg.hand_particles};
    const int selected[]={cfg.gun_style,cfg.barrel_style,cfg.hand_style,cfg.hand_style};
    const attachment anchors[]={attachment::gun,attachment::barrel,attachment::left_hand,attachment::right_hand};
    const effect_kind kinds[]={effect_kind::gun,effect_kind::barrel,effect_kind::hands,effect_kind::hands};
    for(unsigned i=0;i<channels.size();++i) {
        auto& item=channels[i];
        if(!enabled[i]||item.style!=selected[i]) {particles::cancel_burst(item.ticket);item={};}
        if(!enabled[i])continue;
        const auto receipt=particles::kill_effect_state(item.ticket);
        if(item.ticket&&receipt==particles::burst_state::pending)continue;
        if(now<item.next)continue;
        particles::cancel_burst(item.ticket);
        const auto& style=choices[std::clamp(selected[i],0,5)];
        const auto interval=style.bursts?cfg.gun_interval:std::max(1.5f,cfg.gun_interval);
        item.ticket=attached_burst(style.resource,kinds[i],anchors[i],interval+.3f,held);
        item.style=selected[i];item.next=now+static_cast<ULONGLONG>((item.ticket?interval:.5f)*1000);
    }
    std::deque<signal> pending;
    {std::lock_guard lock(state_mutex);pending.swap(signals);}

    for(auto event:pending) {
        if(event.pawn!=held.local.handle||event.weapon!=held.handle||now-event.created>400)continue;
        const auto emit=[&](const char* resource,effect_kind kind,attachment anchor,float lifetime){return attached_burst(resource,kind,anchor,lifetime,held);};
        bool accepted=true;
        const auto layer=[&](unsigned bit,const char* resource,effect_kind kind,attachment anchor,float lifetime) {
            if(event.emitted&bit)return;
            if(emit(resource,kind,anchor,lifetime))event.emitted|=bit;else accepted=false;
        };
        if(event.kind==action::shot) {
            if(cfg.muzzle_flash)layer(1,muzzle_resource,effect_kind::muzzle,attachment::barrel,.25f);
            if(cfg.muzzle_smoke)layer(2,smoke_resource,effect_kind::gun_smoke,attachment::barrel,.7f);
        } else if(event.kind==action::inspect&&cfg.inspect_burst)layer(1,choices[cfg.gun_style].resource,effect_kind::inspect,attachment::gun,1.0f);
        else if(event.kind==action::reload&&cfg.reload_burst)layer(1,choices[cfg.barrel_style].resource,effect_kind::gun_reload,attachment::gun,1.0f);
        if(!accepted) {std::lock_guard lock(state_mutex);if(signals.size()<32)signals.push_back(event);}
    }
}
void draw_menu() {
    auto cfg=get_configuration();bool changed=ImGui::Checkbox("Enable weapon effects",&cfg.weapon_effects);
    if(changed&&cfg.weapon_effects)cfg.enabled=true;
    if(ImGui::Button("Enable all effects")) {
        cfg.enabled=cfg.weapon_effects=true;
        cfg.gun_particles=cfg.barrel_particles=cfg.hand_particles=true;
        cfg.inspect_burst=cfg.reload_burst=cfg.muzzle_flash=cfg.muzzle_smoke=true;
        cfg.reactive_weapon_glow=true;changed=true;
    }
    if(ImGui::CollapsingHeader("Continuous particles",ImGuiTreeNodeFlags_DefaultOpen)) {
        changed|=ImGui::Checkbox("Weapon particles",&cfg.gun_particles);if(cfg.gun_particles)style_control("Weapon style",cfg.gun_style,changed);
        changed|=ImGui::Checkbox("Barrel particles",&cfg.barrel_particles);if(cfg.barrel_particles)style_control("Barrel style",cfg.barrel_style,changed);
        changed|=ImGui::Checkbox("Hand particles",&cfg.hand_particles);if(cfg.hand_particles)style_control("Hand style",cfg.hand_style,changed);
        changed|=ImGui::SliderFloat("Burst interval",&cfg.gun_interval,.25f,3,"%.2f s");
        if(ImGui::TreeNode("Placement")) {
            changed|=ImGui::SliderFloat3("Weapon offset (X / Y / Z)",cfg.gun_offset.data(),-64,64,"%.1f");
            changed|=ImGui::SliderFloat3("Barrel offset (X / Y / Z)",cfg.barrel_offset.data(),-64,64,"%.1f");
            changed|=ImGui::SliderInt("Left hand bone",&cfg.left_hand_bone,0,255);
            changed|=ImGui::SliderInt("Right hand bone",&cfg.right_hand_bone,0,255);ImGui::TreePop();
        }
    }
    if(ImGui::CollapsingHeader("Action effects",ImGuiTreeNodeFlags_DefaultOpen)) {
        changed|=ImGui::Checkbox("Inspect burst",&cfg.inspect_burst);
        changed|=ImGui::Checkbox("Reload burst",&cfg.reload_burst);
        changed|=ImGui::Checkbox("Muzzle flash",&cfg.muzzle_flash);
        changed|=ImGui::Checkbox("Muzzle smoke",&cfg.muzzle_smoke);
        changed|=ImGui::Checkbox("Reactive weapon color",&cfg.reactive_weapon_glow);
        if(ImGui::IsItemHovered())ImGui::SetTooltip("Changes your held weapon's color after shots, reloads, inspections, hits and kills.");
    }
    if(changed)set_configuration(cfg);
}
}
