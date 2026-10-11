#include "game/autowall_probe.h"
#include <iostream>
#include <limits>
#include <stdexcept>
namespace fixture {
HMODULE Module(const char *name);
ULONGLONG Clock();
}
#define GetModuleHandleA fixture::Module
#define GetTickCount64 fixture::Clock
#include "game/autowall_probe.cpp"
#undef GetModuleHandleA
#undef GetTickCount64

namespace fixture {
using namespace autowall_probe;
using Address=std::uintptr_t;
namespace f=game_access::field;
ULONGLONG now=1000;
Address client{},manager=0x123456;
bool missingBullet{},failRay{},failSetup{},failBullet{},failReset{},rayClear{},grown{},replaceContacts{},invalidateWeapon{};
unsigned setupCalls{},bulletCalls{},resetCalls{},filterCalls{};
float rayFraction{-1};Vec3 rayNormal{-1,0,0};
bool rayAllSolid{},wrongEndpoint{};
float appliedDamage{},appliedPenetration{},appliedModifier{},nativeDeltaScale{1};
bool wrongNativeDirection{},invalidateTarget{},killTarget{},switchWeapon{},wrongNativeStart{},failShotEye{};
Vec3 matchedEyeOffset{},shootEyeOffset{};std::optional<Vec3> expectedSetupOrigin;
combat_bindings::HistoryShotTiming lastShotTiming{};
Address rayEntity{},rayObject=0x111000,rayShape=0x222000,materialObject=0x111000,materialShape=0x222000;
std::uint32_t rayOwner{};std::uint8_t materialType{};
bool failShootEye{},switchOnRay{},changeDamage{},moveLocal{},moveProp{};
Address lastSurfaceArray{},lastContactArray{};
std::array<std::byte,0x20> list{};
std::array<std::byte,512*0x70> page{};
std::array<std::byte,0x3000> pawn{},controller{},scene{},services{},weapon{},vdata{};
std::array<std::byte,0x4000> target{};
std::array<std::byte,0x3000> targetScene{};
std::array<std::byte,0x100> identity{},entityClass{},schema{},items{};
const char pawnClassName[]="C_CSPlayerPawn",propClassName[]="C_BreakableProp",weaponClassName[]="C_WeaponAK47",projectileClassName[]="C_HEGrenadeProjectile";
std::vector<std::uint32_t> surfaceHandles;
math::matrix4x4 matrix{};
std::vector<Contact> plan;
std::array<SurfaceElement,192> grownSurfaces{};
std::array<Contact,192> grownContacts{};
std::array<Contact,192> replacement{};
constexpr std::uint32_t pawnHandle=(1u<<15)|100,weaponHandle=(3u<<15)|101,targetHandle=(5u<<15)|102;
constexpr unsigned originOffset=0x810,viewOffset=0x820,servicesOffset=0x830,activeOffset=0x40,subclassOffset=0x380;
constexpr unsigned typeOffset=0x100,damageOffset=0x110,penetrationOffset=0x114,rangeOffset=0x118,modifierOffset=0x11c,headMultiplierOffset=0x120,armorRatioOffset=0x124;
constexpr unsigned rotationOffset=0x848;
constexpr unsigned dormantOffset=0x800,immunityOffset=0x900,armorOffset=0x910,itemsOffset=0x920,helmetOffset=0x49;
template<class T> Address Ptr(T &v){return reinterpret_cast<Address>(&v);}
void Check(bool value,const char *message){if(!value)throw std::runtime_error(message);}
template<class T>void Put(Address at,T value){Check(game_access::Write(at,value),"Fixture write failed");}
void Entry(unsigned index,Address entity,std::uint32_t handle){Put(Ptr(page)+index*0x70,entity);Put(Ptr(page)+index*0x70+0x10,handle);}
ULONGLONG Clock(){return now;}
HMODULE Module(const char *name){return std::strcmp(name,"client.dll")==0?reinterpret_cast<HMODULE>(client):nullptr;}
void __fastcall Filter(systems::tracing::filter *filter,Address skipped,std::uint64_t mask,std::uint8_t layer,int type) {
    Check(skipped==Ptr(pawn)&&mask==0x1c300b&&layer==3&&type==15,"Native filter ABI or ballistic mask changed");
    filter->mask=mask;filter->layer=layer;++filterCalls;
}
bool __fastcall NativeRay(Address owner,systems::tracing::ray *ray,const Vec3 *start,const Vec3 *end,
                    const systems::tracing::filter *filter,systems::tracing::result *hit) {
    Check(owner==manager&&ray&&ray->type==0&&filter->mask==0x1c300b,"Surface ray ABI changed");
    if(failRay)RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);
    *hit={};hit->fraction=rayClear?1.f:100.f/32768.f;
    hit->start_pos=*start;hit->end_pos=*start+(*end-*start)*hit->fraction;
    if(rayFraction!=-1) {hit->fraction=rayFraction;hit->end_pos=*start+(*end-*start)*hit->fraction;}
    if(wrongEndpoint)hit->end_pos.x+=20;
    hit->normal=rayNormal;hit->all_solid=rayAllSolid;hit->hit_entity=rayEntity;
    Put(reinterpret_cast<Address>(hit)+0x18,rayObject);Put(reinterpret_cast<Address>(hit)+0x20,rayShape);
    Put(reinterpret_cast<Address>(hit)+0x68,rayOwner);
    if(switchOnRay)Put(Ptr(services)+activeOffset,pawnHandle);
    // Native point-ray hits set +BC=1: this metadata is not start-solid.
    reinterpret_cast<std::uint8_t*>(hit)[0xbc]=rayClear?0:1;
    return !rayClear;
}
void __fastcall NativeSetup(TraceData *trace,Vec3 eye,Vec3 delta,systems::tracing::filter filter,int count,bool world) {
    Check(trace==&storage&&count==4&&world&&filter.mask==0x1c300b,"Native trace setup ABI changed");
    Check((expectedSetupOrigin?Near(eye,*expectedSetupOrigin):std::abs(eye.z-64)<.01f)&&std::abs(delta.length()-1000)<.01f,
        "Probe did not use the declared ballistic origin and current weapon range");
    ++setupCalls;
    if(failSetup)RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);
    if(grown) {
        if(lastSurfaceArray)Check(reinterpret_cast<Address>(trace->surfaceArray)==lastSurfaceArray&&reinterpret_cast<Address>(trace->contactArray)==lastContactArray,
                                 "Grown native vectors were discarded on consecutive probes");
        trace->surfaceArray=grownSurfaces.data();trace->surfaceCapacity=192;trace->surfaceFlags=0;
        trace->contactArray=grownContacts.data();trace->contactCapacity=192;trace->contactFlags=0;
    }
    lastSurfaceArray=reinterpret_cast<Address>(trace->surfaceArray);lastContactArray=reinterpret_cast<Address>(trace->contactArray);
    trace->start=eye;trace->delta=delta*nativeDeltaScale;
    if(wrongNativeDirection)trace->delta.y+=100;
    if(wrongNativeStart)trace->start.y+=100;
    trace->surfaces=static_cast<int>(surfaceHandles.size());
    Check(trace->surfaces<=trace->surfaceCapacity,"Fixture exceeded surface capacity");
    for(std::size_t index=0;index<surfaceHandles.size();++index) {
        const auto boundary=reinterpret_cast<Address>(trace->surfaceArray)+index*sizeof(SurfaceElement);
        Put(boundary+0x10,materialObject);Put(boundary+0x18,materialShape);
        Put(boundary+0x2c,surfaceHandles[index]);Put(boundary+0x33,materialType);
    }
    trace->contacts=replaceContacts?2:static_cast<int>(plan.size());
    Check(memory::store(reinterpret_cast<Address>(trace->contactArray),plan.data(),plan.size()*sizeof(Contact)),"Contact write failed");
}
void __fastcall Bullet(TraceData *trace,float damage,float penetration,float modifier,int count,int team,Address unknown) {
    Check(trace==&storage&&count==4&&team==2&&!unknown,"Native bullet simulation ABI changed");
    appliedDamage=damage;appliedPenetration=penetration;appliedModifier=modifier;++bulletCalls;
    if(failBullet)RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);
    if(replaceContacts) {
        std::copy(plan.begin(),plan.end(),replacement.begin());
        trace->contactArray=replacement.data();trace->contactCapacity=192;trace->contactFlags=0;
        trace->contacts=static_cast<int>(plan.size());
    }
    if(invalidateWeapon)Entry(101,Ptr(weapon),weaponHandle+(1u<<15));
    if(invalidateTarget)Entry(102,Ptr(target),targetHandle+(1u<<15));
    if(killTarget)Put(Ptr(target)+f::C_BaseEntity::m_iHealth,0);
    if(switchWeapon)Put(Ptr(services)+activeOffset,pawnHandle);
    if(changeDamage)Put(Ptr(vdata)+damageOffset,31);
    if(moveLocal)Put(Ptr(scene)+originOffset,Vec3{1,0,0});
    if(moveProp)Put(Ptr(targetScene)+originOffset,Vec3{1,0,0});
}
void __fastcall Release(TraceData *trace) {
    Check(trace==&storage,"Native cleanup storage changed");++resetCalls;
    if(failReset)RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);
    *trace={};trace->surfaceArray=trace->inlineSurfaces.data();trace->contactArray=trace->inlineContacts;
}
void Defaults() {
    failRay=failSetup=failBullet=failReset=rayClear=grown=replaceContacts=invalidateWeapon=missingBullet=false;
    lastSurfaceArray=lastContactArray=0;
    rayFraction=-1;rayNormal={-1,0,0};rayAllSolid=wrongEndpoint=false;
    shootEyeOffset=matchedEyeOffset={};expectedSetupOrigin.reset();failShootEye=failShotEye=switchOnRay=changeDamage=moveLocal=moveProp=false;
    rayEntity=0;rayOwner=0;rayObject=materialObject=0x111000;rayShape=materialShape=0x222000;materialType=0;
    Put(Ptr(scene)+originOffset,Vec3{});Put(Ptr(pawn)+viewOffset,Vec3{0,0,64});
    Put(Ptr(targetScene)+originOffset,Vec3{});Put(Ptr(targetScene)+rotationOffset,Vec3{});
    nativeDeltaScale=1;wrongNativeDirection=invalidateTarget=killTarget=switchWeapon=wrongNativeStart=false;
    Check(Reset(),"Default cleanup failed");
    api={};now+=1000;
    plan={{0,.1f,30,0,0,0,0,{}},{.1f,.11f,20,0,0,0,1,{}}};
    surfaceHandles={0};
    Entry(102,Ptr(target),targetHandle);
    Put(Ptr(target)+f::C_BaseEntity::m_iHealth,100);
    Put(Ptr(target)+f::C_BaseEntity::m_lifeState,std::uint8_t{});
    Put(Ptr(target)+f::C_BaseEntity::m_iTeamNum,std::uint8_t{3});
    Put(Ptr(target)+f::C_BaseEntity::m_pGameSceneNode,Ptr(targetScene));
    Put(Ptr(target)+immunityOffset,false);Put(Ptr(targetScene)+dormantOffset,false);
    Put(Ptr(target)+armorOffset,0);Put(Ptr(target)+itemsOffset,Ptr(items));Put(Ptr(items)+helmetOffset,false);
    Put(Ptr(target)+0x10,Ptr(identity));Put(Ptr(identity)+8,Ptr(entityClass));
    Put(Ptr(entityClass)+0x58,Ptr(schema));Put(Ptr(schema)+8,Ptr(pawnClassName));
    Put(Ptr(services)+activeOffset,weaponHandle);
    Put(Ptr(vdata)+typeOffset,2);Put(Ptr(vdata)+damageOffset,30);
    Put(Ptr(vdata)+headMultiplierOffset,4.f);Put(Ptr(vdata)+armorRatioOffset,1.5f);
    Put(Ptr(vdata)+penetrationOffset,2.f);Put(Ptr(vdata)+rangeOffset,1000.f);Put(Ptr(vdata)+modifierOffset,.98f);
    Entry(101,Ptr(weapon),weaponHandle);
    matrix={};matrix.mat[0][1]=1;matrix.mat[1][2]=1;matrix.mat[1][3]=-64;matrix.mat[2][0]=1;matrix.mat[3][0]=1;
}
void InitializeFixture() {
    const auto bytes=std::max(game_access::offset::dwEntityList,game_access::offset::dwLocalPlayerController)+0x1000;
    client=reinterpret_cast<Address>(VirtualAlloc(nullptr,bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));Check(client!=0,"Client allocation failed");
    Put(client+game_access::offset::dwEntityList,Ptr(list));Put(client+game_access::offset::dwLocalPlayerController,Ptr(controller));
    Put(Ptr(list)+0x10,Ptr(page));Entry(100,Ptr(pawn),pawnHandle);Entry(101,Ptr(weapon),weaponHandle);
    Put(Ptr(controller)+f::CCSPlayerController::m_hPlayerPawn,pawnHandle);
    Put(Ptr(pawn)+f::C_BaseEntity::m_iHealth,100);Put(Ptr(pawn)+f::C_BaseEntity::m_lifeState,std::uint8_t{});Put(Ptr(pawn)+f::C_BaseEntity::m_iTeamNum,std::uint8_t{2});
    Put(Ptr(pawn)+f::C_BaseEntity::m_pGameSceneNode,Ptr(scene));Put(Ptr(scene)+originOffset,Vec3{});Put(Ptr(pawn)+viewOffset,Vec3{0,0,64});
    Put(Ptr(pawn)+servicesOffset,Ptr(services));Put(Ptr(services)+activeOffset,weaponHandle);Put(Ptr(weapon)+subclassOffset+8,Ptr(vdata));
    game_access::verifiedClient=client;addresses::globals::view_matrix=Ptr(matrix);Defaults();
}
}
namespace combat_access {
float Convar(const char *,float fallback) {return fallback;}
bool ReadShootPosition(game_access::Address pawn,math::vector3 &position) {
    if(fixture::failShootEye||pawn!=fixture::Ptr(fixture::pawn))return false;
    const auto eye=autowall_probe::Eye(game_access::ReadLocalPlayer(fixture::client));
    if(!eye)return false;
    position=*eye+fixture::shootEyeOffset;return true;
}
bool ReadShotPosition(game_access::Address pawn,combat_bindings::HistoryShotTiming timing,math::vector3 &position) {
    if(fixture::failShotEye||!ReadShootPosition(pawn,position))return false;
    fixture::lastShotTiming=timing;position=position+fixture::matchedEyeOffset;return true;
}
}
namespace memory {
std::string read_string(std::uintptr_t address,std::size_t limit) {
    std::string result;
    for(std::size_t index=0;index<limit;++index) {
        char value{};if(!game_access::Copy(address+index,&value,1))return {};
        if(!value)return result;result.push_back(value);
    }
    return {};
}
bool copy(void *out,std::uintptr_t address,std::size_t size)noexcept{return game_access::Copy(address,out,size);}
bool store(std::uintptr_t address,const void *in,std::size_t size)noexcept{return game_access::Store(address,in,size);}
std::uintptr_t resolve_cached(std::string_view expression) {
    using namespace fixture;namespace p=combat_bindings::patterns;
    if(expression==p::trace_filter_init)return reinterpret_cast<Address>(&Filter);
    if(expression==patterns::trace_ray)return reinterpret_cast<Address>(&NativeRay);
    if(expression==autowall_probe::managerPattern)return Ptr(manager);
    if(expression==p::trace_bullet_data_init)return reinterpret_cast<Address>(&NativeSetup);
    if(expression==p::trace_bullet)return missingBullet?0:reinterpret_cast<Address>(&Bullet);
    if(expression==p::trace_bullet_data_reset)return reinterpret_cast<Address>(&Release);
    return 0;
}
}
namespace systems {
std::uint32_t schemas::lookup(const char *,std::uint32_t name) {
    using namespace fixture;
    switch(name) {
    case "m_pGameSceneNode"_hash:return f::C_BaseEntity::m_pGameSceneNode;
    case "m_iHealth"_hash:return f::C_BaseEntity::m_iHealth;
    case "m_lifeState"_hash:return f::C_BaseEntity::m_lifeState;
    case "m_iTeamNum"_hash:return f::C_BaseEntity::m_iTeamNum;
    case "m_bGunGameImmunity"_hash:return immunityOffset;
    case "m_bDormant"_hash:return dormantOffset;
    case "m_ArmorValue"_hash:return armorOffset;
    case "m_pItemServices"_hash:return itemsOffset;
    case "m_bHasHelmet"_hash:return helmetOffset;
    case "m_flHeadshotMultiplier"_hash:return headMultiplierOffset;
    case "m_flArmorRatio"_hash:return armorRatioOffset;
    case "m_vecAbsOrigin"_hash:return originOffset;
    case "m_angAbsRotation"_hash:return rotationOffset;
    case "m_vecViewOffset"_hash:return viewOffset;
    case "m_pWeaponServices"_hash:return servicesOffset;
    case "m_hActiveWeapon"_hash:return activeOffset;
    case "m_nSubclassID"_hash:return subclassOffset;
    case "m_WeaponType"_hash:return typeOffset;
    case "m_nDamage"_hash:return damageOffset;
    case "m_flPenetration"_hash:return penetrationOffset;
    case "m_flRange"_hash:return rangeOffset;
    case "m_flRangeModifier"_hash:return modifierOffset;
    default:return 0;
    }
}
}
namespace math {
vector3 vector3::operator+(const vector3 &v)const noexcept{return {x+v.x,y+v.y,z+v.z};}
vector3 vector3::operator-(const vector3 &v)const noexcept{return {x-v.x,y-v.y,z-v.z};}
vector3 vector3::operator*(float s)const noexcept{return {x*s,y*s,z*s};}
vector3 vector3::operator/(float s)const noexcept{return {x/s,y/s,z/s};}
vector3 vector3::operator-()const noexcept{return {-x,-y,-z};}
float vector3::dot(const vector3 &v)const noexcept{return x*v.x+y*v.y+z*v.z;}
vector3 vector3::cross(const vector3 &v)const noexcept{return {y*v.z-z*v.y,z*v.x-x*v.z,x*v.y-y*v.x};}
float vector3::length_sqr()const noexcept{return dot(*this);}
float vector3::length()const noexcept{return std::sqrt(length_sqr());}
}
int main() {
    using namespace fixture;
    try {
        InitializeFixture();Check(Initialize(),"Expected native bindings");
        auto result=Probe();Check(result.state==State::Pass&&result.surface&&result.positionValid&&result.damage==20&&std::abs(result.position.x-100)<.01f&&result.normal.x==-1,"Air-leading penetrated wall or normal failed");
        Check(result.contactIndex==1&&std::abs(result.thickness-10)<.01f,"Material diagnostic/thickness incorrect");
        Check(appliedDamage==30&&appliedPenetration==2&&appliedModifier==.98f,"Current weapon stats missing");
        Defaults();nativeDeltaScale=.2f;plan[0].exitFraction=plan[1].enterFraction=.5f;plan[1].exitFraction=.55f;
        result=Probe();Check(result.state==State::Pass&&std::abs(result.penetrationRange-200)<.01f&&std::abs(result.materialEntry.x-100)<.01f&&std::abs(result.thickness-10)<.01f&&result.materialError<.01f,"Native shortened delta/fraction basis was ignored");
        Defaults();nativeDeltaScale=1.09f;plan[0].exitFraction=plan[1].enterFraction=100.f/1090.f;plan[1].exitFraction=110.f/1090.f;
        result=Probe();Check(result.state==State::Pass&&std::abs(result.penetrationRange-1090)<.01f&&result.materialError<.01f&&std::abs(result.thickness-10)<.01f,"Native90-unit ray extension was rejected");
        Defaults();nativeDeltaScale=1.363f;result=Probe();Check(result.state==State::Unavailable&&result.reason==Reason::InvalidTraceRay,"Native delta beyond documented four90-unit extensions was accepted");
        Defaults();wrongNativeDirection=true;result=Probe();Check(result.state==State::Unavailable&&result.reason==Reason::InvalidTraceRay,"Native wrong-direction delta was accepted");
        Defaults();plan={{0,1,30,0,0,0,0,{}}};result=Probe();Check(result.state==State::Unavailable&&result.surface&&result.reason==Reason::NoMaterialRecord,"Missing material record lacked a specific diagnostic");
        Defaults();
        Put(Ptr(vdata)+damageOffset,42);Put(Ptr(vdata)+penetrationOffset,1.5f);Probe();Check(appliedDamage==42&&appliedPenetration==1.5f,"Current weapon stats were cached stale");
        Defaults();plan.insert(plan.begin(),Contact{0,0,30,0,0,0,0,{}});result=Probe();Check(result.state==State::Pass&&result.contactIndex==2,"Zero-length leading air masked wall");
        Defaults();plan[1].exitFraction=1;plan[1].damage=0;result=Probe();Check(result.state==State::Blocked&&result.reason==Reason::Stopped&&result.surface,"Stopped wall incorrect");
        Defaults();plan[1].damage=0;result=Probe();Check(result.state==State::Blocked&&result.reason==Reason::DamageExhausted,"Exhausted damage incorrect");
        Defaults();rayAllSolid=true;result=Probe();Check(result.state==State::Unavailable&&!result.surface&&result.allSolid&&result.reason==Reason::AllSolid,"Embedded allSolid ray was accepted");
        Defaults();rayFraction=std::numeric_limits<float>::quiet_NaN();result=Probe();Check(result.state==State::Unavailable&&result.reason==Reason::InvalidFraction,"Invalid fraction lacked a specific diagnostic");
        Defaults();rayNormal={};result=Probe();Check(result.state==State::Unavailable&&result.reason==Reason::InvalidHitNormal&&result.normalLength==0,"Zero surface normal lacked a specific diagnostic");
        Defaults();wrongEndpoint=true;result=Probe();Check(result.state==State::Unavailable&&result.reason==Reason::RayEndpointMismatch&&std::abs(result.endpointError-20)<.01f,"Endpoint mismatch lacked a specific diagnostic");
        Defaults();rayFraction=0;result=Probe();Check(result.state==State::Unavailable&&!result.surface&&result.reason==Reason::InvalidHitDistance,"Embedded zero-distance hit was accepted");
        Defaults();rayClear=true;const auto before=bulletCalls;result=Probe();Check(result.state==State::Clear&&!result.surface&&bulletCalls==before,"Native false/clear ray incorrect");
        Defaults();plan[1].enterFraction=std::numeric_limits<float>::quiet_NaN();result=Probe();Check(result.state==State::Unavailable&&result.surface&&result.reason==Reason::InvalidContact,"Malformed material advertised blocked/pass");
        Defaults();plan[1].enterFraction=.15f;result=Probe();Check(result.state==State::Unavailable&&result.reason==Reason::FirstMaterialMismatch,"Mismatched wall incorrectly advertised penetrable");
        Defaults();plan={{0,0,30,0,0,0,0,{}},{0,.1f,29,0,0,0,0,{}},{.1f,.11f,17,0,0,0,1,{}}};replaceContacts=true;result=Probe();Check(result.state==State::Pass&&result.contactIndex==2&&result.contactCount==3&&result.damage==17,"Native replacement/contact count not consumed live");
        Defaults();grown=true;result=Probe();Check(result.state==State::Pass&&storage.surfaceCapacity==192,"Grown native capacity rejected");result=Probe();Check(result.state==State::Pass,"Grown storage not retained");
        Defaults();grown=true;plan.assign(131,Contact{0,0,30,0,0,0,0,{}});plan.back()={.1f,.11f,12,0,0,0,1,{}};result=Probe();Check(result.state==State::Pass&&result.contactCount==131&&result.contactIndex==130,"Live contacts above inline capacity were rejected");
        const auto saved=storage.contactArray;failReset=true;Check(!Reset()&&storage.contactArray==saved,"Cleanup failure lost owned storage");failReset=false;Check(Reset()&&!storage.contactArray,"Cleanup retry failed");
        Defaults();missingBullet=true;result=Probe();Check(result.state==State::Unavailable&&result.surface&&result.reason==Reason::BindingsUnavailable,"Missing bullet bindings lost aimed amber surface");missingBullet=false;now+=501;result=Probe();Check(result.state==State::Pass,"Failed binding did not retry");
        Defaults();Put(Ptr(vdata)+typeOffset,0);result=Probe();Check(result.surface&&result.state==State::Unavailable&&result.reason==Reason::UnsupportedWeapon,"Unsupported weapon not amber");
        Defaults();failSetup=true;result=Probe();Check(result.surface&&result.state==State::Unavailable&&result.reason==Reason::TraceSetupFailed,"Setup SEH failed open");
        Defaults();failRay=true;result=Probe();Check(!result.surface&&result.state==State::Unavailable&&result.reason==Reason::RayFailed,"Ray SEH produced fake surface");
        Defaults();failBullet=true;result=Probe();Check(result.surface&&result.state==State::Unavailable&&result.reason==Reason::SimulationFailed,"Native SEH failed open");
        Defaults();invalidateWeapon=true;result=Probe();Check(result.state==State::Unavailable&&result.reason==Reason::InvalidWeapon,"Stale weapon handle published");
        Defaults();matrix={};result=Probe();Check(result.state==State::Unavailable&&!result.surface&&result.reason==Reason::InvalidCamera,"Degenerate camera accepted");
        Defaults();matrix.mat[0][3]=-10;matrix.mat[1][3]=-70;matrix.mat[3][3]=20;Vec3 origin,dir;Check(Camera(matrix,origin,dir)&&std::abs(origin.x+20)<.01f&&std::abs(origin.y-10)<.01f&&std::abs(origin.z-70)<.01f&&dir.x==1,"Third person center camera inverse incorrect");
        const float selectedDistance=std::sqrt(80.f*80.f+10.f*10.f+6.f*6.f);
        plan[0].exitFraction=plan[1].enterFraction=selectedDistance/1000.f;plan[1].exitFraction=plan[1].enterFraction+.01f;
        result=Probe();Check(result.state==State::Pass&&std::abs(result.position.x-80)<.01f&&std::abs(result.position.y-10)<.01f&&std::abs(result.eyeDistance-selectedDistance)<.01f&&result.direction.x==1,"Third person visible ray and eye ballistic endpoint did not match");
        // Discovery uses the same audited ballistics without requiring a
        // render matrix or restricting queries to the current camera frustum.
        Defaults();const auto matrixAddress=addresses::globals::view_matrix;addresses::globals::view_matrix=0;
        auto profile=CaptureProfile(72);
        Check(profile.valid&&profile.stamp.session==72&&Near(profile.cameraOrigin,profile.eyeOrigin)&&IsCurrent(profile),
            "Camera-independent discovery profile required a view matrix");
        Check(!CaptureFrame().valid&&CaptureFrame().reason==Reason::InvalidCamera&&ProbeAt(profile,0,0).reason==Reason::InvalidCamera,
            "Camera-dependent query accepted a profile without a view matrix");
        for(const Vec3 direction:{Vec3{10,0,0},Vec3{-10,0,0},Vec3{0,10,0},Vec3{0,-10,0},Vec3{0,0,10},Vec3{0,0,-10}}) {
            auto unit=direction;Normalize(unit);rayNormal=-unit;
            result=ProbeRay(profile,profile.eyeOrigin,direction);
            Check(result.state==State::Pass&&result.stamp==profile.stamp&&Near(result.direction,unit)&&
                Near(result.position,profile.eyeOrigin+unit*100)&&Near(result.exitPoint,profile.eyeOrigin+unit*110)&&
                result.materialIdentityValid&&result.physicsShape==materialShape,
                "Camera-independent360 ray lost origin, direction or exact physical identity");
        }
        addresses::globals::view_matrix=matrixAddress;
        Defaults();profile=CaptureProfile();expectedSetupOrigin=Vec3{50,25,84};
        result=ProbeRay(profile,*expectedSetupOrigin,{1,0,0});
        Check(result.state==State::Pass&&Near(result.eyeOrigin,*expectedSetupOrigin)&&Near(result.cameraOrigin,*expectedSetupOrigin)&&
            Near(result.position,{150,25,84})&&Near(result.exitPoint,{160,25,84}),
            "Synthetic candidate probe silently used the player's eye");
        const auto invalidQueryBefore=filterCalls+setupCalls+bulletCalls;
        result=ProbeRay(profile,{NAN,0,0},{1,0,0});Check(result.reason==Reason::InvalidPoint,"Nonfinite candidate origin accepted");
        result=ProbeRay(profile,profile.eyeOrigin,{});Check(result.reason==Reason::InvalidTraceRay,"Zero discovery direction accepted");
        result=ProbeRay(profile,profile.eyeOrigin,{1,NAN,0});Check(result.reason==Reason::InvalidTraceRay,"Nonfinite discovery direction accepted");
        Check(filterCalls+setupCalls+bulletCalls==invalidQueryBefore,"Invalid discovery ray invoked native calls");
        Defaults();profile=CaptureProfile();materialShape+=8;result=ProbeRay(profile,profile.eyeOrigin,{1,0,0});
        Check(result.reason==Reason::SurfaceMismatch&&result.materialPhysicsShape==materialShape,
            "Camera-independent query weakened physical shape discrimination");
        Defaults();profile=CaptureProfile();plan[1].exitFraction=1;plan[1].damage=0;
        result=ProbeRay(profile,profile.eyeOrigin,{1,0,0});Check(result.state==State::Blocked&&result.reason==Reason::Stopped,
            "Camera-independent discovery advertised a stopped wall as passing");
        Defaults();profile=CaptureProfile();plan.push_back(Contact{.2f,1,0,0,0,0,1,{}});
        result=ProbeRay(profile,profile.eyeOrigin,{1,0,0});Check(result.state==State::Pass&&result.contactIndex==1&&result.damage==20,
            "Later native stop incorrectly invalidated the first passing surface");
        Defaults();profile=CaptureProfile();Put(Ptr(services)+activeOffset,pawnHandle);
        const auto profileStaleBefore=setupCalls;result=ProbeRay(profile,profile.eyeOrigin,{1,0,0});
        Check(result.reason==Reason::StaleFrame&&setupCalls==profileStaleBefore,"Stale active weapon reached discovery tracing");
        Defaults();profile=CaptureProfile();moveLocal=true;result=ProbeRay(profile,profile.eyeOrigin,{1,0,0});
        Check(result.reason==Reason::StaleFrame&&result.state==State::Unavailable,"Local movement in native discovery callback published green");
        Defaults();profile=CaptureProfile();changeDamage=true;result=ProbeRay(profile,profile.eyeOrigin,{1,0,0});
        Check(result.reason==Reason::StaleFrame,"Weapon profile changed during discovery published green");
        // Capture once, sample actual matrix rays and invalidate stale state.
        Defaults();auto frame=CaptureFrame(73);
        Check(frame.valid&&frame.stamp.session==73&&frame.stamp.pawn==Ptr(pawn)&&frame.stamp.weapon==Ptr(weapon)&&
            frame.stamp.weaponData==Ptr(vdata)&&frame.stamp.localHandle==pawnHandle&&frame.stamp.weaponHandle==weaponHandle&&
            frame.stamp.damage==30&&frame.eyeOrigin.z==64&&frame.sceneNode==Ptr(scene),"Frame did not capture current native player/weapon identity");
        const auto nativeBefore=setupCalls+bulletCalls+filterCalls;
        Check(IsCurrent(frame)&&setupCalls+bulletCalls+filterCalls==nativeBefore,"Frame freshness invoked native tracing");
        result=ProbeAt(frame,.5f,-.25f);Vec3 expectedDirection{1,.5f,-.25f};Normalize(expectedDirection);
        Check(result.state==State::Pass&&result.stamp==frame.stamp&&result.kind==SurfaceKind::World&&
            result.physicsObject==rayObject&&result.physicsShape==rayShape&&result.hitHandle==0&&
            Near(result.direction,expectedDirection)&&Near(result.position,frame.cameraOrigin+expectedDirection*100)&&
            Near(result.exitPoint,frame.eyeOrigin+expectedDirection*110)&&IsSurfaceCurrent(result),
            "Offcenter NDC ray, physics identity or material exit point incorrect");
        const auto queryBefore=setupCalls;result=ProbeAt(frame,1.01f,0);
        Check(result.state==State::Unavailable&&result.reason==Reason::InvalidCamera&&setupCalls==queryBefore,"Out-of-view NDC invoked penetration");
        result=ProbeAt(frame,std::numeric_limits<float>::quiet_NaN(),0);
        Check(result.reason==Reason::InvalidCamera,"Nonfinite NDC accepted");
        Defaults();shootEyeOffset={10,0,0};frame=CaptureFrame(74);
        plan[0].exitFraction=plan[1].enterFraction=.09f;plan[1].exitFraction=.10f;
        result=ProbeAt(frame,0,0);
        Check(result.state==State::Pass&&frame.eyeOrigin.x==10&&result.eyeOrigin.x==10&&result.position.x==100&&
            result.materialError<.01f&&result.exitPoint.x==110,"Surface query used field eye instead of native shooter position");
        Defaults();frame=CaptureFrame();Put(Ptr(vdata)+damageOffset,31);
        const auto staleBefore=setupCalls;result=ProbeAt(frame,0,0);
        Check(!IsCurrent(frame)&&result.reason==Reason::StaleFrame&&setupCalls==staleBefore,"Changed weapon stats reused a captured green result");
        Defaults();frame=CaptureFrame();Put(Ptr(services)+activeOffset,pawnHandle);
        Check(!IsCurrent(frame)&&ProbeAt(frame,0,0).reason==Reason::StaleFrame,"Still-resolvable old weapon bypassed active-slot validation");
        Defaults();frame=CaptureFrame();Put(Ptr(scene)+originOffset,Vec3{1,0,0});
        Check(!IsCurrent(frame),"Moving local field origin retained captured native eye");
        Defaults();frame=CaptureFrame();Put(Ptr(pawn)+viewOffset,Vec3{0,0,63});
        Check(!IsCurrent(frame),"Crouched local retained captured native eye");
        Defaults();frame=CaptureFrame();switchOnRay=true;const auto beforeRaySwitch=setupCalls;result=ProbeAt(frame,0,0);
        Check(result.state==State::Unavailable&&result.reason==Reason::StaleFrame&&setupCalls==beforeRaySwitch,"Weapon switch in surface callback reached native penetration");
        Defaults();frame=CaptureFrame();switchWeapon=true;result=ProbeAt(frame,0,0);
        Check(result.state==State::Unavailable&&result.reason==Reason::StaleFrame,"Weapon switch in bullet callback published green");
        Defaults();frame=CaptureFrame();changeDamage=true;result=ProbeAt(frame,0,0);
        Check(result.state==State::Unavailable&&result.reason==Reason::StaleFrame,"Weapon stats changed in native callback published green");
        Defaults();frame=CaptureFrame();moveLocal=true;result=ProbeAt(frame,0,0);
        Check(result.state==State::Unavailable&&result.reason==Reason::StaleFrame,"Moving local in native callback published green");
        // Both -1 and -2 are explicit native no-entity sentinels. Basic and
        // bulk traces may preserve either value; their opaque shape must match.
        for(const auto raySentinel:{0xffffffffu,0xfffffffeu})for(const auto materialSentinel:{0xffffffffu,0xfffffffeu}) {
            Defaults();rayOwner=raySentinel;surfaceHandles={materialSentinel};frame=CaptureFrame();
            result=ProbeAt(frame,0,0);
            Check(result.state==State::Pass&&result.kind==SurfaceKind::World&&result.hitHandle==raySentinel&&
                result.materialIdentityValid&&result.materialOwner==materialSentinel,
                "Native no-entity sentinel rejected or erased from diagnostics");
        }
        Defaults();rayOwner=targetHandle;frame=CaptureFrame();result=ProbeAt(frame,0,0);
        Check(result.reason==Reason::InvalidSurfaceIdentity,"Unresolved real entity handle was treated as anonymous world");
        Defaults();rayOwner=0xfffffffeu;surfaceHandles={targetHandle};frame=CaptureFrame();result=ProbeAt(frame,0,0);
        Check(result.reason==Reason::SurfaceMismatch&&result.materialOwner==targetHandle,
            "World sentinel broadly accepted a real mismatched material owner");
        Defaults();frame=CaptureFrame();materialShape+=8;result=ProbeAt(frame,0,0);
        Check(result.state==State::Unavailable&&result.reason==Reason::SurfaceMismatch&&result.materialIdentityValid&&
            result.materialPhysicsObject==materialObject&&result.materialPhysicsShape==materialShape&&
            result.physicsShape==rayShape&&result.materialType==0,
            "Adjacent different physics shape accepted or mismatch diagnostics lost");
        Defaults();frame=CaptureFrame();materialObject+=8;result=ProbeAt(frame,0,0);
        Check(result.reason==Reason::SurfaceMismatch,"Different physics object accepted");
        Defaults();frame=CaptureFrame();materialType=1;result=ProbeAt(frame,0,0);
        Check(result.reason==Reason::SurfaceMismatch&&result.materialIdentityValid&&result.materialType==1,
            "Actor hitbox data was treated as physical object pointers or type diagnostic lost");
        Defaults();frame=CaptureFrame();rayObject=rayShape=0;result=ProbeAt(frame,0,0);
        Check(result.kind==SurfaceKind::Unknown&&result.reason==Reason::InvalidSurfaceIdentity&&!result.materialIdentityValid,
            "Anonymous material pointer became a surface identity or uncaptured material diagnostics appeared valid");
        Defaults();frame=CaptureFrame();rayClear=true;const auto beforeClear=setupCalls;result=ProbeAt(frame,.25f,.25f);
        Check(result.state==State::Clear&&!result.surface&&setupCalls==beforeClear&&result.stamp==frame.stamp,"Clear arbitrary ray invoked bullet tracing or lost stamp");
        Defaults();frame=CaptureFrame();plan[1].exitFraction=1;plan[1].damage=0;result=ProbeAt(frame,0,0);
        Check(result.state==State::Blocked&&result.exitPoint.length_sqr()==0,"Stopped fraction1 sentinel was advertised as a real back face");
        const auto Prop=[] {
            Defaults();rayEntity=Ptr(target);rayOwner=targetHandle;surfaceHandles={targetHandle};
            Put(Ptr(schema)+8,Ptr(propClassName));return CaptureFrame(75);
        };
        frame=Prop();result=ProbeAt(frame,0,0);
        Check(result.state==State::Pass&&result.kind==SurfaceKind::Prop&&result.hitEntity==Ptr(target)&&
            result.hitHandle==targetHandle&&result.sceneNode==Ptr(targetScene)&&IsSurfaceCurrent(result),"Full-serial prop was not identified/captured");
        const auto prior=result;Put(Ptr(targetScene)+originOffset,Vec3{1,0,0});
        Check(!IsSurfaceCurrent(prior),"Moved prop retained green cached patch");
        frame=Prop();result=ProbeAt(frame,0,0);Put(Ptr(targetScene)+rotationOffset,Vec3{0,1,0});
        Check(!IsSurfaceCurrent(result),"Rotated door retained green cached patch");
        frame=Prop();result=ProbeAt(frame,0,0);Entry(102,Ptr(target),targetHandle+(1u<<15));
        Check(!IsSurfaceCurrent(result),"Recycled prop serial retained cached physics shape");
        frame=Prop();rayOwner=targetHandle+(1u<<15);result=ProbeAt(frame,0,0);
        Check(result.state==State::Unavailable&&result.kind==SurfaceKind::Unknown&&result.reason==Reason::InvalidSurfaceIdentity,"Stale prop owner handle accepted");
        frame=Prop();moveProp=true;result=ProbeAt(frame,0,0);
        Check(result.state==State::Unavailable&&result.reason==Reason::StaleFrame,"Prop moved in simulation callback published green");
        frame=Prop();surfaceHandles={0};result=ProbeAt(frame,0,0);
        Check(result.reason==Reason::SurfaceMismatch,"Prop owner mismatch accepted despite matching opaque shape");
        for(const char *name:{pawnClassName,weaponClassName,projectileClassName,"C_AK47","C_DEagle","C_Knife",
                "C_Item_Healthshot","C_Flashbang","C_Inferno","C_CS2HudModelArms","C_Hostage","C_Chicken"}) {
            frame=Prop();Put(Ptr(schema)+8,reinterpret_cast<Address>(name));const auto excludedBefore=setupCalls;result=ProbeAt(frame,0,0);
            Check(result.kind==SurfaceKind::Excluded&&result.reason==Reason::ExcludedSurface&&setupCalls==excludedBefore,"Player/weapon/projectile surface entered wall overlay");
        }
        Defaults();failShootEye=true;frame=CaptureFrame();
        Check(!frame.valid&&frame.reason==Reason::InvalidEye,"Missing native shooter eye fabricated frame");
        Defaults();missingBullet=true;frame=CaptureFrame();Check(!frame.valid&&frame.reason==Reason::BindingsUnavailable,"Missing binding produced valid batch frame");
        missingBullet=false;now+=501;Check(CaptureFrame().valid,"Frame binding retry failed");
        Defaults();Put(Ptr(vdata)+typeOffset,0);frame=CaptureFrame();
        Check(!frame.valid&&frame.reason==Reason::UnsupportedWeapon,"Nonfirearm created penetrable surface frame");
        // Real target arrival is the free-travel EXIT boundary, before flesh stops.
        const auto Direct=[] {
            Defaults();surfaceHandles={targetHandle};
            plan={{0,.2f,30,0,0,0,0,{}},{.2f,1,0,0,0,0,1,{}}};
        };
        const auto BehindWall=[] {
            Defaults();surfaceHandles={0,0,targetHandle};
            plan={{0,.1f,30,0,0,0,0,{}},{.1f,.11f,20,0,0,1,1,{}},
                  {.11f,.2f,19,0,1,2,0,{}},{.2f,1,0,0,2,2,1,{}}};
        };
        Direct();auto targetResult=EvaluateTarget(Ptr(target),targetHandle,{200,0,64});
        {
            const combat_bindings::HistoryShotTiming time{1500,.25f};
            Direct();auto historical=EvaluateHistoricalTarget(Ptr(target),targetHandle,{150,0,64},time);
            Check(historical.valid&&historical.reached&&!historical.penetrated&&historical.rawDamage==30,"Historical hitbox in free travel was not reachable");
            BehindWall();historical=EvaluateHistoricalTarget(Ptr(target),targetHandle,{150,0,64},time);
            Check(historical.reached&&historical.penetrated&&historical.rawDamage==19,"Historical penetration ignored native post-wall damage");
            BehindWall();historical=EvaluateHistoricalTarget(Ptr(target),targetHandle,{105,0,64},time);
            Check(!historical.reached,"Historical point inside a wall was accepted");
            BehindWall();plan[1].exitFraction=1;historical=EvaluateHistoricalTarget(Ptr(target),targetHandle,{150,0,64},time);
            Check(!historical.reached,"Historical target beyond stopped material was accepted");
            Direct();plan.clear();surfaceHandles.clear();historical=EvaluateHistoricalTarget(Ptr(target),targetHandle,{150,0,64},time);
            Check(historical.reached&&historical.rawDamage<30&&historical.rawDamage>29,"Historical empty ray failed range falloff");
            Direct();invalidateTarget=true;historical=EvaluateHistoricalTarget(Ptr(target),targetHandle,{150,0,64},time);
            Check(!historical.valid&&!historical.reached,"Historical trace accepted a replaced target");
            Direct();wrongNativeDirection=true;historical=EvaluateHistoricalTarget(Ptr(target),targetHandle,{150,0,64},time);
            Check(!historical.valid&&!historical.reached,"Historical trace accepted a different native direction");
            Direct();
        }
        Check(targetResult.valid&&targetResult.reached&&!targetResult.penetrated&&targetResult.rawDamage==30&&targetResult.damage==30&&targetResult.weaponHandle==weaponHandle&&targetResult.contactIndex==0&&targetResult.position.x==200&&targetResult.eyeOrigin.z==64&&targetResult.localHandle==pawnHandle,"Direct target was lost to stopped flesh or not identified by native boundary");
        Direct();matchedEyeOffset={10,0,0};
        targetResult=EvaluateTarget(Ptr(target),targetHandle,{210,0,64},0,combat_bindings::HistoryShotTiming{1500,.25f});
        Check(targetResult.reached&&targetResult.eyeOrigin.x==10&&targetResult.position.x==210&&lastShotTiming.tick==1500&&lastShotTiming.fraction==.25f,
              "Target penetration ignored the supplied time-matched native eye");
        failShotEye=true;targetResult=EvaluateTarget(Ptr(target),targetHandle,{210,0,64},0,combat_bindings::HistoryShotTiming{1500,.25f});
        Check(!targetResult.valid&&targetResult.reason==Reason::InvalidEye,"Unavailable matched eye fabricated a target trace");
        failShotEye=false;matchedEyeOffset={};
        BehindWall();targetResult=EvaluateTarget(Ptr(target),targetHandle,{200,0,64},1);
        Check(targetResult.valid&&targetResult.reached&&targetResult.penetrated&&targetResult.rawDamage==19&&targetResult.damage==76&&targetResult.contactIndex==2,"Post-wall target did not use free-travel exit boundary and pre-flesh damage");
        Direct();Put(Ptr(target)+armorOffset,50);Put(Ptr(items)+helmetOffset,true);
        targetResult=EvaluateTarget(Ptr(target),targetHandle,{200,0,64},1);Check(targetResult.reached&&targetResult.damage==90,"Head armor damage scaling incorrect");
        Put(Ptr(items)+helmetOffset,false);targetResult=EvaluateTarget(Ptr(target),targetHandle,{200,0,64},1);Check(targetResult.reached&&targetResult.damage==120,"Unhelmeted head armor incorrectly reduced damage");
        targetResult=EvaluateTarget(Ptr(target),targetHandle,{200,0,64},2);Check(targetResult.damage==22,"Body armor damage scaling incorrect");
        targetResult=EvaluateTarget(Ptr(target),targetHandle,{200,0,64},6);Check(targetResult.damage==22,"Leg scaling or armor rule incorrect");
        Direct();nativeDeltaScale=.2f;plan[0].exitFraction=.5f;plan[1].enterFraction=.5f;
        targetResult=EvaluateTarget(Ptr(target),targetHandle,{100,0,64});Check(targetResult.reached&&targetResult.position.x==100,"Target arrival ignored shortened native ray basis");
        Direct();nativeDeltaScale=1.09f;plan[0].exitFraction=200.f/1090.f;plan[1].enterFraction=200.f/1090.f;
        targetResult=EvaluateTarget(Ptr(target),targetHandle,{200,0,64});Check(targetResult.reached&&std::abs(targetResult.position.x-200)<.01f,"Target arrival rejected documented native extension");
        BehindWall();grown=true;targetResult=EvaluateTarget(Ptr(target),targetHandle,{200,0,64});Check(targetResult.reached,"Grown target contact storage rejected");
        targetResult=EvaluateTarget(Ptr(target),targetHandle,{200,0,64});Check(targetResult.reached,"Target query discarded retained native storage");
        BehindWall();replaceContacts=true;targetResult=EvaluateTarget(Ptr(target),targetHandle,{200,0,64});Check(targetResult.reached&&targetResult.contactCount==4,"Target query ignored live post-simulation pointer/count replacement");
        Direct();surfaceHandles={0};targetResult=EvaluateTarget(Ptr(target),targetHandle,{200,0,64});Check(!targetResult.reached,"Empty ray was incorrectly treated as target arrival");
        Direct();plan={};surfaceHandles={};targetResult=EvaluateTarget(Ptr(target),targetHandle,{200,0,64});Check(targetResult.valid&&!targetResult.reached&&targetResult.reason==Reason::TargetNotReached,"Clear target ray advertised a hit");
        Direct();plan[0].exit=1;targetResult=EvaluateTarget(Ptr(target),targetHandle,{200,0,64});Check(!targetResult.valid&&!targetResult.reached&&targetResult.reason==Reason::InvalidContact,"Unused surface capacity masqueraded as target boundary");
        BehindWall();plan[1].exitFraction=1;plan[1].damage=0;
        targetResult=EvaluateTarget(Ptr(target),targetHandle,{200,0,64});Check(targetResult.valid&&!targetResult.reached,"Stopped wall reached target behind it");
        Direct();plan.erase(plan.begin());targetResult=EvaluateTarget(Ptr(target),targetHandle,{200,0,64});Check(!targetResult.reached,"Started-inside flesh fabricated an unproven target arrival");
        Direct();const auto beforeTargetSetup=setupCalls;
        targetResult=EvaluateTarget(Ptr(target),targetHandle+(1u<<15),{200,0,64});Check(!targetResult.valid&&targetResult.reason==Reason::InvalidTarget&&setupCalls==beforeTargetSetup,"Stale target serial accepted or native trace called before target validation");
        Direct();Put(Ptr(target)+f::C_BaseEntity::m_iTeamNum,std::uint8_t{2});
        targetResult=EvaluateTarget(Ptr(target),targetHandle,{200,0,64});Check(!targetResult.valid&&targetResult.reason==Reason::InvalidTarget,"Friendly pawn accepted as enemy");
        Direct();Put(Ptr(target)+f::C_BaseEntity::m_iHealth,0);
        targetResult=EvaluateTarget(Ptr(target),targetHandle,{200,0,64});Check(!targetResult.valid&&targetResult.reason==Reason::InvalidTarget,"Dead pawn accepted as target");
        Direct();Put(Ptr(target)+immunityOffset,true);
        targetResult=EvaluateTarget(Ptr(target),targetHandle,{200,0,64});Check(!targetResult.valid&&targetResult.reason==Reason::InvalidTarget,"Immune pawn accepted as target");
        Direct();Put(Ptr(targetScene)+dormantOffset,true);
        targetResult=EvaluateTarget(Ptr(target),targetHandle,{200,0,64});Check(!targetResult.valid&&targetResult.reason==Reason::InvalidTarget,"Dormant pawn accepted as target");
        Direct();Put(Ptr(schema)+8,Ptr(propClassName));
        targetResult=EvaluateTarget(Ptr(target),targetHandle,{200,0,64});Check(!targetResult.valid&&targetResult.reason==Reason::InvalidTarget,"Non-player entity accepted as target");
        Direct();invalidateTarget=true;targetResult=EvaluateTarget(Ptr(target),targetHandle,{200,0,64});Check(!targetResult.valid&&targetResult.reason==Reason::InvalidTarget,"Target serial replacement during native call accepted");
        Direct();killTarget=true;targetResult=EvaluateTarget(Ptr(target),targetHandle,{200,0,64});Check(!targetResult.valid&&targetResult.reason==Reason::InvalidTarget,"Target dying during native call accepted");
        Direct();invalidateWeapon=true;targetResult=EvaluateTarget(Ptr(target),targetHandle,{200,0,64});Check(!targetResult.valid&&targetResult.reason==Reason::InvalidWeapon,"Weapon serial replacement during native call accepted");
        Direct();switchWeapon=true;targetResult=EvaluateTarget(Ptr(target),targetHandle,{200,0,64});Check(!targetResult.valid&&targetResult.reason==Reason::InvalidWeapon,"Changed active weapon was ignored");
        Direct();targetResult=EvaluateTarget(Ptr(target),targetHandle,{1200,0,64});Check(targetResult.valid&&!targetResult.reached&&targetResult.reason==Reason::OutOfRange,"Target beyond weapon range accepted");
        Direct();targetResult=EvaluateTarget(Ptr(target),targetHandle,{std::numeric_limits<float>::quiet_NaN(),0,64});Check(!targetResult.valid&&targetResult.reason==Reason::InvalidPoint,"Non-finite target point accepted");
        Direct();wrongNativeDirection=true;targetResult=EvaluateTarget(Ptr(target),targetHandle,{200,0,64});Check(!targetResult.valid&&targetResult.reason==Reason::InvalidTraceRay,"Wrong native ray direction accepted for target");
        Direct();wrongNativeStart=true;targetResult=EvaluateTarget(Ptr(target),targetHandle,{200,0,64});Check(!targetResult.valid&&targetResult.reason==Reason::InvalidTraceRay,"Shifted native ray origin accepted for target");
        BehindWall();plan[1].enter=3;targetResult=EvaluateTarget(Ptr(target),targetHandle,{200,0,64});Check(!targetResult.valid&&targetResult.reason==Reason::InvalidContact,"Invalid material boundary accepted for target");
        Direct();failBullet=true;targetResult=EvaluateTarget(Ptr(target),targetHandle,{200,0,64});Check(!targetResult.valid&&targetResult.reason==Reason::SimulationFailed,"Native target simulation fault failed open");
        Check(Reset(),"Final cleanup failed");VirtualFree(reinterpret_cast<void*>(client),0,MEM_RELEASE);game_access::verifiedClient=0;
        std::cout<<"Autowall probe regression PASS\n";return 0;
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
