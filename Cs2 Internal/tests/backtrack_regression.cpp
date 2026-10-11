#include "features/combat/backtrack.cpp"
#include "features/visuals/backtrack_chams.cpp"
#include "player_fixture.h"
#include <iostream>
#include <limits>

namespace fixture {
using Address=std::uintptr_t;
namespace f=game_access::field;
PlayerFixture local;
std::array<std::byte,0x4000> pawn{},scene{},model{},globals{};
Address modelBinding{},globalPointer{};
std::array<systems::bones::data,128> bones;
std::array<std::byte,0x150> ghostScene;
std::array<std::array<float,12>,128> renderBones;
std::vector<combat_access::Target> targets;
constexpr std::uint32_t handle=0x10002;
constexpr unsigned simulation=0x780,origin=0x200,state=0x400,transform=0x240,immunity=0x7a0,dormant=0x210,groupId=0x220;
unsigned creates{},destroys{},draws{},materials{},geometryReads{},boneCopies{},fullHistoryReads{};
bool failCreate{},failPose{},changeCountDuringGeometry{};
float maxUnlag=.2f,playerUnlag{};
template<class T> Address Ptr(T& value){return reinterpret_cast<Address>(&value);}
template<class T> void Put(Address address,T value){Check(game_access::Write(address,value),"Fixture memory write failed");}
void Move(float time,float x){
    Put(Ptr(globals)+0x30,time);Put(Ptr(pawn)+simulation,time);
    Put(Ptr(scene)+origin,math::vector3{x,200,30});
    for(auto& bone:bones){bone.position={x,200,64};bone.scale=1;bone.rotation={0,0,0,1};}
    std::array<float,8> world{x,200,30,1,0,0,0,1};
    Put(Ptr(scene)+transform,world);
}
Address __fastcall WorldGroup(Address system,int group){Check(system==123&&group==0,"Ghost world-group ABI changed");return 456;}
Address __fastcall Create(Address mesh,Address binding,float* world,const char* descriptor,
    std::uint64_t flags,std::uint64_t drawFlags,Address group){
    Check(mesh==10&&binding==Ptr(modelBinding)&&world&&std::strcmp(descriptor,"AnimatableSceneObjectDesc")==0&&
        flags==0x2000000008ull&&drawFlags==0x4100000001ull&&group==456,"Ghost creation ABI changed");
    ++creates;if(failCreate)return 0;
    ghostScene.fill({});renderBones={};
    Put(Ptr(ghostScene)+chams_bindings::kRenderBoneCount,128);
    Put(Ptr(ghostScene)+chams_bindings::kRenderBones,Ptr(renderBones));
    return Ptr(ghostScene);
}
void __fastcall Destroy(Address system,Address object){Check(system==20&&object==Ptr(ghostScene),"Ghost destroy ABI changed");++destroys;}
void __fastcall Draw(Address,Address,Address,Address){++draws;}
void Setup(){
    visuals::backtrack_chams::Reset();backtrack::Reset();backtrack::Set({true,100});
    pawn.fill({});scene.fill({});model.fill({});globals.fill({});
    game_access::verifiedClient=local.Base();hvh_input::worldEpoch=1;
    local.Health(100);local.Team(2);
    local.Set(local.page,2*0x70,Ptr(pawn));local.Set(local.page,2*0x70+0x10,handle);
    Put(Ptr(pawn)+f::C_BaseEntity::m_iHealth,100);
    Put(Ptr(pawn)+f::C_BaseEntity::m_lifeState,std::uint8_t{});
    Put(Ptr(pawn)+f::C_BaseEntity::m_iTeamNum,std::uint8_t{3});
    Put(Ptr(pawn)+f::C_BaseEntity::m_pGameSceneNode,Ptr(scene));
    modelBinding=Ptr(model);Put(Ptr(scene)+state+0x20,Ptr(modelBinding));
    Put(Ptr(model)+0x178,128);Put(Ptr(scene)+state+0x80,Ptr(bones));
    Put(Ptr(scene)+state+0x94,std::uint16_t{128});
    globalPointer=Ptr(globals);addresses::globals::global_vars=Ptr(globalPointer);
    combat_access::Target target;target.pawn=Ptr(pawn);target.handle=handle;target.health=100;target.team=3;
    target.hitboxes.count=1;target.hitboxes.entries[0]={0,6,{-2,-2,-2},{2,2,2},3,0,false,1};targets={target};
    Move(10,100);maxUnlag=.2f;playerUnlag=0;failCreate=failPose=changeCountDuringGeometry=false;creates=destroys=draws=materials=0;
    geometryReads=boneCopies=fullHistoryReads=0;
    namespace ch=visuals::backtrack_chams;
    ch::api={local.Base(),10,20,reinterpret_cast<Address>(&Create),reinterpret_cast<Address>(&Destroy),reinterpret_cast<Address>(&WorldGroup)};
    local.Set(local.globals,backtrack_bindings::worldStorage,Address{123});
    ch::ready=true;ch::config.enabled=true;ch::config.visible.enabled=true;ch::config.hidden.enabled=false;
}
}
namespace memory {
bool copy(void* out,std::uintptr_t address,std::size_t size) noexcept{
    if(address==fixture::Ptr(fixture::bones)&&size)++fixture::boneCopies;
    return game_access::Copy(address,out,size);
}
bool store(std::uintptr_t address,const void* in,std::size_t size) noexcept{
    if(fixture::failPose&&address==fixture::Ptr(fixture::renderBones))return false;
    return game_access::Store(address,in,size);
}
}
namespace math {
quaternion::quaternion() noexcept:x(0),y(0),z(0),w(1){}
constexpr quaternion::quaternion(float a,float b,float c,float d) noexcept:x(a),y(b),z(c),w(d){}
vector3 vector3::operator+(const vector3& v)const noexcept{return {x+v.x,y+v.y,z+v.z};}
vector3 vector3::operator-(const vector3& v)const noexcept{return {x-v.x,y-v.y,z-v.z};}
vector3 vector3::operator*(float s)const noexcept{return {x*s,y*s,z*s};}
float vector3::dot(const vector3& v)const noexcept{return x*v.x+y*v.y+z*v.z;}
float vector3::length_sqr()const noexcept{return dot(*this);}
vector3 quaternion::rotate_vector(const vector3& v)const noexcept{
    return {(1-2*(y*y+z*z))*v.x+2*(x*y-w*z)*v.y+2*(x*z+w*y)*v.z,
        2*(x*y+w*z)*v.x+(1-2*(x*x+z*z))*v.y+2*(y*z-w*x)*v.z,
        2*(x*z-w*y)*v.x+2*(y*z+w*x)*v.y+(1-2*(x*x+y*y))*v.z};
}
}
namespace systems {
std::uint32_t schemas::lookup(const char*,std::uint32_t name){
    using namespace fixture;
    switch(name){
    case "m_pGameSceneNode"_hash:return f::C_BaseEntity::m_pGameSceneNode;
    case "m_iHealth"_hash:return f::C_BaseEntity::m_iHealth;
    case "m_lifeState"_hash:return f::C_BaseEntity::m_lifeState;
    case "m_iTeamNum"_hash:return f::C_BaseEntity::m_iTeamNum;
    case "m_flSimulationTime"_hash:return simulation;
    case "m_vecAbsOrigin"_hash:return origin;
    case "m_modelState"_hash:return state;
    case "m_hModel"_hash:return 0x20;
    case "m_nodeToWorld"_hash:return transform;
    case "m_bGunGameImmunity"_hash:return immunity;
    case "m_bDormant"_hash:return dormant;
    case "m_worldGroupId"_hash:return groupId;
    default:return 0;
    }
}
std::array<bones::data,27> bones::get_skeleton(std::uintptr_t,bool){return {};}
}
namespace combat_access {
float Convar(const char* name,float fallback){
    return std::strcmp(name,"sv_maxunlag")==0?fixture::maxUnlag:
        std::strcmp(name,"sv_maxunlag_player")==0?fixture::playerUnlag:fallback;
}
bool Finite(const Vec3& p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
std::vector<Target> ReadTargets(const Context&,TargetScan*,bool){return fixture::targets;}
std::vector<Target> ReadHistoryTargets(const game_access::LocalPlayer&,bool geometry){
    fixture::fullHistoryReads+=geometry?1u:0u;
    auto targets=fixture::targets;
    if(!geometry)for(auto& target:targets)target.hitboxes={};
    return targets;
}
Hitboxes ReadHitboxes(Address scene,bool){
    Check(scene==fixture::Ptr(fixture::scene),"History geometry used a stale scene");
    ++fixture::geometryReads;
    if(fixture::changeCountDuringGeometry) {
        fixture::changeCountDuringGeometry=false;
        fixture::Put(fixture::modelBinding+0x178,64);
    }
    return fixture::targets.front().hitboxes;
}
}
namespace game {bool module_matches(const char*,std::string_view){return false;}}
namespace visuals::chams {
bool Apply(const Configuration&,std::uintptr_t self,std::uintptr_t scene,std::uintptr_t view,std::uintptr_t output,GenerateFn original){original(self,scene,view,output);return true;}
}
namespace visuals::chams::materials {void Ensure(Material,bool){++fixture::materials;}}

void HistoryAndGeometry(){
    using namespace fixture;
    Setup();backtrack::Frame(true);
    auto records=backtrack::Records(Ptr(pawn),handle);Check(records.size()==1,"Initial pose was not captured");
    const auto first=records.front();Check(first->bones.size()==128&&first->points.size()==1,"Full model pose/head point missing");
    backtrack::Frame(true);Check(backtrack::Records(Ptr(pawn),handle).size()==1,"Duplicate simulation time created records");
    Move(10.015625f,105);backtrack::Frame(true);
    records=backtrack::Records(Ptr(pawn),handle);Check(records.size()==2&&records.back()==first,"Moving player history missing");
    const auto hit=backtrack::Intersection(*first,{0,200,64},{100,200,64});
    Check(hit&&hit->x<100&&hit->x>90,"Recorded native hitboxes not intersected");
    Check(!backtrack::Intersection(*first,{0,400,64},{100,400,64}),"Missing historical hit accepted");
    const auto timing=backtrack::Timing(*first);Check(timing&&timing->render.tick==641&&timing->targetIndex==2,"Target render timing wrong");
    const auto reject=[&](auto change,auto restore,const char* reason){change();Check(!backtrack::Current(*first),reason);restore();};
    reject([&]{Put(Ptr(pawn)+f::C_BaseEntity::m_iHealth,0);},[&]{Put(Ptr(pawn)+f::C_BaseEntity::m_iHealth,100);},"Dead record accepted");
    reject([&]{Put(Ptr(pawn)+immunity,true);},[&]{Put(Ptr(pawn)+immunity,false);},"Immune record accepted");
    reject([&]{Put(Ptr(scene)+dormant,true);},[&]{Put(Ptr(scene)+dormant,false);},"Dormant record accepted");
    reject([&]{Put(Ptr(pawn)+f::C_BaseEntity::m_iTeamNum,std::uint8_t{2});},[&]{Put(Ptr(pawn)+f::C_BaseEntity::m_iTeamNum,std::uint8_t{3});},"Friendly record accepted");
    reject([&]{++modelBinding;},[&]{--modelBinding;},"Changed model resource accepted");
    reject([&]{local.Set(local.page,2*0x70+0x10,handle+0x8000);},[&]{local.Set(local.page,2*0x70+0x10,handle);},"Reused pawn handle accepted");
    Put(Ptr(globals)+0x30,10.2f);Check(!backtrack::Current(*first)&&backtrack::Records(Ptr(pawn),handle).empty(),"Expired records survived");
    Setup();backtrack::Frame(true);const auto old=backtrack::Oldest().front();backtrack::Set({false,100});backtrack::Set({true,100});
    Check(!backtrack::Current(*old),"Disable/re-enable revived a discarded snapshot");
    Setup();backtrack::Frame(true);Move(10.015625f,300);backtrack::Frame(true);
    Check(backtrack::Records(Ptr(pawn),handle).size()==1,"Teleport retained pre-teleport history");
    Move(9,300);backtrack::Frame(true);Check(backtrack::Records(Ptr(pawn),handle).size()==1,"Time reset retained old history");
    Setup();backtrack::Set({true,200});
    for(int i=0;i<30;++i){Move(10.f+static_cast<float>(i)*.001f,100.f+static_cast<float>(i));backtrack::Frame(true);}
    Check(backtrack::Records(Ptr(pawn),handle).size()<=16,"History memory grew beyond cap");
    playerUnlag=.01f;backtrack::nextLimitsCheck=0;backtrack::Frame(true);
    Check(backtrack::Records(Ptr(pawn),handle).size()<16,"Per-player server window ignored");
    maxUnlag=0;backtrack::nextLimitsCheck=0;backtrack::Frame(true);
    Check(backtrack::Records(Ptr(pawn),handle).empty(),"Unknown/disabled server history accepted");
    Setup();Put(Ptr(scene)+state+0x94,std::uint16_t{127});backtrack::Frame(true);
    Check(backtrack::Oldest().empty(),"Incomplete model pose accepted");
    Setup();bones[0].position.x=std::numeric_limits<float>::quiet_NaN();backtrack::Frame(true);
    Check(backtrack::Oldest().empty(),"Nonfinite bone accepted");
    Setup();backtrack::Frame(true);auto saved=backtrack::Oldest().front();hvh_input::LevelChanged();
    Check(!backtrack::Current(*saved),"Map transition retained usable history");
    backtrack::Frame(false);Check(backtrack::Oldest().empty(),"Leaving map retained records");
}
void RenderDuplicateWork(){
    using namespace fixture;
    Setup();backtrack::Frame(true);
    const auto first=backtrack::Records(Ptr(pawn),handle).front();
    for(unsigned frame=1;frame<=240;++frame){
        Put(Ptr(globals)+0x30,10.f+static_cast<float>(frame)*.0001f);
        backtrack::Frame(true);
    }
    auto records=backtrack::Records(Ptr(pawn),handle);
    Check(records.size()==1&&records.front()==first,"Render repeats erased or replaced valid duplicate history");
    Check(geometryReads==1&&boneCopies==1&&fullHistoryReads==0,
        "Duplicate render frames reread hitbox metadata or copied a model pose");
    Move(10.03125f,105);backtrack::Frame(true);
    Check(geometryReads==2&&boneCopies==2&&backtrack::Records(Ptr(pawn),handle).size()==2,
        "A new simulation state failed to capture its fresh pose");
    // Resource addresses and bone counts can change without simulation time.
    const auto newModel=Ptr(model)+0x800;
    Put(newModel+0x178,128);modelBinding=newModel;backtrack::Frame(true);
    records=backtrack::Records(Ptr(pawn),handle);
    Check(geometryReads==3&&boneCopies==3&&records.size()==1&&records.front()->model==newModel,
        "Same-time model replacement reused an incompatible history pose");
    Put(newModel+0x178,64);backtrack::Frame(true);
    records=backtrack::Records(Ptr(pawn),handle);
    Check(geometryReads==4&&boneCopies==4&&records.size()==1&&records.front()->bones.size()==64,
        "Same-time bone-count change was treated as a duplicate");
    Move(10.03125f,300);backtrack::Frame(true);
    Check(geometryReads==5&&boneCopies==5&&backtrack::Records(Ptr(pawn),handle).size()==1,
        "Same-time teleport retained the previous pose");
    Move(9,300);backtrack::Frame(true);
    Check(geometryReads==6&&boneCopies==6&&backtrack::Records(Ptr(pawn),handle).size()==1,
        "Clock rewind did not discard and recapture history");
    const auto beforeEpoch=backtrack::Oldest().front();hvh_input::LevelChanged();backtrack::Frame(true);
    Check(geometryReads==7&&boneCopies==7&&!backtrack::Current(*beforeEpoch)&&backtrack::Records(Ptr(pawn),handle).size()==1,
        "Session transition reused a previous-generation pose");
    const auto recycled=handle+0x8000;
    local.Set(local.page,2*0x70+0x10,recycled);targets.front().handle=recycled;backtrack::Frame(true);
    Check(geometryReads==8&&boneCopies==8&&backtrack::Records(Ptr(pawn),recycled).size()==1&&
        backtrack::Records(Ptr(pawn),handle).empty(),"Recycled pawn handle was treated as a duplicate");
    Put(Ptr(scene)+dormant,true);backtrack::Frame(true);
    Check(backtrack::Oldest().empty()&&geometryReads==8&&boneCopies==8,
        "Invalid duplicate identity retained history or reached expensive capture");
    Put(Ptr(scene)+dormant,false);backtrack::Frame(true);
    Check(geometryReads==9&&boneCopies==9&&backtrack::Records(Ptr(pawn),recycled).size()==1,
        "Recovered eligible target did not capture again");
    Put(Ptr(globals)+0x30,9.2f);backtrack::Frame(true);
    Check(backtrack::Oldest().empty()&&geometryReads==9&&boneCopies==9,
        "Expired repeated simulation state survived pruning or was recopied");
    std::cout<<"PASS: 240 repeated render frames reused one geometry read and one full pose copy; simulation/model/bone-count/teleport/clock/session/handle changes captured afresh\n";
}
void CaptureSourceChange(){
    using namespace fixture;
    Setup();changeCountDuringGeometry=true;backtrack::Frame(true);
    Check(backtrack::Oldest().empty()&&geometryReads==1&&boneCopies==1,
        "A model bone-count change during geometry capture published a mixed source pose");
    backtrack::Frame(true);const auto records=backtrack::Records(Ptr(pawn),handle);
    Check(records.size()==1&&records.front()->bones.size()==64&&geometryReads==2&&boneCopies==2,
        "History did not recover with a coherent pose after a mid-capture model change");
}
void HistoricalModels(){
    using namespace fixture;namespace ch=visuals::backtrack_chams;
    Setup();backtrack::Frame(true);Move(10.015625f,105);backtrack::Frame(true);ch::Frame(true);
    Check(creates==1&&ch::ghosts.size()==1,"Native ghost not created");
    Check(renderBones[6][3]==100&&renderBones[6][7]==200&&renderBones[6][11]==64,"Ghost used live bones instead of saved pose");
    const auto world=memory::read<std::array<float,12>>(Ptr(ghostScene)+chams_bindings::kSceneTransform);
    Check(world[0]==1&&world[5]==1&&world[10]==1&&world[3]==100&&world[7]==200&&world[11]==30,"Scene transform is not a full historical matrix");
    Check(ch::Generate(1,Ptr(ghostScene),2,3,Draw)&&draws==1,"Ghost material pass missing");
    Check(!ch::Generate(1,Ptr(ghostScene)+1,2,3,Draw)&&draws==1,"Ghost renderer consumed another model");
    ch::Frame(true);Check(creates==1,"Unchanged ghost was recreated every frame");
    ch::config.enabled=false;Check(ch::Generate(1,Ptr(ghostScene),2,3,Draw)&&draws==1,"Disabled ghost fell through to original drawing");
    ch::Frame(true);Check(destroys==1&&ch::ghosts.empty(),"Disabled ghost leaked");
    Setup();backtrack::Frame(true);Move(10.015625f,105);backtrack::Frame(true);ch::Frame(true);
    Put(Ptr(globals)+0x30,10.2f);Check(ch::Generate(1,Ptr(ghostScene),2,3,Draw)&&draws==0,"Expired ghost rendered");
    ch::Frame(true);Check(destroys==1&&ch::ghosts.empty(),"Expired ghost not removed");
    Setup();backtrack::Frame(true);Move(10.015625f,105);backtrack::Frame(true);failPose=true;ch::Frame(true);
    Check(creates==1&&destroys==1&&ch::ghosts.empty(),"Failed pose upload retained a malformed ghost");
    Setup();backtrack::Frame(true);Move(10.015625f,105);backtrack::Frame(true);ch::Frame(true);ch::Reset();
    Check(destroys==1&&ch::ghosts.empty(),"Level reset leaked a scene");ch::Shutdown();
}
int main(){
    try{HistoryAndGeometry();RenderDuplicateWork();CaptureSourceChange();HistoricalModels();std::cout<<"PASS: backtrack capture, native hitboxes, expiry, identity/model/map changes, bounded history and native ghost lifecycle\n";return 0;}
    catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
