#include "penetrable_props.h"
#include "detail/penetrable_props_cache.hpp"
#include "chams_materials.h"
#include "features/combat/detail/hvh_input.hpp"
#include "game/sdk/ChamsBindings.h"

namespace visuals::penetrable_props {
namespace {
namespace policy=detail::penetrable_props;
std::mutex mutex;
policy::Cache<2048> cache;
policy::MapKey map;
unsigned candidateIndex{1},candidateSide{};
std::optional<policy::Stamp> candidate;
template<class T> std::optional<T> Field(std::uintptr_t object,const char* type,const char* name){
    if(!object)return {};
    const auto offset=systems::schemas::lookup(type,fnv1a::runtime_hash(name));
    return offset?memory::safe_read<T>(object+offset):std::nullopt;
}
std::optional<policy::Stamp> Read(policy::MapKey key,std::uintptr_t entity){
    const auto identity=memory::read<std::uintptr_t>(entity+0x10);
    const auto handle=memory::read<std::uint32_t>(identity+0x10);
    if(!entity||game_access::Resolve(key.list,handle)!=entity)return {};
    policy::Stamp value;value.map=key;value.handle=handle;value.entity=entity;
    value.node=Field<std::uintptr_t>(entity,"C_BaseEntity","m_pGameSceneNode").value_or(0);
    value.collision=Field<std::uintptr_t>(entity,"C_BaseEntity","m_pCollision").value_or(0);
    if(!value.node||!value.collision)return {};
    value.modelBinding=memory::read<std::uintptr_t>(value.node+game_access::field::CSkeletonInstance::m_modelState+
        game_access::field::CModelState::m_hModel);
    value.modelResource=memory::read<std::uintptr_t>(value.modelBinding);
    const auto origin=Field<math::vector3>(value.node,"CGameSceneNode","m_vecAbsOrigin");
    const auto angles=Field<math::vector3>(value.node,"CGameSceneNode","m_angAbsRotation");
    if(!origin||!angles||!value.modelBinding||!value.modelResource)return {};
    value.transform={{origin->x,origin->y,origin->z},{angles->x,angles->y,angles->z}};
    return value;
}
}
void BeginMap(std::uint64_t epoch,std::uintptr_t list){
    std::lock_guard lock(mutex);
    if(map!=policy::MapKey{epoch,list}){candidateIndex=1;candidateSide=0;candidate.reset();}
    map={epoch,list};cache.SetMap(map);
}
void Clear(){std::lock_guard lock(mutex);if(map.epoch||cache.Size())cache.Clear();map={};candidateIndex=1;candidateSide=0;candidate.reset();}
std::size_t Count(){std::lock_guard lock(mutex);return cache.Size();}
bool Discover(const autowall_probe::Frame& profile,float maximumDistance,float minimumDamage){
    if(!std::isfinite(maximumDistance)||maximumDistance<=0||!std::isfinite(minimumDamage)||minimumDamage<0)return false;
    policy::Stamp selected;unsigned side{};
    {
        std::lock_guard lock(mutex);
        if(!profile.valid||map!=policy::MapKey{profile.stamp.session,profile.stamp.list}||
           map.epoch!=hvh_input::worldEpoch.load())return false;
        // Revisit loaded slots without expiring selected props. New round
        // spawns/replacements are discovered even while standing still.
        if(!candidate&&candidateIndex>=0x7fff)candidateIndex=1;
        for(unsigned examined=0;!candidate&&candidateIndex<0x7fff&&examined<64;++examined){
            const auto entity=game_access::Entity(map.list,candidateIndex++);
            if(!entity)continue;
            const auto identity=memory::read<std::uintptr_t>(entity+0x10);
            const auto entityClass=memory::read<std::uintptr_t>(identity+8);
            const auto schema=memory::read<std::uintptr_t>(entityClass+0x58);
            const auto name=memory::read_string(memory::read<std::uintptr_t>(schema+8),96);
            // An explicit family allowlist keeps weapon/player/ragdoll models
            // out of geometry discovery before any native call is attempted.
            if(name.find("Prop")==std::string::npos&&name.find("Door")==std::string::npos&&
               name.find("Breakable")==std::string::npos&&name!="C_FuncBrush")continue;
            if(name.find("Ragdoll")!=std::string::npos)continue;
            const auto stamp=Read(map,entity);
            if(!stamp||!policy::Valid(*stamp)||cache.Matches(*stamp))continue;
            const auto& p=stamp->transform.origin;
            const auto dx=p[0]-profile.eyeOrigin.x,dy=p[1]-profile.eyeOrigin.y,dz=p[2]-profile.eyeOrigin.z;
            if(dx*dx+dy*dy+dz*dz>maximumDistance*maximumDistance)continue;
            candidate=*stamp;candidateSide=0;
        }
        if(!candidate)return false;
        selected=*candidate;side=candidateSide++;
        if(candidateSide==6)candidate.reset();
    }
    const auto current=Read(selected.map,selected.entity);
    if(!current||*current!=selected)return false;
    const auto mins=Field<math::vector3>(selected.collision,"CCollisionProperty","m_vecMins");
    const auto maxs=Field<math::vector3>(selected.collision,"CCollisionProperty","m_vecMaxs");
    if(!mins||!maxs)return false;
    const float low[]{mins->x,mins->y,mins->z},high[]{maxs->x,maxs->y,maxs->z};
    float center[3]{},radiusSquared{};
    for(unsigned i=0;i<3;++i){
        if(!std::isfinite(low[i])||!std::isfinite(high[i])||low[i]>high[i]||std::abs(low[i])>8192||std::abs(high[i])>8192)return false;
        center[i]=(low[i]+high[i])*.5f;
        const auto extent=(high[i]-low[i])*.5f;radiusSquared+=extent*extent;
    }
    const float radius=std::sqrt(radiusSquared)+8.f;
    math::vector3 forward,right,up;
    const auto& angles=selected.transform.angles;
    math::helpers::angle_vectors_left({angles[0],angles[1],angles[2]},&forward,&right,&up);
    const auto& p=selected.transform.origin;
    const math::vector3 target{p[0]+forward.x*center[0]-right.x*center[1]+up.x*center[2],
        p[1]+forward.y*center[0]-right.y*center[1]+up.y*center[2],
        p[2]+forward.z*center[0]-right.z*center[1]+up.z*center[2]};
    constexpr float directions[6][3]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    const auto& d=directions[side];
    const math::vector3 origin{target.x+d[0]*radius,target.y+d[1]*radius,target.z+d[2]*radius};
    const auto result=autowall_probe::ProbeRay(profile,origin,{-d[0],-d[1],-d[2]});
    if(result.state==autowall_probe::State::Pass&&result.hitEntity==selected.entity&&result.hitHandle==selected.handle&&
       std::isfinite(result.damage)&&result.damage>=minimumDamage&&Read(selected.map,selected.entity)==current){
        Remember(result);
        std::lock_guard lock(mutex);if(candidate&&candidate->handle==selected.handle)candidate.reset();
    }
    return true;
}
void Remember(const autowall_probe::Result& result){
    if(result.state!=autowall_probe::State::Pass||result.kind!=autowall_probe::SurfaceKind::Prop||
       !autowall_probe::IsSurfaceCurrent(result))return;
    const auto stamp=Read({result.stamp.session,result.stamp.list},result.hitEntity);
    if(!stamp||stamp->handle!=result.hitHandle||stamp->node!=result.sceneNode||
       stamp->transform.origin!=std::array{result.entityOrigin.x,result.entityOrigin.y,result.entityOrigin.z}||
       stamp->transform.angles!=std::array{result.entityRotation.x,result.entityRotation.y,result.entityRotation.z})return;
    std::lock_guard lock(mutex);
    if(map.epoch==hvh_input::worldEpoch.load(std::memory_order_acquire))cache.Remember(*stamp,true);
}
bool Generate(std::uintptr_t self,std::uintptr_t scene,std::uintptr_t view,
              std::uintptr_t output,chams::GenerateFn original,color tint){
    policy::MapKey key;
    {std::lock_guard lock(mutex);if(!cache.Size())return false;key=map;}
    if(key.epoch!=hvh_input::worldEpoch.load(std::memory_order_acquire))return false;
    const auto owner=chams::ResolveSceneOwner(key.list,memory::read<std::uint32_t>(scene+chams_bindings::kSceneOwner));
    const auto current=Read(key,owner);if(!current)return false;
    {std::lock_guard lock(mutex);if(!cache.Matches(*current))return false;}
    // One depth-tested Flat pass; never mutate a shared engine material or
    // infer anonymous world ownership from the C_World entity.
    const auto material=chams::materials::Find(chams::Material::Flat,false);
    struct Guard {policy::Stamp stamp;std::uintptr_t scene;};
    Guard guard{*current,scene};
    const auto accepts=[](void* context){
        const auto& value=*static_cast<Guard*>(context);
        if(value.stamp.map.epoch!=hvh_input::worldEpoch.load(std::memory_order_acquire))return false;
        const auto owner=chams::ResolveSceneOwner(value.stamp.map.list,
            memory::read<std::uint32_t>(value.scene+chams_bindings::kSceneOwner));
        if(owner!=value.stamp.entity)return false;
        const auto now=Read(value.stamp.map,owner);
        if(!now||*now!=value.stamp)return false;
        std::lock_guard lock(mutex);return cache.Matches(*now);
    };
    return material&&chams::ApplyImageChecked(self,scene,view,output,original,material,tint,accepts,&guard);
}
}
