#include "game/autowall_probe.h"
#include "game/sdk/CombatBindings.h"

// Shared native read adapters; this backend applies no input or shot edits.
namespace combat_access {
float Convar(const char *name,float fallback);
bool ReadShootPosition(game_access::Address pawn,math::vector3 &position);
bool ReadShotPosition(game_access::Address pawn,combat_bindings::HistoryShotTiming timing,math::vector3 &position);
}

namespace autowall_probe {
namespace {
using Address = game_access::Address;
constexpr int maximumCapacity = 65536;
constexpr float maximumRay = 32768.f;
// Resolve the storage address, then read the live manager on every frame.
constexpr std::string_view managerPattern = "client.dll:488B0D*????????488D3452";
struct NativeApi {
    Address filter{}, ray{}, managerSlot{}, setup{}, bullet{}, reset{};
    ULONGLONG nextRetry{};
} api;
struct SurfaceElement { std::byte data[0x38]; };
struct TraceData {
    int surfaces{};
    float unknown{52};
    void *surfaceArray{};
    int surfaceCapacity{128}, surfaceFlags{static_cast<int>(0x80000000u)};
    std::array<SurfaceElement,128> inlineSurfaces{};
    std::byte padding[8]{};
    int contacts{}, unknownCount{};
    void *contactArray{};
    int contactCapacity{8}, contactFlags{static_cast<int>(0x80000000u)};
    std::byte inlineContacts[0xc0]{};
    Vec3 start{}, delta{};
    float fraction{1};
    bool unknownFlag{};
    std::byte tail[0x4b]{};
};
struct Contact {
    float enterFraction{}, exitFraction{}, damage{};
    int team{};
    std::uint16_t enter{}, exit{};
    std::uint8_t flags{}, padding[3]{};
};
static_assert(sizeof(Contact)==0x18 && offsetof(Contact,flags)==0x14);
static_assert(sizeof(TraceData)==0x1d60 && offsetof(TraceData,contacts)==0x1c20 && offsetof(TraceData,contactArray)==0x1c28);
static_assert(offsetof(systems::tracing::result,end_pos)==0x84 && offsetof(systems::tracing::result,normal)==0x90);
TraceData storage;
std::mutex mutex;
struct Weapon {
    Address address{}, data{};
    std::uint32_t handle{0xffffffffu};
    float damage{}, penetration{}, range{}, rangeModifier{};
};
bool Finite(const Vec3 &p) { return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z); }
bool WorldPoint(const Vec3 &p) {
    return Finite(p)&&std::abs(p.x)<131072&&std::abs(p.y)<131072&&std::abs(p.z)<131072;
}
bool Normalize(Vec3 &p) {
    const float length=p.length_sqr();
    if(!std::isfinite(length)||length<1e-10f)return false;
    p=p/std::sqrt(length);return Finite(p);
}
bool CompatibleClient() {
    const auto client=game_access::GameplayClient();
    return client&&client==reinterpret_cast<Address>(GetModuleHandleA("client.dll"));
}
template<class T> std::optional<T> Field(Address object,const char *type,const char *name) {
    const auto offset=systems::schemas::lookup(type,fnv1a::runtime_hash(name));
    return offset&&game_access::Valid(object,static_cast<std::size_t>(offset)+sizeof(T))
        ?memory::safe_read<T>(object+offset):std::nullopt;
}
bool ResolveBindings() {
    if(!CompatibleClient())return false;
    const auto now=GetTickCount64();
    if(now>=api.nextRetry) {
        api.nextRetry=now+500;
        auto resolve=[](Address &address,std::string_view pattern){if(!address)address=memory::resolve_cached(pattern);};
        namespace p=combat_bindings::patterns;
        resolve(api.filter,p::trace_filter_init);resolve(api.ray,patterns::trace_ray);
        resolve(api.managerSlot,managerPattern);resolve(api.setup,p::trace_bullet_data_init);
        resolve(api.bullet,p::trace_bullet);resolve(api.reset,p::trace_bullet_data_reset);
    }
    return api.filter&&api.ray&&api.managerSlot&&api.setup&&api.bullet&&api.reset;
}
bool Camera(const math::matrix4x4 &m,Vec3 &origin,Vec3 &direction,float ndcX=0,float ndcY=0) {
    if(!std::isfinite(ndcX)||!std::isfinite(ndcY)||std::abs(ndcX)>1||std::abs(ndcY)>1)return false;
    for(const auto &row:m.mat)for(float value:row)if(!std::isfinite(value))return false;
    const Vec3 a{m.mat[0][0],m.mat[0][1],m.mat[0][2]};
    const Vec3 b{m.mat[1][0],m.mat[1][1],m.mat[1][2]};
    const Vec3 c{m.mat[3][0],m.mat[3][1],m.mat[3][2]};
    const auto bc=b.cross(c),ca=c.cross(a),ab=a.cross(b);
    const double denominator=a.dot(bc);
    const double scale=std::sqrt(static_cast<double>(a.length_sqr())*b.length_sqr()*c.length_sqr());
    if(!std::isfinite(scale)||scale<1e-10||std::abs(denominator)<scale*1e-6)return false;
    origin=(bc*(-m.mat[0][3])+ca*(-m.mat[1][3])+ab*(-m.mat[3][3]))/static_cast<float>(denominator);
    direction=(a-c*ndcX).cross(b-c*ndcY);
    if(!WorldPoint(origin)||!Normalize(direction))return false;
    const float facing=c.dot(direction);
    if(!std::isfinite(facing)||std::abs(facing)<1e-6f)return false;
    if(facing<0)direction=-direction;
    return true;
}
std::optional<Vec3> Eye(const game_access::LocalPlayer &local) {
    const auto scene=Field<Address>(local.pawn,"C_BaseEntity","m_pGameSceneNode");
    const auto origin=scene?Field<Vec3>(*scene,"CGameSceneNode","m_vecAbsOrigin"):std::nullopt;
    const auto offset=Field<Vec3>(local.pawn,"C_BaseModelEntity","m_vecViewOffset");
    if(!origin||!offset)return {};
    const auto eye=*origin+*offset;return WorldPoint(eye)?std::optional<Vec3>{eye}:std::nullopt;
}
Reason ReadWeapon(const game_access::LocalPlayer &local,Weapon &weapon) {
    const auto services=Field<Address>(local.pawn,"C_BasePlayerPawn","m_pWeaponServices");
    const auto handle=services?Field<std::uint32_t>(*services,"CPlayer_WeaponServices","m_hActiveWeapon"):std::nullopt;
    if(!handle)return Reason::InvalidWeapon;
    weapon.handle=*handle;weapon.address=game_access::Resolve(local.list,*handle);
    if(!weapon.address)return Reason::InvalidWeapon;
    const auto subclass=systems::schemas::lookup("C_BaseEntity","m_nSubclassID"_hash);
    const auto data=subclass&&game_access::Valid(weapon.address,static_cast<std::size_t>(subclass)+16)
        ?memory::read<Address>(weapon.address+subclass+8):0;
    if(!data)return Reason::InvalidWeapon;
    weapon.data=data;
    const auto type=Field<int>(data,"CCSWeaponBaseVData","m_WeaponType");
    if(!type)return Reason::InvalidWeapon;
    if(*type<1||*type>6)return Reason::UnsupportedWeapon;
    const auto damage=Field<int>(data,"CCSWeaponBaseVData","m_nDamage");
    const auto penetration=Field<float>(data,"CCSWeaponBaseVData","m_flPenetration");
    const auto range=Field<float>(data,"CCSWeaponBaseVData","m_flRange");
    const auto modifier=Field<float>(data,"CCSWeaponBaseVData","m_flRangeModifier");
    if(!damage||!penetration||!range||!modifier||*damage<=0||*damage>10000||
       !std::isfinite(*penetration)||*penetration<0||*penetration>100||
       !std::isfinite(*range)||*range<=0||*range>maximumRay||
       !std::isfinite(*modifier)||*modifier<0||*modifier>2)return Reason::InvalidWeapon;
    weapon.damage=static_cast<float>(*damage);weapon.penetration=*penetration;
    weapon.range=*range;weapon.rangeModifier=*modifier;return Reason::Unavailable;
}
__declspec(noinline) bool Ray(Address pawn,const Vec3 &start,const Vec3 &end,systems::tracing::result &result) {
    __try {
        const auto manager=memory::read<Address>(api.managerSlot);if(!manager)return false;
        systems::tracing::filter filter{};systems::tracing::ray ray{};
        memory::call<void>(api.filter,&filter,pawn,std::uint64_t{0x1c300b},std::uint8_t{3},15);
        // Native false means clear (fraction1), not a failed call.
        memory::call<bool>(api.ray,manager,&ray,&start,&end,&filter,&result);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
void ClearTrace() {
    const auto surfaces=storage.surfaceArray,contacts=storage.contactArray;
    const int surfaceCapacity=storage.surfaceCapacity,surfaceFlags=storage.surfaceFlags;
    const int contactCapacity=storage.contactCapacity,contactFlags=storage.contactFlags;
    storage={};storage.surfaceArray=surfaces;storage.surfaceCapacity=surfaceCapacity;storage.surfaceFlags=surfaceFlags;
    storage.contactArray=contacts;storage.contactCapacity=contactCapacity;storage.contactFlags=contactFlags;
    if(!storage.surfaceArray)storage.surfaceArray=storage.inlineSurfaces.data();
    if(!storage.contactArray)storage.contactArray=storage.inlineContacts;
}
bool ValidTrace() {
    const auto &t=storage;
    if(t.surfaceCapacity<=0||t.surfaceCapacity>maximumCapacity||t.contactCapacity<=0||t.contactCapacity>maximumCapacity||
       t.surfaces<0||t.surfaces>t.surfaceCapacity||t.contacts<0||t.contacts>t.contactCapacity)return false;
    if(t.surfaceArray==t.inlineSurfaces.data()&&t.surfaceCapacity>static_cast<int>(t.inlineSurfaces.size()))return false;
    if(t.contactArray==t.inlineContacts&&t.contactCapacity>8)return false;
    return (t.surfaces==0||game_access::Valid(reinterpret_cast<Address>(t.surfaceArray),static_cast<std::size_t>(t.surfaces)*sizeof(SurfaceElement)))&&
           (t.contacts==0||game_access::Valid(reinterpret_cast<Address>(t.contactArray),static_cast<std::size_t>(t.contacts)*sizeof(Contact)));
}
__declspec(noinline) bool Setup(Address pawn,const Vec3 &eye,const Vec3 &delta) {
    __try {
        systems::tracing::filter filter{};
        memory::call<void>(api.filter,&filter,pawn,std::uint64_t{0x1c300b},std::uint8_t{3},15);
        memory::call<void>(api.setup,&storage,eye,delta,filter,4,true);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
__declspec(noinline) bool Simulate(const Weapon &weapon,std::uint8_t team) {
    __try {
        memory::call<void>(api.bullet,&storage,weapon.damage,weapon.penetration,weapon.rangeModifier,4,static_cast<int>(team),Address{});
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
__declspec(noinline) bool Cleanup() {
    __try {memory::call<void>(api.reset,&storage);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool ReadContact(int index,Contact &contact) {
    if(index<0||index>=storage.contacts||!memory::copy(&contact,reinterpret_cast<Address>(storage.contactArray)+static_cast<Address>(index)*sizeof(Contact),sizeof(Contact)))return false;
    return std::isfinite(contact.enterFraction)&&contact.enterFraction>=0&&contact.enterFraction<=1&&
           std::isfinite(contact.exitFraction)&&contact.exitFraction>=0&&contact.exitFraction<=1&&std::isfinite(contact.damage);
}
struct Enemy {
    int health{}, team{}, armor{};
    bool helmet{};
};
bool ReadEnemy(const game_access::LocalPlayer &local,Address pawn,std::uint32_t handle,int hitgroup,Enemy &enemy) {
    const unsigned index=handle&0x7fffu;
    if(!pawn||pawn==local.pawn||handle==0xffffffffu||!index||index>=0x7fff||
       game_access::Resolve(local.list,handle)!=pawn)return false;
    const auto identity=memory::read<Address>(pawn+0x10);
    const auto entityClass=memory::read<Address>(identity+8);
    const auto schema=memory::read<Address>(entityClass+0x58);
    const auto name=memory::read<Address>(schema+8);
    if(memory::read_string(name,96)!="C_CSPlayerPawn")return false;
    const auto health=Field<int>(pawn,"C_BaseEntity","m_iHealth");
    const auto life=Field<std::uint8_t>(pawn,"C_BaseEntity","m_lifeState");
    const auto team=Field<std::uint8_t>(pawn,"C_BaseEntity","m_iTeamNum");
    const auto immune=Field<bool>(pawn,"C_CSPlayerPawn","m_bGunGameImmunity");
    const auto scene=Field<Address>(pawn,"C_BaseEntity","m_pGameSceneNode");
    const auto dormant=scene?Field<bool>(*scene,"CGameSceneNode","m_bDormant"):std::nullopt;
    if(!health||*health<=0||!life||*life||!team||(*team!=2&&*team!=3)||*team==local.team||
       !immune||*immune||!dormant||*dormant)return false;
    enemy.health=*health;enemy.team=*team;
    if(hitgroup) {
        const auto armor=Field<int>(pawn,"C_CSPlayerPawn","m_ArmorValue");
        if(!armor||*armor<0||*armor>10000)return false;
        enemy.armor=*armor;
        if(hitgroup==1&&enemy.armor>0) {
            const auto items=Field<Address>(pawn,"C_BasePlayerPawn","m_pItemServices");
            const auto helmet=items?Field<bool>(*items,"CCSPlayer_ItemServices","m_bHasHelmet"):std::nullopt;
            if(!helmet)return false;
            enemy.helmet=*helmet;
        }
    }
    return true;
}
float TargetDamage(float damage,int group,const Enemy &enemy,const Weapon &weapon) {
    if(!std::isfinite(damage)||damage<=0)return 0;
    if(!group)return damage;
    const bool ct=enemy.team==3;
    const auto body=combat_access::Convar(ct?"mp_damage_scale_ct_body":"mp_damage_scale_t_body",1);
    const auto head=combat_access::Convar(ct?"mp_damage_scale_ct_head":"mp_damage_scale_t_head",1);
    if(!std::isfinite(body)||body<0||!std::isfinite(head)||head<0)return 0;
    switch(group) {
    case 1: {
        const auto multiplier=Field<float>(weapon.data,"CCSWeaponBaseVData","m_flHeadshotMultiplier");
        if(!multiplier||!std::isfinite(*multiplier)||*multiplier<0)return 0;
        damage*=*multiplier*head;break;
    }
    case 2:case 4:case 5:case 8:damage*=body;break;
    case 3:damage*=1.25f*body;break;
    case 6:case 7:damage*=.75f*body;break;
    default:return 0;
    }
    const bool armored=(group>=1&&group<=5)||group==8;
    if(enemy.armor>0&&armored&&(group!=1||enemy.helmet)) {
        const auto ratio=Field<float>(weapon.data,"CCSWeaponBaseVData","m_flArmorRatio");
        if(!ratio||!std::isfinite(*ratio)||*ratio<0)return 0;
        float health=damage*(*ratio)*.5f;
        if((damage-health)*.5f>enemy.armor)health=damage-enemy.armor/.5f;
        damage=health;
    }
    return std::isfinite(damage)?std::max(0.f,std::floor(damage)):0;
}
bool ValidRayBasis(const Weapon &weapon,const Vec3 &direction,const Vec3 &eye) {
    Vec3 nativeDirection=storage.delta;
    const float length=storage.delta.length();
    return WorldPoint(storage.start)&&(storage.start-eye).length()<=2.f&&Finite(storage.delta)&&Normalize(nativeDirection)&&
        std::isfinite(length)&&length>0&&length<=weapon.range+360.f+std::max(2.f,weapon.range*.00025f)&&
        nativeDirection.dot(direction)>=.9999f;
}
FrameStamp Stamp(const game_access::LocalPlayer &local,const Weapon &weapon,std::uint64_t session=0) {
    return {session,local.list,local.controller,local.pawn,weapon.address,weapon.data,
        local.handle,weapon.handle,local.team,weapon.damage,weapon.penetration,weapon.range,weapon.rangeModifier};
}
bool CurrentStamp(const FrameStamp &stamp) {
    if(!CompatibleClient())return false;
    const auto local=game_access::ReadLocalPlayer(game_access::GameplayClient());
    if(local.state!=game_access::PlayerState::Alive||local.list!=stamp.list||local.controller!=stamp.controller||
       local.pawn!=stamp.pawn||local.handle!=stamp.localHandle||local.team!=stamp.team)return false;
    Weapon weapon;
    return ReadWeapon(local,weapon)==Reason::Unavailable&&Stamp(local,weapon,stamp.session)==stamp;
}
bool ReadAnchors(const game_access::LocalPlayer &local,Address &scene,Vec3 &origin,Vec3 &offset) {
    const auto node=Field<Address>(local.pawn,"C_BaseEntity","m_pGameSceneNode");
    const auto position=node?Field<Vec3>(*node,"CGameSceneNode","m_vecAbsOrigin"):std::nullopt;
    const auto view=Field<Vec3>(local.pawn,"C_BaseModelEntity","m_vecViewOffset");
    if(!node||!*node||!position||!view||!WorldPoint(*position)||!Finite(*view))return false;
    scene=*node;origin=*position;offset=*view;return true;
}
bool Near(const Vec3 &a,const Vec3 &b,float tolerance=.01f) {
    return Finite(a)&&Finite(b)&&(a-b).length_sqr()<=tolerance*tolerance;
}
bool CurrentFrame(const Frame &frame) {
    if(!WorldPoint(frame.eyeOrigin)||!CurrentStamp(frame.stamp))return false;
    const game_access::LocalPlayer local{frame.stamp.list,frame.stamp.controller,frame.stamp.pawn,
        frame.stamp.localHandle,frame.stamp.team,game_access::PlayerState::Alive};
    Address scene{};Vec3 origin,offset;
    return ReadAnchors(local,scene,origin,offset)&&scene==frame.sceneNode&&
        Near(origin,frame.playerOrigin)&&Near(offset,frame.viewOffset);
}
// Native9D31A0 copies these opaque physics tokens to CGameTrace+18/+20 and
// the exact full owner handle to+68. Never dereference the physics pointers.
void ReadSurface(const systems::tracing::result &hit,Result &result) {
    static_assert(sizeof(systems::tracing::result)==0xc0);
    const auto bytes=reinterpret_cast<const std::byte*>(&hit);
    result.hitEntity=hit.hit_entity;
    std::memcpy(&result.physicsObject,bytes+0x18,sizeof(Address));
    std::memcpy(&result.physicsShape,bytes+0x20,sizeof(Address));
    std::memcpy(&result.hitHandle,bytes+0x68,sizeof(result.hitHandle));
}
std::string EntityName(Address entity) {
    const auto identity=memory::read<Address>(entity+0x10);
    const auto entityClass=memory::read<Address>(identity+8);
    const auto schema=memory::read<Address>(entityClass+0x58);
    return memory::read_string(memory::read<Address>(schema+8),96);
}
bool ReadTransform(Address entity,Address &scene,Vec3 &origin,Vec3 &rotation) {
    const auto node=Field<Address>(entity,"C_BaseEntity","m_pGameSceneNode");
    const auto position=node?Field<Vec3>(*node,"CGameSceneNode","m_vecAbsOrigin"):std::nullopt;
    const auto angles=node?Field<Vec3>(*node,"CGameSceneNode","m_angAbsRotation"):std::nullopt;
    if(!node||!*node||!position||!angles||!WorldPoint(*position)||!Finite(*angles))return false;
    scene=*node;origin=*position;rotation=*angles;return true;
}
bool ExcludedName(std::string_view name) {
    // Source scene scanner excludes these real class names; several firearms
    // do not contain the word Weapon. Also exclude actor/viewmodel geometry.
    constexpr std::array<std::string_view,11> exact{
        "C_AK47","C_DEagle","C_Knife","C_Item_Healthshot","C_Flashbang",
        "C_Inferno","C_CS2HudModelArms","C_CS2HudModelWeapon","C_Hostage","C_Chicken","C_EconWearable"};
    return std::find(exact.begin(),exact.end(),name)!=exact.end()||
        name.find("Player")!=std::string_view::npos||name.find("Pawn")!=std::string_view::npos||
        name.find("Controller")!=std::string_view::npos||name.find("Weapon")!=std::string_view::npos||
        name.find("Projectile")!=std::string_view::npos||name.find("Grenade")!=std::string_view::npos||
        name.find("ViewModel")!=std::string_view::npos||name.find("C4")!=std::string_view::npos;
}
bool NoEntityOwner(std::uint32_t handle) {
    // Native9D3421/9D3432 explicitly resolves both -1 and -2 to a null
    // entity while retaining the original full handle in CGameTrace+68.
    return handle==0xffffffffu||handle==0xfffffffeu;
}
bool ClassifySurface(const game_access::LocalPlayer &local,Result &result) {
    if(!result.physicsObject&&!result.physicsShape){result.reason=Reason::InvalidSurfaceIdentity;return false;}
    if(!result.hitEntity) {
        const auto index=result.hitHandle&0x7fffu;
        if(!NoEntityOwner(result.hitHandle)&&index!=0){result.reason=Reason::InvalidSurfaceIdentity;return false;}
        result.kind=SurfaceKind::World;return true;
    }
    const auto name=EntityName(result.hitEntity);
    if(name.empty()){result.reason=Reason::InvalidSurfaceIdentity;return false;}
    if(name=="C_World"||name=="CWorld") {result.kind=SurfaceKind::World;return true;}
    if(ExcludedName(name)) {
        result.kind=SurfaceKind::Excluded;result.reason=Reason::ExcludedSurface;return false;
    }
    if(game_access::Resolve(local.list,result.hitHandle)!=result.hitEntity||
       !ReadTransform(result.hitEntity,result.sceneNode,result.entityOrigin,result.entityRotation)){
        result.reason=Reason::InvalidSurfaceIdentity;return false;
    }
    result.kind=SurfaceKind::Prop;return true;
}
bool CurrentSurface(const Result &result) {
    if(!result.surface||(!result.physicsObject&&!result.physicsShape))return false;
    // Caller separately validates current frame and map epoch. This function
    // intentionally performs no native calls or repeated weapon-data scans.
    if(result.kind==SurfaceKind::World)return true;
    if(result.kind!=SurfaceKind::Prop||game_access::Resolve(result.stamp.list,result.hitHandle)!=result.hitEntity)return false;
    Address scene{};Vec3 origin,rotation;
    return ReadTransform(result.hitEntity,scene,origin,rotation)&&scene==result.sceneNode&&
        Near(origin,result.entityOrigin)&&Near(rotation,result.entityRotation);
}
bool MatchesMaterial(const Contact &contact,Result &result) {
    const Address boundary=reinterpret_cast<Address>(storage.surfaceArray)+
        static_cast<Address>(contact.enter&0x7fffu)*sizeof(SurfaceElement);
    const auto object=memory::safe_read<Address>(boundary+0x10),shape=memory::safe_read<Address>(boundary+0x18);
    const auto owner=memory::safe_read<std::uint32_t>(boundary+0x2c);
    const auto type=memory::safe_read<std::uint8_t>(boundary+0x33);
    // Native885A90 branches on type1 for actor-hitbox data; only type0 carries
    // the verified physical object/shape pair. Material pointers are unrelated.
    result.materialIdentityValid=object&&shape&&owner&&type;
    if(!result.materialIdentityValid)return false;
    result.materialPhysicsObject=*object;result.materialPhysicsShape=*shape;
    result.materialOwner=*owner;result.materialType=*type;
    const bool worldOwner=(*owner&0x7fffu)==0||*owner==result.hitHandle||
        (NoEntityOwner(*owner)&&NoEntityOwner(result.hitHandle));
    return *type==0&&*object==result.physicsObject&&*shape==result.physicsShape&&
        (result.kind==SurfaceKind::World?worldOwner:*owner==result.hitHandle);
}
Result ProbeLocked(const game_access::LocalPlayer &local,const Vec3 &eyePosition,const Vec3 &camera,const Vec3 &direction,const Frame *frame) {
    Result result;result.direction=direction;result.cameraOrigin=camera;result.eyeOrigin=eyePosition;
    if(frame)result.stamp=frame->stamp;
    if(!CompatibleClient()){result.reason=Reason::BindingsUnavailable;return result;}
    const bool penetrationBindings=ResolveBindings();
    const std::optional<Vec3> eye{eyePosition};
    if(!api.filter||!api.ray||!api.managerSlot){result.reason=Reason::BindingsUnavailable;return result;}
    systems::tracing::result hit{};
    if(!Ray(local.pawn,camera,camera+result.direction*maximumRay,hit)){result.reason=Reason::RayFailed;return result;}
    result.fraction=hit.fraction;result.position=hit.end_pos;result.normal=hit.normal;
    result.normalLength=hit.normal.length();result.allSolid=hit.all_solid;
    // Audited client17784EC sets +BC on every point-ray hit; client834957
    // consults it as cached contact-position validity. It is NOT start-solid.
    result.positionValid=reinterpret_cast<const std::uint8_t*>(&hit)[0xbc]!=0;
    if(!std::isfinite(hit.fraction)||hit.fraction<0||hit.fraction>1){result.reason=Reason::InvalidFraction;return result;}
    if(hit.all_solid){result.reason=Reason::AllSolid;return result;}
    if(hit.fraction>=1){result.state=State::Clear;result.reason=Reason::Clear;return result;}
    if(!WorldPoint(result.position)){result.reason=Reason::InvalidHitPosition;return result;}
    if(!Normalize(result.normal)){result.reason=Reason::InvalidHitNormal;return result;}
    result.distance=(result.position-camera).length();result.eyeDistance=(result.position-*eye).length();
    const auto expected=camera+result.direction*(maximumRay*hit.fraction);
    result.endpointError=(expected-result.position).length();
    const float rayTolerance=std::max(2.f,maximumRay*.00025f);
    if(!std::isfinite(result.distance)||!std::isfinite(result.eyeDistance)||result.distance<.01f){result.reason=Reason::InvalidHitDistance;return result;}
    if(!std::isfinite(result.endpointError)||result.endpointError>rayTolerance){result.reason=Reason::RayEndpointMismatch;return result;}
    if(result.normal.dot(result.direction)>0)result.normal=-result.normal;
    result.surface=true;
    if(frame&&!CurrentFrame(*frame)){result.reason=Reason::StaleFrame;return result;}
    ReadSurface(hit,result);
    if(frame&&!ClassifySurface(local,result))return result;
    Weapon weapon;
    result.reason=ReadWeapon(local,weapon);
    if(result.reason!=Reason::Unavailable)return result;
    result.initialDamage=weapon.damage;result.requestedRange=weapon.range;
    if(!penetrationBindings){result.reason=Reason::BindingsUnavailable;return result;}
    if(result.eyeDistance>weapon.range){result.state=State::Blocked;result.reason=Reason::OutOfRange;return result;}
    Vec3 bulletDirection=result.position-*eye;
    if(!Normalize(bulletDirection)){result.reason=Reason::InvalidEye;return result;}
    const auto bulletDelta=bulletDirection*weapon.range;
    ClearTrace();
    if(!Setup(local.pawn,*eye,bulletDelta)){result.reason=Reason::TraceSetupFailed;return result;}
    if(!ValidTrace()){result.reason=Reason::InvalidTrace;return result;}
    result.setupContactCount=storage.contacts;
    if(storage.contacts&&!Simulate(weapon,local.team)){result.reason=Reason::SimulationFailed;return result;}
    if(!ValidTrace()){result.reason=Reason::InvalidTrace;return result;}
    result.contactCount=storage.contacts;result.surfaceCount=storage.surfaces;
    result.penetrationOrigin=storage.start;result.penetrationDelta=storage.delta;
    result.penetrationRange=storage.delta.length();
    // Native setup can extend four times by90 units, or truncate/renormalize.
    // All contact positions use this retained native start/delta basis.
    if(!ValidRayBasis(weapon,bulletDirection,*eye)){result.reason=Reason::InvalidTraceRay;return result;}
    bool exhausted{};
    for(int index=0;index<storage.contacts;++index) {
        Contact contact;result.contactIndex=index;
        if(!ReadContact(index,contact)){result.reason=Reason::InvalidContact;return result;}
        result.contactFlags=contact.flags;result.enterFraction=contact.enterFraction;
        result.exitFraction=contact.exitFraction;result.contactDamage=contact.damage;
        if(!(contact.flags&1)){exhausted=exhausted||contact.damage<=0;continue;}
        if((contact.enter&0x7fffu)>=static_cast<unsigned>(storage.surfaces)) {result.reason=Reason::InvalidContact;return result;}
        result.materialEntry=storage.start+storage.delta*contact.enterFraction;
        result.materialError=(result.materialEntry-result.position).length();
        const float tolerance=std::max(2.f,weapon.range*.00025f);
        if(!std::isfinite(result.materialError)||result.materialError>tolerance){result.reason=Reason::FirstMaterialMismatch;return result;}
        if(contact.exitFraction<contact.enterFraction){result.reason=Reason::InvalidContact;return result;}
        if(frame&&!MatchesMaterial(contact,result)){result.reason=Reason::SurfaceMismatch;return result;}
        // Recheck the active weapon slot and data, not merely the old handle.
        if(!CurrentStamp(Stamp(local,weapon))){result.reason=Reason::InvalidWeapon;return result;}
        result.damage=std::max(0.f,contact.damage);
        if(exhausted||contact.damage<=0){result.state=State::Blocked;result.reason=contact.exitFraction>=1?Reason::Stopped:Reason::DamageExhausted;return result;}
        if(contact.exitFraction>=1){result.state=State::Blocked;result.reason=Reason::Stopped;return result;}
        result.thickness=std::max(0.f,(contact.exitFraction-contact.enterFraction)*result.penetrationRange);
        result.exitPoint=storage.start+storage.delta*contact.exitFraction;
        if(!WorldPoint(result.exitPoint)){result.reason=Reason::InvalidContact;return result;}
        if(weapon.penetration<=0){result.state=State::Blocked;result.reason=Reason::NoPenetrationPower;return result;}
        result.state=State::Pass;result.reason=Reason::Penetrated;return result;
    }
    // A visible hit without a matching native material record is unavailable;
    // it must not be advertised as a penetrable or blocked wall.
    result.reason=Reason::NoMaterialRecord;return result;
}
Frame CaptureLocked(std::uint64_t session,bool cameraRequired) {
    Frame frame;frame.stamp.session=session;
    if(!CompatibleClient()){frame.reason=Reason::BindingsUnavailable;return frame;}
    if(!ResolveBindings()){frame.reason=Reason::BindingsUnavailable;return frame;}
    const auto local=game_access::ReadLocalPlayer(game_access::GameplayClient());
    if(local.state!=game_access::PlayerState::Alive){frame.reason=Reason::NoLocalPlayer;return frame;}
    if(cameraRequired) {
        const auto matrix=memory::safe_read<math::matrix4x4>(addresses::globals::view_matrix);
        Vec3 direction;
        if(!matrix||!Camera(*matrix,frame.cameraOrigin,direction)){frame.reason=Reason::InvalidCamera;return frame;}
        frame.viewMatrix=*matrix;
    }
    if(!ReadAnchors(local,frame.sceneNode,frame.playerOrigin,frame.viewOffset)||
       !combat_access::ReadShootPosition(local.pawn,frame.eyeOrigin)||!WorldPoint(frame.eyeOrigin)){
        frame.reason=Reason::InvalidEye;return frame;
    }
    if(!cameraRequired)frame.cameraOrigin=frame.eyeOrigin;
    Weapon weapon;frame.reason=ReadWeapon(local,weapon);
    if(frame.reason!=Reason::Unavailable)return frame;
    frame.stamp=Stamp(local,weapon,session);
    if(!CurrentFrame(frame)){frame.reason=Reason::StaleFrame;return frame;}
    frame.valid=true;frame.reason=Reason::Unavailable;return frame;
}
} // namespace

bool Initialize() {std::lock_guard lock(mutex);return ResolveBindings();}
Frame CaptureFrame(std::uint64_t session) {std::lock_guard lock(mutex);return CaptureLocked(session,true);}
Frame CaptureProfile(std::uint64_t session) {std::lock_guard lock(mutex);return CaptureLocked(session,false);}
std::optional<ShotContact> TraceShot(const Frame& profile,const Vec3& origin,const Vec3& rayDirection,float range) {
    std::lock_guard lock(mutex);
    auto direction=rayDirection;
    if (!profile.valid || !CurrentFrame(profile) || !WorldPoint(origin) || !Normalize(direction) ||
        !std::isfinite(range) || range<=0.f || range>profile.stamp.range || range>maximumRay) return {};
    ResolveBindings();
    if (!api.filter || !api.ray || !api.managerSlot) return {};
    systems::tracing::result result{};
    const auto endpoint=origin+direction*range;
    if (!WorldPoint(endpoint) || !Ray(profile.stamp.pawn,origin,endpoint,result) ||
        !CurrentFrame(profile) || !std::isfinite(result.fraction) || result.fraction<0.f ||
        result.fraction>1.f || result.all_solid) return {};
    if (result.fraction==1.f) return ShotContact{endpoint,false};
    const auto expected=origin+direction*(range*result.fraction);
    if (!WorldPoint(result.end_pos) || (expected-result.end_pos).length()>std::max(.05f,range*.00001f)) return {};
    return ShotContact{result.end_pos,true};
}
bool IsCurrent(const Frame &frame) {std::lock_guard lock(mutex);return frame.valid&&CurrentFrame(frame);}
Result ProbeAt(const Frame &frame,float ndcX,float ndcY) {
    std::lock_guard lock(mutex);Result result;result.stamp=frame.stamp;
    if(!frame.valid){result.reason=frame.reason;return result;}
    if(!CurrentFrame(frame)){result.reason=Reason::StaleFrame;return result;}
    Vec3 origin,direction;
    if(!Camera(frame.viewMatrix,origin,direction,ndcX,ndcY)){result.reason=Reason::InvalidCamera;return result;}
    const auto local=game_access::ReadLocalPlayer(game_access::GameplayClient());
    result=ProbeLocked(local,frame.eyeOrigin,origin,direction,&frame);
    if(!CurrentFrame(frame)||((result.kind==SurfaceKind::Prop)&&!CurrentSurface(result))){
        result.state=State::Unavailable;result.reason=Reason::StaleFrame;
    }
    return result;
}
Result ProbeRay(const Frame &profile,const Vec3 &origin,const Vec3 &rayDirection) {
    std::lock_guard lock(mutex);Result result;result.stamp=profile.stamp;
    if(!profile.valid){result.reason=profile.reason;return result;}
    if(!CurrentFrame(profile)){result.reason=Reason::StaleFrame;return result;}
    if(!WorldPoint(origin)){result.reason=Reason::InvalidPoint;return result;}
    auto direction=rayDirection;
    if(!Normalize(direction)){result.reason=Reason::InvalidTraceRay;return result;}
    const auto local=game_access::ReadLocalPlayer(game_access::GameplayClient());
    result=ProbeLocked(local,origin,origin,direction,&profile);
    if(!CurrentFrame(profile)||((result.kind==SurfaceKind::Prop)&&!CurrentSurface(result))){
        result.state=State::Unavailable;result.reason=Reason::StaleFrame;
    }
    return result;
}
bool IsSurfaceCurrent(const Result &result) {std::lock_guard lock(mutex);return CurrentSurface(result);}
Result Probe() {
    std::lock_guard lock(mutex);Result result;
    if(!CompatibleClient()){result.reason=Reason::BindingsUnavailable;return result;}
    const auto local=game_access::ReadLocalPlayer(game_access::GameplayClient());
    if(local.state!=game_access::PlayerState::Alive){result.reason=Reason::NoLocalPlayer;return result;}
    const auto matrix=memory::safe_read<math::matrix4x4>(addresses::globals::view_matrix);
    Vec3 camera,direction;
    if(!matrix||!Camera(*matrix,camera,direction)){result.reason=Reason::InvalidCamera;return result;}
    const auto eye=Eye(local);
    if(!eye){result.reason=Reason::InvalidEye;return result;}
    return ProbeLocked(local,*eye,camera,direction,nullptr);
}
static TargetResult EvaluateTargetImpl(Address pawn,std::uint32_t handle,const Vec3 &point,int hitgroup,
    std::optional<combat_bindings::HistoryShotTiming> shotTiming, bool historical) {
    std::lock_guard lock(mutex);
    TargetResult result;result.pawn=pawn;result.targetHandle=handle;result.hitgroup=hitgroup;
    if(!CompatibleClient()){result.reason=Reason::BindingsUnavailable;return result;}
    ResolveBindings();
    if(!api.filter||!api.setup||!api.bullet||!api.reset){result.reason=Reason::BindingsUnavailable;return result;}
    const auto local=game_access::ReadLocalPlayer(game_access::GameplayClient());
    if(local.state!=game_access::PlayerState::Alive){result.reason=Reason::NoLocalPlayer;return result;}
    if(hitgroup<0||hitgroup>8||!WorldPoint(point)){result.reason=Reason::InvalidPoint;return result;}
    result.localHandle=local.handle;
    Enemy enemy;
    if(!ReadEnemy(local,pawn,handle,hitgroup,enemy)){result.reason=Reason::InvalidTarget;return result;}
    Vec3 shootPosition{};
    if(!(shotTiming?combat_access::ReadShotPosition(local.pawn,*shotTiming,shootPosition):
         combat_access::ReadShootPosition(local.pawn,shootPosition))||!WorldPoint(shootPosition)){
        result.reason=Reason::InvalidEye;return result;
    }
    const std::optional<Vec3> eye{shootPosition};
    result.eyeOrigin=*eye;
    Weapon weapon;result.reason=ReadWeapon(local,weapon);
    if(result.reason!=Reason::Unavailable)return result;
    result.weaponHandle=weapon.handle;
    Vec3 direction=point-*eye;
    const float distance=direction.length();
    if(!std::isfinite(distance)||distance<.01f||!Normalize(direction)){result.reason=Reason::InvalidPoint;return result;}
    if(distance>weapon.range){result.valid=true;result.reason=Reason::OutOfRange;return result;}
    ClearTrace();
    if(!Setup(local.pawn,*eye,direction*weapon.range)){result.reason=Reason::TraceSetupFailed;return result;}
    if(!ValidTrace()){result.reason=Reason::InvalidTrace;return result;}
    if(storage.contacts&&!Simulate(weapon,local.team)){result.reason=Reason::SimulationFailed;return result;}
    if(!ValidTrace()){result.reason=Reason::InvalidTrace;return result;}
    result.contactCount=storage.contacts;
    // Native calls are observational, but a replaced pawn or active weapon must
    // invalidate the result even when no later arrival boundary can be resolved.
    const auto currentLocal=game_access::ReadLocalPlayer(game_access::GameplayClient());
    Weapon currentWeapon;
    if(currentLocal.state!=game_access::PlayerState::Alive||currentLocal.pawn!=local.pawn||
       currentLocal.handle!=local.handle||currentLocal.team!=local.team){result.reason=Reason::NoLocalPlayer;return result;}
    if(ReadWeapon(currentLocal,currentWeapon)!=Reason::Unavailable||currentWeapon.handle!=weapon.handle||
       currentWeapon.address!=weapon.address||currentWeapon.data!=weapon.data){result.reason=Reason::InvalidWeapon;return result;}
    if(!ReadEnemy(currentLocal,pawn,handle,hitgroup,enemy)){result.reason=Reason::InvalidTarget;return result;}
    if(historical&&!ValidRayBasis(weapon,direction,*eye)){result.reason=Reason::InvalidTraceRay;return result;}
    if(!storage.contacts){
        result.valid=true;result.reason=Reason::TargetNotReached;
        if(historical){
            result.position=point;result.rawDamage=weapon.damage*std::pow(weapon.rangeModifier,distance/500.f);
            result.damage=TargetDamage(result.rawDamage,hitgroup,enemy,currentWeapon);
            result.reached=std::isfinite(result.damage)&&result.damage>0;
            result.reason=result.reached?Reason::TargetReached:Reason::DamageExhausted;
        }
        return result;
    }
    if(!ValidRayBasis(weapon,direction,*eye)){result.reason=Reason::InvalidTraceRay;return result;}
    bool penetrated{};
    for(int index=0;index<storage.contacts;++index) {
        Contact contact;result.contactIndex=index;
        if(!ReadContact(index,contact)){result.reason=Reason::InvalidContact;return result;}
        if(contact.damage<=0){result.valid=true;result.reason=Reason::DamageExhausted;return result;}
        if(contact.flags&1) {
            if(contact.exitFraction<contact.enterFraction||
               (contact.enter&0x7fffu)>=static_cast<unsigned>(storage.surfaces)){
                result.reason=Reason::InvalidContact;return result;
            }
            if(contact.exitFraction>=1){result.valid=true;result.reason=Reason::Stopped;return result;}
            if(weapon.penetration<=0){result.valid=true;result.reason=Reason::NoPenetrationPower;return result;}
            penetrated=true;continue;
        }
        if(historical){
            if(contact.exitFraction<contact.enterFraction){result.reason=Reason::InvalidContact;return result;}
            const auto relative=point-storage.start;
            const auto lengthSquared=storage.delta.length_sqr();
            const auto fraction=lengthSquared>0?(relative.x*storage.delta.x+relative.y*storage.delta.y+relative.z*storage.delta.z)/lengthSquared:-1.f;
            // A saved hitbox can lie in a free-travel span without any live
            // actor there. Native material spans still block or reduce damage.
            if(std::isfinite(fraction)&&fraction>=contact.enterFraction&&fraction<contact.exitFraction){
                result.position=point;result.rawDamage=contact.damage;
                result.damage=TargetDamage(contact.damage,hitgroup,enemy,currentWeapon);
                result.valid=true;result.reached=result.damage>0;result.penetrated=penetrated;
                result.reason=result.reached?Reason::TargetReached:Reason::DamageExhausted;return result;
            }
            continue;
        }
        // Free travel ends at the NEXT collision, +12, not the prior +10
        // boundary. Read arrival before a stopped flesh span consumes damage.
        const unsigned boundary=contact.exit&0x7fffu;
        if(boundary>=static_cast<unsigned>(storage.surfaces)){result.reason=Reason::InvalidContact;return result;}
        const auto arrived=memory::safe_read<std::uint32_t>(reinterpret_cast<Address>(storage.surfaceArray)+
            static_cast<Address>(boundary)*sizeof(SurfaceElement)+0x2c);
        if(!arrived){result.reason=Reason::InvalidContact;return result;}
        if(*arrived!=handle||game_access::Resolve(local.list,*arrived)!=pawn)continue;
        result.position=storage.start+storage.delta*contact.exitFraction;
        if(!WorldPoint(result.position)){result.reason=Reason::InvalidContact;return result;}
        result.rawDamage=contact.damage;
        result.damage=TargetDamage(contact.damage,hitgroup,enemy,currentWeapon);
        result.valid=true;result.reached=result.damage>0;result.penetrated=penetrated;
        result.reason=result.reached?Reason::TargetReached:Reason::DamageExhausted;
        return result;
    }
    result.valid=true;result.reason=Reason::TargetNotReached;return result;
}
bool Reset() {
    std::lock_guard lock(mutex);
    const bool ownedSurfaces=storage.surfaceArray&&storage.surfaceArray!=storage.inlineSurfaces.data()&&!(static_cast<unsigned>(storage.surfaceFlags)&0xc0000000u);
    const bool ownedContacts=storage.contactArray&&storage.contactArray!=storage.inlineContacts&&!(static_cast<unsigned>(storage.contactFlags)&0xc0000000u);
    if(api.reset) {
        if(!Cleanup())return false;
    } else if(ownedSurfaces||ownedContacts)return false;
    storage={};return true;
}
TargetResult EvaluateTarget(Address pawn,std::uint32_t handle,const Vec3 &point,int hitgroup,
    std::optional<combat_bindings::HistoryShotTiming> shotTiming) {
    return EvaluateTargetImpl(pawn,handle,point,hitgroup,shotTiming,false);
}
TargetResult EvaluateHistoricalTarget(Address pawn,std::uint32_t handle,const Vec3 &point,
    combat_bindings::HistoryShotTiming shotTiming,int hitgroup) {
    return EvaluateTargetImpl(pawn,handle,point,hitgroup,shotTiming,true);
}
const char *ReasonText(Reason reason) {
    switch(reason) {
    case Reason::StaleFrame:return "Player, weapon or surface moved; refreshing coverage.";
    case Reason::ExcludedSurface:return "Player and weapon surfaces are excluded from this highlight.";
    case Reason::InvalidSurfaceIdentity:return "The surface has no verified physics identity.";
    case Reason::Unavailable:return "Waiting for an aimed surface.";
    case Reason::NoLocalPlayer:return "A living local player is required.";
    case Reason::InvalidTarget:return "The target is not a current living enemy player.";
    case Reason::InvalidPoint:return "The supplied target point or hitgroup is invalid.";
    case Reason::TargetReached:return "The equipped weapon reaches this target.";
    case Reason::TargetNotReached:return "The native weapon trace does not reach this target.";
    case Reason::InvalidCamera:return "The visible camera ray is unavailable.";
    case Reason::InvalidEye:return "The player's eye position is unavailable.";
    case Reason::RayFailed:return "The native surface ray failed.";
    case Reason::InvalidSurface:return "The surface position or normal is invalid.";
    case Reason::InvalidFraction:return "The surface ray returned an invalid hit fraction.";
    case Reason::AllSolid:return "The camera is inside solid geometry.";
    case Reason::InvalidHitPosition:return "The aimed surface position is unavailable.";
    case Reason::InvalidHitNormal:return "The aimed surface has no valid orientation.";
    case Reason::InvalidHitDistance:return "The aimed surface is too close or has an invalid distance.";
    case Reason::RayEndpointMismatch:return "The surface endpoint does not agree with the visible ray.";
    case Reason::UnsupportedWeapon:return "Equip a firearm to evaluate penetration.";
    case Reason::InvalidWeapon:return "Current weapon data is unavailable.";
    case Reason::BindingsUnavailable:return "Native penetration bindings are unavailable.";
    case Reason::OutOfRange:return "The aimed surface is beyond this weapon's range.";
    case Reason::TraceSetupFailed:return "The native penetration trace failed.";
    case Reason::InvalidTrace:return "Native contact storage is invalid.";
    case Reason::SimulationFailed:return "The native bullet simulation failed.";
    case Reason::InvalidContact:return "A native material contact is invalid.";
    case Reason::SurfaceMismatch:return "The weapon ray and visible surface do not agree.";
    case Reason::FirstMaterialMismatch:return "The weapon ray reaches a different surface first.";
    case Reason::NoMaterialRecord:return "The native weapon trace returned no material span for this surface.";
    case Reason::InvalidTraceRay:return "The native weapon trace returned an invalid ray.";
    case Reason::Clear:return "No aimed surface along the visible ray.";
    case Reason::Penetrated:return "The equipped weapon penetrates this surface.";
    case Reason::Stopped:return "The bullet stops inside this surface.";
    case Reason::DamageExhausted:return "The bullet has no damage after this surface.";
    case Reason::NoPenetrationPower:return "This weapon has no penetration power.";
    }
    return "Unknown penetration result.";
}
}
