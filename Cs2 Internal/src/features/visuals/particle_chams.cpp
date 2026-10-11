#include "core/user_defaults.hpp"
#include "features/visuals/engine.hpp"
#include "particle_chams.h"
#include "effects/particles.hpp"
#include <unordered_set>

namespace visuals::particle_chams {
namespace {
using Address = std::uintptr_t;
namespace particles = custom::particles;
constexpr std::array<const char *, 4> resources{
    "particles/ambient_fx/env_sparks_omni.vpcf", "particles/generic_fx/fx_electricspark_glow.vpcf",
    "particles/generic_fx/fx_electric_arc_spark.vpcf", "particles/ui/ui_gold_halo_sparkles.vpcf"};
constexpr std::array<const char *, 4> labels{"Orbiting sparks", "Electric glow", "Electric arcs", "Star glimmer"};
constexpr std::array<unsigned, 3> bones{cstypes::bone_ids::head,cstypes::bone_ids::spine_4,cstypes::bone_ids::pelvis};
std::mutex settingsMutex, statusMutex;
Settings settings=user_defaults::Particles(Settings{});
std::atomic<bool> enabled{};
std::atomic<std::uint64_t> revision{1};
std::string message{"Particle chams disabled"};
struct Identity {
    Address pawn{}, scene{}, binding{}, model{};
    std::uint32_t modelName{};
    bool operator==(const Identity &) const = default;
};
struct Channel { std::uint64_t ticket{}; ULONGLONG next{}; };
struct State {
    Identity identity;
    std::array<math::vector3,3> points{};
    std::array<bool,3> valid{};
    std::array<Channel,3> channels{};
};
struct Candidate { std::uint32_t handle{}; State state; float distance{}; };
// Only accessed by the native frame-stage thread and its CP0 updates.
std::unordered_map<std::uint32_t,State> states;
std::uint64_t appliedRevision{};
float Bounded(float value,float low,float high,float fallback) { return std::isfinite(value)?std::clamp(value,low,high):fallback; }
bool Finite(const math::vector3 &p) {
    return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&
        std::abs(p.x)<131072&&std::abs(p.y)<131072&&std::abs(p.z)<131072;
}
float Distance(const math::vector3 &a,const math::vector3 &b) {
    const float x=a.x-b.x,y=a.y-b.y,z=a.z-b.z;return x*x+y*y+z*z;
}
template<class T> std::optional<T> Field(Address object,const char *type,const char *name) {
    if(!object)return {};
    const auto offset=systems::schemas::lookup(type,fnv1a::runtime_hash(name));
    return offset?memory::safe_read<T>(object+offset):std::nullopt;
}
std::optional<Identity> ReadIdentity(Address pawn) {
    Identity result;result.pawn=pawn;
    result.scene=Field<Address>(pawn,"C_BaseEntity","m_pGameSceneNode").value_or(0);
    if(!result.scene||Field<bool>(result.scene,"CGameSceneNode","m_bDormant").value_or(true))return {};
    result.binding=memory::read<Address>(result.scene+game_access::field::CSkeletonInstance::m_modelState+
                                         game_access::field::CModelState::m_hModel);
    result.model=memory::read<Address>(result.binding);
    if(!result.binding||!result.model)return {};
    const auto name=memory::read_string(memory::read<Address>(result.model+skin_bindings::kModelNameOffset),512);
    result.modelName=fnv1a::runtime_hash(name.c_str());
    return result;
}
std::optional<Candidate> ReadCandidate(const game_access::LocalPlayer &local,std::uint32_t handle,
                                       const Settings &cfg,const math::vector3 &view) {
    const auto pawn=game_access::Resolve(local.list,handle);
    const auto health=Field<int>(pawn,"C_BaseEntity","m_iHealth").value_or(0);
    const auto life=Field<std::uint8_t>(pawn,"C_BaseEntity","m_lifeState").value_or(255);
    const auto team=Field<std::uint8_t>(pawn,"C_BaseEntity","m_iTeamNum").value_or(0);
    if(!pawn||health<=0||life!=0||(team!=2&&team!=3))return {};
    const bool isLocal=pawn==local.pawn;
    if(isLocal?!cfg.local:(team==local.team?!cfg.teammates:!cfg.enemies))return {};
    const auto identity=ReadIdentity(pawn);if(!identity)return {};
    const auto origin=Field<math::vector3>(identity->scene,"CGameSceneNode","m_vecAbsOrigin");
    if(!origin||!Finite(*origin))return {};
    Candidate result;result.handle=handle;result.state.identity=*identity;result.distance=Distance(*origin,view);
    const float maxDistance=cfg.distance/.01905f;
    if(result.distance>maxDistance*maxDistance)return {};
    const auto skeleton=systems::g_bones.get_skeleton(pawn);
    unsigned valid{};
    for(unsigned i=0;i<bones.size();++i) {
        const auto &bone=skeleton[bones[i]];
        if(!Finite(bone.position)||!std::isfinite(bone.scale)||bone.scale<=0||Distance(bone.position,*origin)>160*160)continue;
        result.state.points[i]=bone.position;result.state.valid[i]=true;++valid;
    }
    if(!valid||ReadIdentity(pawn)!=identity||game_access::Resolve(local.list,handle)!=pawn)return {};
    return result;
}
void Cancel(State &state) { for(auto &channel:state.channels){particles::cancel_burst(channel.ticket);channel={};} }
void SetStatus(std::string text) { std::lock_guard lock(statusMutex);message=std::move(text); }
}
Settings Get(){std::lock_guard lock(settingsMutex);return settings;}
void Save(Settings value) {
    value.style=std::clamp(value.style,0,static_cast<int>(resources.size()-1));value.players=std::clamp(value.players,1,6);
    value.radius=Bounded(value.radius,2,30,9);value.orbitSpeed=Bounded(value.orbitSpeed,0,2,.4f);
    value.interval=Bounded(value.interval,.3f,1.5f,.7f);value.distance=Bounded(value.distance,5,200,80);
    std::lock_guard lock(settingsMutex);
    if(settings==value)return;
    settings=value;enabled.store(value.enabled,std::memory_order_release);revision.fetch_add(1,std::memory_order_release);
}
bool Enabled() noexcept {return enabled.load(std::memory_order_acquire);}
std::string Status(){std::lock_guard lock(statusMutex);return message;}
void Reset() {for(auto &[handle,state]:states)Cancel(state);states.clear();SetStatus("Particle chams disabled");}
void Update() {
    const auto currentRevision=revision.load(std::memory_order_acquire);const auto cfg=Get();
    if(appliedRevision!=currentRevision){Reset();appliedRevision=currentRevision;}
    if(!cfg.enabled){Reset();return;}
    const auto local=game_access::ReadLocalPlayer(game_access::verifiedClient.load(std::memory_order_acquire));
    const auto localScene=Field<Address>(local.pawn,"C_BaseEntity","m_pGameSceneNode").value_or(0);
    const auto view=Field<math::vector3>(localScene,"CGameSceneNode","m_vecAbsOrigin");
    if(!game::in_world()||!local.list||!local.pawn||(local.team!=2&&local.team!=3)||!view||!Finite(*view)) {
        Reset();SetStatus("Particle chams waiting for a player and map");return;
    }
    std::vector<Candidate> candidates;
    std::unordered_set<std::uint32_t> seen;
    const auto append=[&](std::uint32_t handle){
        if(!seen.insert(handle).second)return;
        if(auto candidate=ReadCandidate(local,handle,cfg,*view))candidates.push_back(std::move(*candidate));
    };
    if(cfg.local)append(local.handle);
    for(unsigned index=1;index<=64;++index) {
        const auto controller=game_access::Entity(local.list,index);
        const auto handle=Field<std::uint32_t>(controller,"CCSPlayerController","m_hPlayerPawn");
        if(handle)append(*handle);
    }
    std::sort(candidates.begin(),candidates.end(),[](const auto &a,const auto &b){
        return a.distance==b.distance?a.handle<b.handle:a.distance<b.distance;
    });
    if(candidates.size()>static_cast<unsigned>(cfg.players))candidates.resize(cfg.players);
    seen.clear();for(const auto &candidate:candidates)seen.insert(candidate.handle);
    for(auto it=states.begin();it!=states.end();) {
        if(seen.contains(it->first)){++it;continue;}
        Cancel(it->second);it=states.erase(it);
    }
    const auto now=GetTickCount64();unsigned spawned{},pending{},failed{};
    for(auto &candidate:candidates) {
        auto &state=states[candidate.handle];
        if(state.identity!=candidate.state.identity){Cancel(state);state=candidate.state;}
        for(unsigned anchor=0;anchor<bones.size();++anchor) {
            state.valid[anchor]=candidate.state.valid[anchor];
            if(!state.valid[anchor]){particles::cancel_burst(state.channels[anchor].ticket);state.channels[anchor]={};continue;}
            const auto phase=static_cast<float>(now%60000)*.001f*cfg.orbitSpeed*2*std::numbers::pi_v<float>
                +anchor*2*std::numbers::pi_v<float>/3+(candidate.handle&0x7fff)*.11f;
            const float radius=cfg.radius*(anchor==0?.55f:1.f);
            const auto &point=candidate.state.points[anchor];
            state.points[anchor]={point.x+std::cos(phase)*radius,point.y+std::sin(phase)*radius,point.z+(anchor==0?3.f:0.f)};
            auto &channel=state.channels[anchor];
            const auto receipt=particles::kill_effect_state(channel.ticket);
            if(channel.ticket&&receipt==particles::burst_state::pending){++pending;continue;}
            if(channel.ticket&&receipt==particles::burst_state::spawned&&now<channel.next){++spawned;continue;}
            if(now<channel.next){++failed;continue;}
            particles::cancel_burst(channel.ticket);
            const auto &position=state.points[anchor];
            particles::burst_request request{resources[cfg.style],{position.x,position.y,position.z},cfg.interval+.15f,
                                             particles::effect_kind::body_aura};
            request.anchor=particles::attachment::body;request.pawn_handle=candidate.handle;request.body_anchor=anchor;
            channel.ticket=particles::queue_burst(std::move(request));
            channel.next=now+static_cast<ULONGLONG>((channel.ticket?cfg.interval:1.f)*1000);
            channel.ticket?++pending:++failed;
        }
    }
    if(states.empty())SetStatus("No eligible living players / skeleton anchors in range");
    else SetStatus("Body aura: "+std::to_string(states.size())+" players, "+std::to_string(spawned)+
        " active emitters, "+std::to_string(pending)+" pending"+(failed?", retrying unavailable emitters":""));
}
bool AttachmentPosition(std::uint32_t handle,unsigned anchor,math::vector3 &point) {
    if(!Enabled()||anchor>=bones.size())return false;
    const auto found=states.find(handle);if(found==states.end()||!found->second.valid[anchor])return false;
    const auto local=game_access::ReadLocalPlayer(game_access::verifiedClient.load(std::memory_order_acquire));
    const auto &identity=found->second.identity;
    if(game_access::Resolve(local.list,handle)!=identity.pawn||ReadIdentity(identity.pawn)!=std::optional<Identity>(identity)||
       Field<int>(identity.pawn,"C_BaseEntity","m_iHealth").value_or(0)<=0||
       Field<std::uint8_t>(identity.pawn,"C_BaseEntity","m_lifeState").value_or(255)!=0)return false;
    point=found->second.points[anchor];return Finite(point);
}
void DrawMenu() {
    auto cfg=Get();bool changed=ImGui::Checkbox("Enable body aura",&cfg.enabled);
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Particle emitters follow the head, chest and hips.");
    changed|=ImGui::Checkbox("Enemies##particle_chams",&cfg.enemies);ImGui::SameLine();
    changed|=ImGui::Checkbox("Teammates##particle_chams",&cfg.teammates);ImGui::SameLine();
    changed|=ImGui::Checkbox("Local player##particle_chams",&cfg.local);
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Follows your world model; most visible in third person.");
    changed|=ImGui::Combo("Aura style",&cfg.style,labels.data(),static_cast<int>(labels.size()));
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Colors are set by the selected particle effect.");
    changed|=ImGui::SliderInt("Maximum players",&cfg.players,1,6);
    changed|=ImGui::SliderFloat("Aura radius",&cfg.radius,2,30,"%.0f units");
    changed|=ImGui::SliderFloat("Orbit speed",&cfg.orbitSpeed,0,2,"%.2f turns/s");
    changed|=ImGui::SliderFloat("Burst interval",&cfg.interval,.3f,1.5f,"%.2f s");
    changed|=ImGui::SliderFloat("Aura distance",&cfg.distance,5,200,"%.0f m");
    if(changed)Save(cfg);
}
}
