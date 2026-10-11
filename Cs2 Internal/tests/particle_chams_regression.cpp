#include "features/visuals/engine.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace fixture {
ULONGLONG now=1000;
ULONGLONG Clock(){return now;}
bool inWorld=true,failPoint{},failDestroy{};
}
#define GetTickCount64 fixture::Clock
// Include the real planner and backend so native callback injection exercises
// actual effect ownership, CP0 updates, cancellation and frame-stage ordering.
#include "features/visuals/particle_chams.cpp"
#include "features/visuals/effects/particles.cpp"
#undef GetTickCount64

namespace fixture {
void Check(bool ok,const char *message){if(!ok)throw std::runtime_error(message);}
template<class T> std::uintptr_t Address(T &value){return reinterpret_cast<std::uintptr_t>(&value);}
std::uintptr_t Address(std::vector<std::byte> &value){return reinterpret_cast<std::uintptr_t>(value.data());}
namespace field=game_access::field;
constexpr unsigned dormantOffset=0x800,originOffset=0x810;
struct Pawn {
    std::vector<std::byte> pawn=std::vector<std::byte>(0x3000),scene=std::vector<std::byte>(0x3000),
        controller=std::vector<std::byte>(0x3000),model=std::vector<std::byte>(0x3000);
    std::uintptr_t binding{};
    std::uint32_t handle{};
    std::array<systems::bones::data,27> bones{};
    void Initialize(std::uint32_t value,unsigned team,float x) {
        handle=value;binding=Address(model);
        game_access::Write(Address(pawn)+field::C_BaseEntity::m_pGameSceneNode,Address(scene));
        game_access::Write(Address(pawn)+field::C_BaseEntity::m_iHealth,100);
        game_access::Write(Address(pawn)+field::C_BaseEntity::m_lifeState,std::uint8_t{});
        game_access::Write(Address(pawn)+field::C_BaseEntity::m_iTeamNum,std::uint8_t(team));
        game_access::Write(Address(controller)+field::CCSPlayerController::m_hPlayerPawn,handle);
        game_access::Write(Address(scene)+dormantOffset,false);
        game_access::Write(Address(scene)+originOffset,math::vector3{x,10,20});
        game_access::Write(Address(scene)+field::CSkeletonInstance::m_modelState+field::CModelState::m_hModel,Address(binding));
        static const char name[]="models/custom/agents/test/test.vmdl";
        game_access::Write(Address(model)+skin_bindings::kModelNameOffset,reinterpret_cast<std::uintptr_t>(name));
        for(auto &bone:bones)bone.scale=1;
        bones[cstypes::bone_ids::head].position={x,10,84};
        bones[cstypes::bone_ids::spine_4].position={x,10,67};
        bones[cstypes::bone_ids::pelvis].position={x,10,51};
    }
};
std::uintptr_t clientBase{};
std::array<std::byte,0x20> list{};
std::array<std::byte,0x70*512> page{};
std::array<std::byte,0x100> rules{};
std::uintptr_t manager=0x123456,rulesPointer{};
std::array<Pawn,10> players;
custom::configuration config;
struct NativeEffect{std::string resource;math::vector3 point;};
std::unordered_map<unsigned,NativeEffect> live;
unsigned nextId=1,creates{},moves{},destroys{};
void Entry(unsigned index,std::uintptr_t entity,std::uint32_t handle) {
    game_access::Write(Address(page)+index*0x70,entity);
    game_access::Write(Address(page)+index*0x70+0x10,handle);
}
int *__fastcall Create(std::uintptr_t owner,std::uint32_t *index,const char *path,int mode,
                        std::uintptr_t a,std::uintptr_t b,std::uintptr_t c,int d) {
    Check(owner==manager&&mode==8&&!a&&!b&&!c&&!d,"Native create ABI changed");
    *index=nextId++;live.emplace(*index,NativeEffect{path,{}});++creates;return reinterpret_cast<int *>(index);
}
bool __fastcall Point(std::uintptr_t owner,std::uint32_t index,int cp,const math::vector3 *point,float time) {
    Check(owner==manager&&cp==0&&time==0&&point,"Aura used an unsupported control point");
    if(failPoint){failPoint=false;return false;}
    Check(live.contains(index),"Updating unowned effect");live[index].point=*point;++moves;return true;
}
void __fastcall Destroy(std::uintptr_t owner,std::uint32_t index,bool immediate,bool children) {
    Check(owner==manager&&immediate&&children,"Native destroy ABI changed");
    if(failDestroy)RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);
    Check(live.erase(index)==1,"Destroying unowned effect");++destroys;
}
bool __fastcall InitPath(custom::particles::path_buffer *path,const char *name) {
    Check(std::strlen(name)<sizeof(path->storage.inline_data),"Path overflow");
    std::memcpy(path->storage.inline_data,name,std::strlen(name)+1);return true;
}
std::uintptr_t __fastcall Load(std::uintptr_t owner,custom::particles::path_buffer *path,const char *empty) {
    Check(owner==0x9876&&path->resource_type==0x66637076&&empty&&!*empty,"Native resource load ABI changed");return 1;
}
void Frame(){custom::particles::on_frame_stage(7);now+=16;}
void Initialize() {
    const auto clientSize=std::max(game_access::offset::dwLocalPlayerController,game_access::offset::dwEntityList)+0x1000;
    clientBase=reinterpret_cast<std::uintptr_t>(VirtualAlloc(nullptr,clientSize,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    Check(clientBase!=0,"Client fixture allocation failed");
    game_access::Write(Address(list)+0x10,Address(page));
    game_access::Write(clientBase+game_access::offset::dwEntityList,Address(list));
    game_access::Write(clientBase+game_access::offset::dwLocalPlayerController,Address(players[0].controller));
    for(unsigned i=0;i<players.size();++i) {
        players[i].Initialize((1u<<15)|(100+i),i==0||i==2?2:3,100.f+i*100);
        Entry(i+1,Address(players[i].controller),(1u<<15)|(i+1));
        Entry(100+i,Address(players[i].pawn),players[i].handle);
    }
    rulesPointer=Address(rules);game_access::Write(Address(rules)+0x40,1.f);
    game_access::verifiedClient=clientBase;
    addresses::globals::entity_list=clientBase+game_access::offset::dwEntityList;
    auto &unused=players[0];(void)unused;
    namespace p=custom::particles;
    p::manager_slot=Address(manager);p::rules_slot=Address(rulesPointer);p::resource_system=0x9876;
    p::create_effect=&Create;p::set_point=&Point;p::destroy_effect=&Destroy;p::init_path=&InitPath;p::load_resource=&Load;
    p::ready=true;p::stop_requested=false;
    for(const auto *resource:visuals::particle_chams::resources)p::installed_particles.insert(resource);
    p::installed_particles.insert("particles/ambient_fx/env_sparks_omni.vpcf");
}
}

namespace memory {
bool copy(void *output,std::uintptr_t address,std::size_t length) noexcept{return game_access::Copy(address,output,length);}
bool store(std::uintptr_t address,const void *value,std::size_t length) noexcept{return game_access::Store(address,value,length);}
std::string read_string(std::uintptr_t address,std::size_t limit){std::string result;for(size_t i=0;i<limit;++i){char c{};if(!copy(&c,address+i,1)||!c)break;result+=c;}return result;}
std::uintptr_t resolve_cached(std::string_view){return 0;}
}
namespace systems {
std::uint32_t schemas::lookup(const char *,std::uint32_t name) {
    using namespace game_access::field;
    switch(name) {
    case "m_pGameSceneNode"_hash:return C_BaseEntity::m_pGameSceneNode;
    case "m_iHealth"_hash:return C_BaseEntity::m_iHealth;
    case "m_lifeState"_hash:return C_BaseEntity::m_lifeState;
    case "m_iTeamNum"_hash:return C_BaseEntity::m_iTeamNum;
    case "m_hPlayerPawn"_hash:return CCSPlayerController::m_hPlayerPawn;
    case "m_bDormant"_hash:return fixture::dormantOffset;
    case "m_vecAbsOrigin"_hash:return fixture::originOffset;
    case "m_fRoundStartTime"_hash:return 0x40;
    default:return 0;
    }
}
std::array<bones::data,27> bones::get_skeleton(std::uintptr_t pawn) {
    for(auto &player:fixture::players)if(pawn==fixture::Address(player.pawn))return player.bones;
    return {};
}
std::uintptr_t entities::lookup(std::uint32_t handle) const{return game_access::Resolve(fixture::Address(fixture::list),handle);}
}
namespace game {
bool in_world() noexcept{return fixture::inWorld;}
bool module_matches(const char *,std::string_view){return true;}
}
namespace custom {
configuration get_configuration(){return fixture::config;}
}
namespace custom::weapon_fx {
bool attachment_position(particles::attachment,std::uint32_t,std::uint32_t,math::vector3 &){return false;}
}
math::vector3 math::vector3::operator+(const vector3 &v) const noexcept{return {x+v.x,y+v.y,z+v.z};}
math::quaternion::quaternion() noexcept:x{},y{},z{},w{1}{}

int main(){try{
    using namespace fixture;
    namespace aura=visuals::particle_chams;namespace p=custom::particles;
    Initialize();
    aura::Settings settings;settings.enabled=true;settings.players=1;settings.interval=1.5f;aura::Save(settings);
    Frame();Check(live.size()==3,"Aura did not activate independently of world-effects master");
    Check(aura::states.size()==1&&aura::states.contains(players[1].handle),"Enemy-only selection chose wrong nearest pawn");
    const auto first=live.begin()->second.point;const auto beforeMoves=moves;
    Frame();Check(creates==3&&moves>=beforeMoves+3,"Active CP0 did not follow skeleton each frame");
    Check(live.begin()->second.point.x!=first.x||live.begin()->second.point.y!=first.y,"Orbit did not move emitter");
    for(auto &bone:players[1].bones)bone.position.x+=20;
    Frame();Check(live.size()==3,"Movement replaced stable owned effects");
    settings.enemies=false;settings.teammates=true;settings.local=true;settings.players=2;aura::Save(settings);
    Frame();Check(live.size()==6&&aura::states.contains(players[0].handle)&&aura::states.contains(players[2].handle),"Team/local targets did not apply");
    Check(destroys==3,"Target change leaked old enemy aura");
    game_access::Write(Address(players[2].pawn)+field::C_BaseEntity::m_iHealth,0);
    Frame();Check(live.size()==3,"Death did not destroy all pawn emitters");
    players[0].binding=Address(players[2].model);
    const auto beforeModel=destroys;
    failDestroy=true;Frame();Check(live.size()==3,"Failed model-change cleanup forgot old native handles");
    failDestroy=false;Frame();Check(destroys==beforeModel+3&&live.size()==3,"Cancelled effects revived after failed model-change destruction");
    const auto stale=players[0].handle;
    Entry(100,Address(players[0].pawn),(2u<<15)|100);
    Frame();Check(live.empty(),"Recycled entity serial retained particles on stale pawn");
    Entry(100,Address(players[0].pawn),stale);Frame();Check(live.size()==3,"Valid restored pawn did not recover");
    game_access::Write(Address(players[0].scene)+dormantOffset,true);
    Frame();Check(live.empty(),"Dormant pawn retained effects");
    game_access::Write(Address(players[0].scene)+dormantOffset,false);
    Frame();Check(live.size()==3,"Awakened pawn did not recover");
    const auto roundDestroyed=destroys;game_access::Write(Address(rules)+0x40,2.f);
    Frame();Check(destroys==roundDestroyed+3&&live.size()==3,"Round change did not clean previous aura");
    settings.enemies=true;settings.teammates=true;settings.local=true;settings.players=99;
    settings.radius=std::numeric_limits<float>::quiet_NaN();aura::Save(settings);
    Check(aura::Get().players==6&&aura::Get().radius==9,"Unbounded/NaN UI settings escaped validation");
    for(unsigned i=0;i<4;++i)Frame();Check(live.size()<=18&&aura::states.size()<=6,"Aura exceeded bounded pawn/emitter count");
    settings=aura::Get();settings.enabled=false;aura::Save(settings);
    failDestroy=true;Frame();Check(!live.empty()&&p::active_count()!=0,"Failed native destruction forgot owned handles");
    failDestroy=false;Frame();Check(live.empty()&&p::active_count()==0,"Native cleanup did not retry successfully");
    settings.enabled=true;settings.players=1;settings.local=true;settings.enemies=false;settings.teammates=false;settings.style=3;aura::Save(settings);
    p::installed_particles.erase(aura::resources[3]);Frame();Check(live.empty(),"Missing resource reported a spawned aura");
    p::installed_particles.insert(aura::resources[3]);now+=2000;Frame();Check(live.size()==3,"Unavailable particle resource did not recover");
    const auto head=players[0].bones[cstypes::bone_ids::head];
    players[0].bones[cstypes::bone_ids::head].position.x=std::numeric_limits<float>::quiet_NaN();
    Frame();Check(live.size()==2,"Invalid native head anchor retained/spawned an emitter");
    players[0].bones[cstypes::bone_ids::head]=head;Frame();Check(live.size()==3,"Restored head anchor did not recover");
    for(auto &bone:players[0].bones)bone.scale=0;
    Frame();Check(live.empty()&&aura::states.empty(),"Missing skeleton anchors were replaced with fabricated points");
    for(auto &bone:players[0].bones)bone.scale=1;
    Frame();Check(live.size()==3,"Valid skeleton did not recover");
    for(unsigned i=0;i<4;++i)Check(p::transients[i].handle.index==p::invalid_effect,"Aura consumed reserved kill-particle slots");
    settings.enabled=true;settings.players=1;settings.local=true;settings.enemies=false;settings.teammates=false;aura::Save(settings);
    aura::Reset();
    failPoint=true;Frame();Check(live.size()<=2,"Failed CP0 setup leaked a native effect");
    now+=2000;Frame();Check(live.size()==3,"Failed creation did not retry");
    p::on_level_shutdown();Check(live.empty()&&aura::states.empty(),"Level shutdown did not clean effects/state");
    Frame();p::request_shutdown();Frame();Check(live.empty()&&p::shutdown_complete(),"Shutdown did not drain aura native handles");
    std::cout<<"PASS: actual native particle backend + body selection, skeleton motion/orbit, bounds, death/model/serial/round cleanup and SEH retry\n";
    return 0;
}catch(const std::exception &error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}}
