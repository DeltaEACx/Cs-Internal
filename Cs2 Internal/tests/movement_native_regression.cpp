// Compile the actual adapter to inject callable native bindings without a test API.
#include "features/movement/context.cpp"
namespace fixture_input {
HWND Window();
DWORD WindowProcess(HWND,DWORD*);
SHORT Key(int);
void SetKey(int,bool);
void SetFocus(bool);
}
#define GetForegroundWindow fixture_input::Window
#define GetWindowThreadProcessId fixture_input::WindowProcess
#define GetAsyncKeyState fixture_input::Key
#include "features/movement/movement.cpp"
#undef GetForegroundWindow
#undef GetWindowThreadProcessId
#undef GetAsyncKeyState
#include "player_fixture.h"
#include <iostream>
#include <unordered_map>
namespace {
using Address=std::uintptr_t;using Vec3=combat_access::Vec3;using namespace combat_bindings;
struct Field {const char *owner,*name;unsigned offset;};
constexpr Field fields[]{
 {"C_BaseEntity","m_fFlags",0x3f4},{"C_BaseEntity","m_nActualMoveType",0x526},
 {"C_BaseEntity","m_vecVelocity",0x430},{"C_BaseEntity","m_flGravityScale",0x540},
 {"C_BaseEntity","m_pGameSceneNode",0x330},{"C_BaseEntity","m_pCollision",0x340},
 {"C_BasePlayerPawn","m_pMovementServices",0x1330},{"CGameSceneNode","m_vecAbsOrigin",0xc8},
 {"CCollisionProperty","m_vecMins",0x40},{"CCollisionProperty","m_vecMaxs",0x4c},
 {"CPlayer_MovementServices","m_flCmdForwardMove",0x1a0},{"CPlayer_MovementServices","m_flCmdLeftMove",0x1a4},
 {"CPlayer_MovementServices","m_flMaxspeed",0x1ac},{"CCSPlayer_MovementServices","m_flStamina",0x694},
 {"CCSPlayer_MovementServices","m_flDuckAmount",0x40c},{"CCSPlayer_MovementServices","m_flDuckSpeed",0x410},
 {"CPlayer_MovementServices_Humanoid","m_flSurfaceFriction",0x26c},
 {"C_BasePlayerPawn","m_pWeaponServices",0x1340},{"CPlayer_WeaponServices","m_hActiveWeapon",0x50},
 {"C_BaseEntity","m_nSubclassID",0x80},{"CCSWeaponBaseVData","m_WeaponType",0x20},
 {"C_BasePlayerWeapon","m_nNextPrimaryAttackTick",0x100},{"C_BasePlayerWeapon","m_iClip1",0x108},
 {"C_CSWeaponBase","m_bInReload",0x10c},{"CBasePlayerController","m_nTickBase",0x700}
};
struct Raw {std::array<std::byte,16> header{};SubtickPayload payload{};};
static_assert(sizeof(Raw)==0x38);
struct PointerStorage {int allocated{},padding{};std::array<Address,128> entries{};};
struct Fixture:PlayerFixture {
 std::vector<unsigned char> services=std::vector<unsigned char>(0x800),scene=std::vector<unsigned char>(0x300),collision=std::vector<unsigned char>(0x100);
 std::vector<unsigned char> weaponServices=std::vector<unsigned char>(0x100),weapon=std::vector<unsigned char>(0x200),weaponData=std::vector<unsigned char>(0x100);
 std::array<std::byte,0x100> command{},rawBase{};Raw buttonMessage{},angleMessage{};
 std::array<Raw,128> messages{};PointerStorage initial{},grown{};
 Address manager{0xbeef};int sequence{77};bool pending{},failView{},missingCvar{},missingField{},quantized{};
 unsigned filterCalls{},rayCalls{},allocateCalls{},appendCalls{},cvarCalls{},crcCalls{};int rayMode{};bool failFilter{},failAllocate{},failAppend{},failCrc{};
 unsigned memoryReads{},pointerReads{},schemaReads{},commandReads{},viewReads{};
 combat_access::CommandIdentity identity{};Vec3 view{10,20,0};
 template<class T,class Block>void Put(Block &block,size_t offset,const T &value){Check(offset+sizeof(value)<=block.size(),"fixture overflow");std::memcpy(block.data()+offset,&value,sizeof(value));}
 template<class Block>static Address At(const Block &block){return reinterpret_cast<Address>(block.data());}
 Fixture(){
  identity={At(command),sequence,0x8001,false,false};
  Put(pawn,0x3f4,std::uint32_t{0x11});Put(pawn,0x526,std::uint8_t{2});Put(pawn,0x430,Vec3{200,15,-50});Put(pawn,0x540,1.f);
  Put(pawn,0x330,At(scene));Put(pawn,0x340,At(collision));Put(pawn,0x1330,At(services));Put(pawn,0xd50,std::uint64_t{0xc3011});
  Put(scene,0xc8,Vec3{12,20,40});Put(collision,0x40,Vec3{-16,-16,0});Put(collision,0x4c,Vec3{16,16,72});
  Put(services,0x38,Pawn());Put(services,0x1a0,.2f);Put(services,0x1a4,-.1f);Put(services,0x1ac,250.f);
  Put(services,0x694,0.f);Put(services,0x40c,0.f);Put(services,0x410,8.f);Put(services,0x26c,1.f);
  BaseCommand base{};base.bits=0xc0;base.forward=1;base.buttons=reinterpret_cast<Address>(&buttonMessage);base.angles=reinterpret_cast<Address>(&angleMessage);
  Put(rawBase,16,base);CommandProto proto{};proto.bits=1;proto.base=At(rawBase);Put(command,kCommandProto,proto);Put(command,kCommandSequence,sequence);
  ButtonPayload buttons{7,0,8,0,0};std::memcpy(&buttonMessage.payload,&buttons,sizeof(buttons));
  AnglePayload angles{7,0,10,20,0};std::memcpy(&angleMessage.payload,&angles,sizeof(angles));Put(command,kCommandButtons,ButtonState{0,8,0,0});
 }
 BaseCommand Base()const{BaseCommand value{};std::memcpy(&value,rawBase.data()+16,sizeof(value));return value;}
 void Base(BaseCommand value){Put(rawBase,16,value);}
 Address FieldAddress()const{return At(rawBase)+16+offsetof(BaseCommand,subticks);}
 void Equip(){
  Put(pawn,0x1340,At(weaponServices));Put(weaponServices,0x50,std::uint32_t{0x8002});
  Put(page,2*0x70,At(weapon));Put(page,2*0x70+0x10,std::uint32_t{0x8002});Put(weapon,0x88,At(weaponData));
  Put(weaponData,0x20,3);Put(weapon,0x100,101);Put(weapon,0x108,30);Put(weapon,0x10c,false);Put(controller,0x700,100);
 }
};
Fixture *active{};
__declspec(noinline) void __fastcall Filter(void *buffer,Address pawn,std::uint64_t mask,int group){
 Check(pawn==active->Pawn()&&mask==0xc3031&&group==11,"movement filter ABI or current native mask wrong");++active->filterCalls;
 if(active->failFilter)RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);
 std::memset(buffer,0,0x48);*static_cast<Address*>(buffer)=0x1234;
}
__declspec(noinline) bool __fastcall Ray(Address manager,systems::tracing::ray *ray,const Vec3 *start,const Vec3 *end,void *filter,systems::tracing::result *hit){
 Check(manager==active->manager&&ray->type==2&&ray->mins.x==-16&&ray->maxs.z==72&&*static_cast<Address*>(filter)==0x1234,
  "native hull type/filter/six-argument ABI incorrect");++active->rayCalls;
 if(active->rayMode==5)RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);
 const bool clear=active->rayMode==1||(active->rayMode==6&&active->rayCalls%3==0);
 hit->fraction=clear?1.f:.5f;hit->normal=clear?Vec3{}:Vec3{0,0,1};
 hit->start_pos=*start;hit->end_pos={start->x+(end->x-start->x)*hit->fraction,start->y+(end->y-start->y)*hit->fraction,start->z+(end->z-start->z)*hit->fraction};
 hit->position={-900,-900,-900};hit->contact_position_valid=true;
 if(active->rayMode==2)hit->all_solid=true;if(active->rayMode==3)hit->fraction=NAN;if(active->rayMode==4)hit->normal={};
 return !clear;
}
__declspec(noinline) Address __fastcall Allocate(Address arena){
 Check(arena==0x77,"native subtick allocator received wrong arena");++active->allocateCalls;
 if(active->failAllocate)return 0;
 return reinterpret_cast<Address>(&active->messages[8+active->allocateCalls-1]);
}
__declspec(noinline) Address __fastcall Append(RepeatedField *field,Address raw){
 Check(reinterpret_cast<Address>(field)==active->FieldAddress(),"native append received wrong repeated field");++active->appendCalls;
 if(active->failAppend)RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);
 auto *storage=reinterpret_cast<PointerStorage*>(field->entries);
 if(field->capacity==field->size){active->grown=*storage;storage=&active->grown;field->entries=reinterpret_cast<Address>(storage);field->capacity=16;}
 storage->entries[storage->allocated++]=raw;++field->size;return raw;
}
void Bind(Fixture &f){active=&f;movement::native::Reset();movement::native::api={f.PlayerFixture::Base(),reinterpret_cast<Address>(&Ray),reinterpret_cast<Address>(&Filter),
 reinterpret_cast<Address>(&f.manager),reinterpret_cast<Address>(&Allocate),reinterpret_cast<Address>(&Append)};}
void SnapshotAndManualGates(){
 Fixture f;Bind(f);const auto snapshot=movement::native::Read(Fixture::At(f.input),false);
 Check(snapshot.valid&&snapshot.state.traceValid&&snapshot.state.onGround&&snapshot.mask==0xc3031&&snapshot.services==Fixture::At(f.services),"native movement context unavailable");
 Check(snapshot.state.lastImpulses.x==.2f&&snapshot.state.maxSpeed==250&&snapshot.state.origin.z==40&&snapshot.state.velocity.x==200,"native movement snapshot lost source inputs");
 Check(movement::native::Fresh(snapshot),"fresh movement snapshot rejected");
 f.identity.sequence=78;Check(!movement::native::Fresh(snapshot),"stale native sequence accepted");f.identity.sequence=77;
 auto base=f.Base();base.forward=.75f;f.Base(base);Check(!movement::native::Fresh(snapshot),"changed native base accepted");base.forward=1;f.Base(base);
 for(const auto action:{kAttack,1ull<<11,1ull<<5}){
  f.Put(f.command,kCommandButtons,ButtonState{0,8,action,0});Check(!movement::native::Read(Fixture::At(f.input),false).state.shooting,"released action suppressed movement");
  f.Put(f.command,kCommandButtons,ButtonState{0,8|action,action,0});Check(movement::native::Read(Fixture::At(f.input),false).state.shooting,"raw manual action missed");
  f.Put(f.command,kCommandButtons,ButtonState{0,8,0,0});ButtonPayload pb{7,0,8,0,action};std::memcpy(&f.buttonMessage.payload,&pb,sizeof(pb));
  Check(movement::native::Read(Fixture::At(f.input),false).state.shooting,"PB-only manual action missed");pb.scroll=0;std::memcpy(&f.buttonMessage.payload,&pb,sizeof(pb));
  f.messages[0].payload={};f.messages[0].payload.bits=7;f.messages[0].payload.button=action;f.messages[0].payload.pressed=true;f.messages[0].payload.when=.4f;
  f.initial.allocated=1;f.initial.entries[0]=reinterpret_cast<Address>(&f.messages[0]);base.subticks={0x77,1,1,reinterpret_cast<Address>(&f.initial)};f.Base(base);
  const auto step=movement::native::Read(Fixture::At(f.input),false);Check(step.valid&&step.state.shooting&&step.state.maxSubtickWhen==0,"manual pressed subtick missed or incorrectly constrained angle timing");
  f.messages[0].payload.bits|=0x40;
  Check(movement::native::Read(Fixture::At(f.input),false).state.maxSubtickWhen==.4f,"Native angle subtick did not constrain the yaw window");
  base.subticks={};f.Base(base);
 }
 Check(movement::native::Read(Fixture::At(f.input),true).state.shooting,"requested shot was not prioritized");
 f.pending=true;Check(!movement::native::Read(Fixture::At(f.input),false).valid,"weapon selection not paused");f.pending=false;
 f.Put(f.pawn,0x526,std::uint8_t{7});Check(!movement::native::Read(Fixture::At(f.input),false).valid,"noclip movement edited");f.Put(f.pawn,0x526,std::uint8_t{2});
 f.Put(f.pawn,0x3f4,std::uint32_t{0x51});Check(!movement::native::Read(Fixture::At(f.input),false).valid,"frozen player edited");f.Put(f.pawn,0x3f4,std::uint32_t{0x11});
 f.missingCvar=true;movement::native::Reset();const auto missing=movement::native::Read(Fixture::At(f.input),false);Check(missing.valid&&!movement_policy::detail::AirValid(missing.state),"missing air prerequisites became synthetic state");
}
void HeldFireCooldown(){
 Fixture f;Bind(f);f.Equip();f.Put(f.pawn,0x3f4,std::uint32_t{0x10});
 f.Put(f.command,kCommandButtons,ButtonState{0,8|kAttack,0,0});
 Check(!(f.Base().bits&0x20)&&!systems::schemas::lookup("CCSPlayerController",fnv1a::runtime_hash("m_nTickBase")),
  "Cooldown fixture accidentally supplied the absent tick field or the derived-class schema field");
 const auto cooldown=movement::native::Read(Fixture::At(f.input),false);
 Check(cooldown.valid&&!cooldown.state.shooting,"Held primary fire blocked strafe during cooldown without a command tick");
 f.Put(f.weapon,0x100,100);
 Check(movement::native::Read(Fixture::At(f.input),false).state.shooting,"Ready primary shot was not preserved");
 f.Put(f.weapon,0x10c,true);
 Check(!movement::native::Read(Fixture::At(f.input),false).state.shooting,"Reload blocked ordinary movement");f.Put(f.weapon,0x10c,false);
 f.Put(f.weapon,0x108,0);
 Check(!movement::native::Read(Fixture::At(f.input),false).state.shooting,"Empty weapon blocked ordinary movement");f.Put(f.weapon,0x108,30);
 auto base=f.Base();base.bits|=0x20;base.clientTick=99;f.Base(base);
 Check(!movement::native::Read(Fixture::At(f.input),false).state.shooting,"Command tick lost priority to predicted tick base");
 Check(movement::native::Read(Fixture::At(f.input),true).state.shooting,"Committed shot was not preserved during weapon cooldown");
 movement::Reset();movement::settings={};movement::settings.enabled=true;movement::settings.options.airStrafe=true;movement::menuOpen=false;
 base.subticks={0x77,0,128,reinterpret_cast<Address>(&f.initial)};f.initial.allocated=128;
 for(int i=0;i<128;++i)f.initial.entries[i]=reinterpret_cast<Address>(&f.messages[i]);f.Base(base);
 movement::Process(Fixture::At(f.input),false);
 ButtonState buttons{};std::memcpy(&buttons,f.command.data()+kCommandButtons,sizeof(buttons));
 Check(f.crcCalls==1&&f.Base().subticks.size==32&&(buttons.value&kAttack),"Held-fire cooldown did not commit strafe with attack preserved");
 movement::Reset();movement::settings={};movement::menuOpen=true;
}
void FreshManualJumpEdges(){
 Fixture f;Bind(f);f.Put(f.pawn,0x3f4,std::uint32_t{0x10});f.Put(f.pawn,0x430,Vec3{200,15,100});
 const auto raw=[&](ButtonState value){f.Put(f.command,kCommandButtons,value);};
 const auto pb=[&](ButtonPayload value){std::memcpy(&f.buttonMessage.payload,&value,sizeof(value));};
 movement_policy::Options jumps;jumps.bhop=jumps.jumpbug=jumps.edgebug=jumps.edgeJump=jumps.edgeStop=true;
 const auto fresh=[&]{
  const auto context=movement::native::Read(Fixture::At(f.input),false);
  Check(context.valid&&!context.state.onGround&&context.state.jumpPressed&&(context.state.buttons&movement_policy::Jump),
   "Fresh raw/PB/scroll/subtick jump edge was missed after predicted takeoff");
  movement_policy::DirectionState direction;unsigned traces{};
  const auto plan=movement_policy::Build(context.state,jumps,direction,[&](auto,auto,auto,auto)->std::optional<movement_policy::Trace>{++traces;return {};});
  Check(!plan.Changed()&&!traces,"Fresh predicted takeoff jump was suppressed or landing automation ran");
 };
 constexpr auto jump=movement_policy::Jump;
 raw({0,8|jump,jump,0});fresh();
 raw({0,8,0,jump});fresh();
 raw({0,8,0,0});pb({7,0,8|jump,jump,0});fresh();
 pb({7,0,8,0,jump});fresh();
 pb({7,0,8,0,0});f.messages[0].payload={};f.messages[0].payload.bits=7;
 f.messages[0].payload.button=jump;f.messages[0].payload.pressed=true;f.messages[0].payload.when=.25f;
 f.initial.allocated=1;f.initial.entries[0]=reinterpret_cast<Address>(&f.messages[0]);
 auto base=f.Base();base.subticks={0x77,1,1,reinterpret_cast<Address>(&f.initial)};f.Base(base);fresh();
 f.messages[0].payload.pressed=false;
 auto context=movement::native::Read(Fixture::At(f.input),false);
 Check(context.valid&&!context.state.jumpPressed&&!(context.state.buttons&jump),"Released native jump event synthesized held intent");
 base.subticks={};f.Base(base);raw({0,8|jump,0,0});context=movement::native::Read(Fixture::At(f.input),false);
 Check(!context.state.jumpPressed&&(context.state.buttons&jump),"Held jump became a repeated fresh edge");
 movement_policy::Options bhop;bhop.bhop=true;movement_policy::DirectionState direction;
 Check(!movement_policy::Build(context.state,bhop,direction).Changed(),"Native post-prediction held rising hop was cleared");
 for(float velocity:{0.f,-50.f}){
  f.Put(f.pawn,0x430,Vec3{200,15,velocity});context=movement::native::Read(Fixture::At(f.input),false);
  Check(!context.state.jumpPressed&&movement_policy::Build(context.state,bhop,direction).clearButtons==jump,
   "Native held jump did not release at apex/descent");
 }
 context=movement::native::Read(Fixture::At(f.input),false);
 const auto landing=movement_policy::Build(context.state,bhop,direction,[&](auto from,auto to,auto mins,auto maxs){
  return movement::native::TraceHull(context,from,to,mins,maxs);});
 Check(landing.clearButtons==jump&&landing.count==2&&!landing.steps[0].pressed&&landing.steps[1].pressed,
  "Native held descent lost landing release/press scheduling");
 // Raw release is authoritative even if the old protobuf held/changed fields remain.
 raw({0,8,jump,0});pb({7,0,8|jump,jump,0});context=movement::native::Read(Fixture::At(f.input),false);
 Check(context.valid&&!context.state.jumpPressed&&!(context.state.buttons&jump)&&
  !movement_policy::Build(context.state,bhop,direction).Changed(),"Raw jump release was reversed by stale protobuf held value");
 // Absent protobuf presence bits must not turn stale storage into a new jump.
 raw({0,8,0,0});pb({0,0,8|jump,jump,jump});context=movement::native::Read(Fixture::At(f.input),false);
 Check(!context.state.jumpPressed&&!(context.state.buttons&jump),"Absent protobuf jump fields were treated as input");
}
void GeneratedJumpOwnership(){
 Fixture f;Bind(f);f.Put(f.pawn,0x3f4,std::uint32_t{0x10});
 f.Put(f.command,kCommandButtons,ButtonState{0,8|movement_policy::Jump,0,0});
 ButtonPayload pb{7,0,8|movement_policy::Jump,0,0};std::memcpy(&f.buttonMessage.payload,&pb,sizeof(pb));
 auto base=f.Base();base.subticks={0x77,2,2,reinterpret_cast<Address>(&f.initial)};f.initial.allocated=2;
 for(int index=0;index<2;++index){f.initial.entries[index]=reinterpret_cast<Address>(&f.messages[index]);
  f.messages[index].payload={};f.messages[index].payload.bits=7;f.messages[index].payload.button=movement_policy::Jump;
  f.messages[index].payload.pressed=index==1;f.messages[index].payload.when=.3f+.1f*index;}
 f.Base(base);auto context=movement::native::Read(Fixture::At(f.input),false);movement_policy::Options options;options.bhop=true;
 Check(context.state.jumpPressed&&movement::Capture(context,options,false,movement::committed),"cannot capture generated landing pulse fixture");
 movement::committed.ownedCount=2;movement::committed.ownedSteps[0]=f.initial.entries[0];movement::committed.ownedSteps[1]=f.initial.entries[1];
 auto state=movement::PlanningInput(context);
 Check(!state.jumpPressed&&(state.buttons&movement_policy::Jump),"owned landing pulse was mistaken for a new manual press");
 // A real new raw press retains priority even when our landing messages remain.
 f.Put(f.command,kCommandButtons,ButtonState{0,8|movement_policy::Jump,movement_policy::Jump,0});
 context=movement::native::Read(Fixture::At(f.input),false);Check(movement::PlanningInput(context).jumpPressed,"owned pulse hid a fresh raw manual press");
 // Native rebuild reusing a message pointer with new contents loses ownership.
 f.messages[1].payload.when=.75f;context=movement::native::Read(Fixture::At(f.input),false);
 Check(movement::PlanningInput(context).jumpPressed&&!movement::Reusable(context),"recycled native jump message was ignored by pointer alone");
 movement::Clear();
}
void PhysicsSnapshotCache(){
 Fixture f;Bind(f);const auto first=movement::native::Read(Fixture::At(f.input),false);const auto calls=f.cvarCalls;
 Check(first.valid&&calls==11,"physics snapshot did not query all prerequisites");
 for(int i=0;i<20;++i)Check(movement::native::Read(Fixture::At(f.input),false).valid,"cached physics snapshot failed");
 Check(f.cvarCalls==calls,"physics convars were rescanned for each command");
 f.missingCvar=true;movement::native::physics.refreshed=GetTickCount64()-300;
 Check(!movement_policy::detail::AirValid(movement::native::Read(Fixture::At(f.input),false).state)&&f.cvarCalls==calls+11,"expired missing physics snapshot was not retried");
 f.missingCvar=false;movement::native::physics.refreshed=GetTickCount64()-300;
 Check(movement_policy::detail::AirValid(movement::native::Read(Fixture::At(f.input),false).state)&&f.cvarCalls==calls+22,"missing convar remained cached after retry");
 movement::native::Reset();Check(movement::native::Read(Fixture::At(f.input),false).valid&&f.cvarCalls==calls+33,"session reset did not invalidate physics snapshot");
}
void HullABIAndFailure(){
 Fixture f;Bind(f);auto context=movement::native::Read(Fixture::At(f.input),false);
 const movement_policy::Vec3 from{12,20,40},to{12,20,20};
 const auto run=[&]{return movement::native::TraceHull(context,from,to,context.state.mins,context.state.maxs);};
 const auto hit=run();Check(hit&&hit->fraction==.5f&&hit->position.z==30&&hit->normal.z==1,"native hull endpoint/normal layout incorrect");
 f.rayMode=1;const auto clear=run();Check(clear&&clear->fraction==1&&clear->position.z==20,"false native return did not represent clear hull");
 for(int mode:{2,3,4,5}){f.rayMode=mode;Check(!run(),"solid/invalid/exception hull became valid geometry");}
 f.rayMode=0;f.failFilter=true;Check(!run(),"native movement filter exception escaped");f.failFilter=false;
 f.identity.sequence=78;const auto calls=f.rayCalls;Check(!run()&&f.rayCalls==calls,"stale context reached native hull");
}
void RuntimeOwnershipAndMenuLifecycle(){
 Fixture f;Bind(f);auto base=f.Base();base.subticks={0x77,2,2,reinterpret_cast<Address>(&f.initial)};f.initial.allocated=2;
 for(int index=0;index<2;++index){f.initial.entries[index]=reinterpret_cast<Address>(&f.messages[index]);f.messages[index].payload.bits=4|0x18;
  f.messages[index].payload.when=.2f+.2f*index;f.messages[index].payload.forward=.1f;}
 f.Base(base);auto context=movement::native::Read(Fixture::At(f.input),false);movement_policy::Options options;options.airStrafe=true;
 Check(movement::Capture(context,options,false,movement::committed),"runtime committed snapshot capture failed");
 movement::committed.ownedCount=2;movement::committed.ownedSteps[0]=f.initial.entries[0];movement::committed.ownedSteps[1]=f.initial.entries[1];
 Check(movement::Intact(context,options)&&movement::Reusable(context),"intact command/owned window rejected");
 const auto before=f.messages[0].payload;f.messages[0].payload.bits=7;f.messages[0].payload.button=1ull<<13;f.messages[0].payload.pressed=true;
 Check(!movement::Intact(context,options)&&!movement::Reusable(context),"recycled native reload message was treated as movement-owned");
 movement::finishPending=false;movement::FinishCommand(Fixture::At(f.input));
 Check(!movement::Reusable(context),"finish adopted a rebuilt unrelated payload without a movement commit");f.messages[0].payload=before;
 auto angle=*reinterpret_cast<AnglePayload*>(&f.angleMessage.payload);angle.yaw=175;std::memcpy(&f.angleMessage.payload,&angle,sizeof(angle));
 Check(!movement::Intact(context,options),"AA angle change was not detected");movement::finishPending=true;
 movement::FinishCommand(Fixture::At(f.input));context=movement::native::Read(Fixture::At(f.input),false);
 Check(movement::Intact(context,options)&&movement::Reusable(context)&&movement::committed.ownedCount==2,
  "post-AA finish failed to refresh payload while retaining owned window");
 auto changed=options;changed.airStrafe=false;Check(!movement::Intact(context,changed),"changed movement options were suppressed");
 auto rebuilt=f.Base();rebuilt.left=.3f;f.Base(rebuilt);context=movement::native::Read(Fixture::At(f.input),false);
 Check(!movement::Intact(context,options),"native rebuilt base was suppressed by sequence-only dedup");
 movement::SetInputState(true);movement::captureKey=3;for(int frame=0;frame<5;++frame)movement::SetInputState(true);
 Check(movement::captureKey==3,"hold-key capture was canceled on every menu frame");movement::SetInputState(false);
 Check(movement::captureKey==-1&&!movement::committed.valid,"menu transition failed to reset capture/runtime state");
}
void PublicRuntimeAdmissionAndCommit(){
 Fixture f;Bind(f);
 const auto setup=[&](movement_policy::Options options) {
  movement::Reset();movement::settings={};movement::settings.enabled=true;movement::settings.options=options;
  movement::menuOpen=false;
  f.Put(f.command,kCommandButtons,ButtonState{0,8,0,0});
  ButtonPayload buttons{7,0,8,0,0};std::memcpy(&f.buttonMessage.payload,&buttons,sizeof(buttons));
  AnglePayload angles{7,0,10,20,0};std::memcpy(&f.angleMessage.payload,&angles,sizeof(angles));
  auto base=f.Base();base.bits=0xc0;base.forward=1;base.left=0;
  base.subticks={0x77,0,128,reinterpret_cast<Address>(&f.initial)};f.initial.allocated=128;
  for(int i=0;i<128;++i){f.initial.entries[i]=reinterpret_cast<Address>(&f.messages[i]);f.messages[i]={};}
  f.Base(base);f.crcCalls=0;f.failCrc=false;
 };
 movement_policy::Options slow;slow.slowWalk=true;setup(slow);
 movement::Process(Fixture::At(f.input),false);
 Check(std::abs(f.Base().forward-.33f)<.0001f&&f.Base().subticks.size==1&&f.crcCalls==1&&movement::committed.valid,
  "Public enabled/Always slow walk did not commit its base and AG2 window");
 const auto step=f.Base().subticks.entries;const auto firstPayload=f.messages[0].payload;
 movement::Process(Fixture::At(f.input),false);
 Check(f.crcCalls==1&&f.Base().subticks.size==1&&f.Base().subticks.entries==step&&
  !std::memcmp(&f.messages[0].payload,&firstPayload,sizeof(firstPayload)),"Public repeated command applied movement twice");
 movement::FinishCommand(Fixture::At(f.input));
 setup(slow);movement::settings.keys[3]=VK_SHIFT;
 movement::Process(Fixture::At(f.input),false);Check(f.crcCalls==0&&f.Base().forward==1,"Unheld movement key committed input");
 // The deterministic window/key adapter supplies focus and key state below.
 fixture_input::SetKey(VK_SHIFT,true);movement::Process(Fixture::At(f.input),false);
 Check(f.crcCalls==1&&f.Base().forward<1,"Held movement key did not admit public runtime");fixture_input::SetKey(VK_SHIFT,false);
 setup(slow);movement::SetInputState(true);movement::Process(Fixture::At(f.input),false);
 Check(f.crcCalls==0&&f.Base().forward==1&&movement::observation.valid,
  "Menu-open movement committed input or erased the last gameplay diagnostics");movement::SetInputState(false);
 fixture_input::SetFocus(false);movement::Process(Fixture::At(f.input),false);
 Check(f.crcCalls==0&&f.Base().forward==1,"Unfocused movement committed input");fixture_input::SetFocus(true);
 movement::Process(Fixture::At(f.input),true);Check(f.crcCalls==1&&f.Base().forward<1,"Committed shot suppressed ground movement");
 setup(slow);
 f.Put(f.command,kCommandButtons,ButtonState{0,8|kAttack,0,0});movement::Process(Fixture::At(f.input),false);
 ButtonState firing{};std::memcpy(&firing,f.command.data()+kCommandButtons,sizeof(firing));
 Check(f.crcCalls==1&&f.Base().forward<1&&(firing.value&kAttack),"Ground movement erased or was suppressed by primary fire");
 setup(slow);f.failCrc=true;const auto before=f.Base();movement::Process(Fixture::At(f.input),false);
 const auto after=f.Base();Check(f.crcCalls==2&&!std::memcmp(&before,&after,sizeof(before))&&!movement::committed.valid,
  "Public CRC failure left a movement edit committed");
 movement_policy::Options air;air.airStrafe=true;setup(air);f.Put(f.pawn,0x3f4,std::uint32_t{0x10});
 const auto airInput=movement::native::Read(Fixture::At(f.input),false);movement_policy::DirectionState airDirections;
 const auto expected=movement_policy::Build(airInput.state,air,airDirections,[](auto,auto,auto,auto){return std::optional<movement_policy::Trace>{};});
 const auto viewBefore=f.angleMessage;
 movement::Process(Fixture::At(f.input),false);
 Check(f.crcCalls==1&&f.Base().subticks.size==32&&movement::committed.ownedCount==32&&f.Base().forward==0&&f.Base().left==0,
  "Public airborne strafe failed to commit its32-step native window");
 float forward=airInput.state.lastImpulses.x,left=airInput.state.lastImpulses.y;
 for(int i=0;i<32;++i){const auto &step=f.messages[i].payload;Check((step.bits&0x1c)==0x1c&&step.when==i/32.f,"Strafe window timing or presence bits changed");forward+=step.forward;left+=step.left;}
 Check(expected.analog&&std::abs(forward-expected.analog->x)<.00001f&&std::abs(left-expected.analog->y)<.00001f,
  "Native strafe deltas do not reconstruct the planned movement");
 Check(!std::memcmp(&viewBefore,&f.angleMessage,sizeof(viewBefore)),"Ordinary air strafe moved the camera");
 const auto firstStrafe=f.messages[0].payload;
 Check(combat_command::Edit(Fixture::At(f.command),Fixture::At(f.rawBase)+16,f.sequence,std::array{10.f,-160.f,0.f},
  false,combat_access::CommandIO(),{},combat_command::AngleMode::BaseOnly),"Anti aim after strafe could not commit");
 Check(std::abs(f.messages[0].payload.forward+firstStrafe.forward)<.00001f&&std::abs(f.messages[0].payload.left+firstStrafe.left)<.00001f&&
  f.Base().forward==0&&f.Base().left==0&&f.view.y==20,"Anti aim lost strafe direction, restored base analog or moved the visible view");
 movement::FinishCommand(Fixture::At(f.input));movement::Process(Fixture::At(f.input),false);
 Check(f.crcCalls==2&&f.Base().subticks.size==32,"Finalized anti-aim/strafe command accumulated another window");
 setup(air);movement::Process(Fixture::At(f.input),true);
 Check(f.crcCalls==0&&f.Base().subticks.size==0,"Air strafe competed with an actual shot");
 setup(air);movement::native::api.ray=movement::native::api.filter=0;
 movement::Process(Fixture::At(f.input),false);
 Check(f.crcCalls==1&&f.Base().subticks.size==32,"Missing optional hull binding disabled ordinary strafe");
 movement::Reset();movement::settings={};movement::menuOpen=true;
}
void PublicHeldJumpCycle(){
 Fixture f;Bind(f);movement::Reset();movement::settings={};movement::settings.enabled=true;movement::menuOpen=false;
 auto &options=movement::settings.options;options.bhop=options.jumpbug=options.edgebug=true;
 options.edgebugMode=1;options.edgebugPasses=1;options.edgebugJumpSteps=false;
 constexpr auto jump=movement_policy::Jump;
 const auto next=[&](bool ground,float velocity,bool held,bool fresh) {
  ++f.identity.sequence;f.Put(f.command,kCommandSequence,f.identity.sequence);
  f.Put(f.pawn,0x3f4,std::uint32_t{0x10u|(ground?1u:0u)});f.Put(f.pawn,0x430,Vec3{200,15,velocity});
  const auto value=std::uint64_t{8}|(held?jump:0),changed=fresh?jump:0;
  f.Put(f.command,kCommandButtons,ButtonState{0,value,changed,0});
  ButtonPayload pb{7,0,value,changed,0};std::memcpy(&f.buttonMessage.payload,&pb,sizeof(pb));
  auto base=f.Base();base.subticks={0x77,0,128,reinterpret_cast<Address>(&f.initial)};f.initial.allocated=128;
  for(int i=0;i<128;++i){f.initial.entries[i]=reinterpret_cast<Address>(&f.messages[i]);f.messages[i]={};}
  f.Base(base);f.rayCalls=0;
 };
 next(true,0,true,true);movement::Process(Fixture::At(f.input));
 Check(f.crcCalls==0&&movement::observation.freshJump&&std::string_view(movement::status.load()).find("Manual jump")!=std::string_view::npos,
  "Fresh manual grounded jump was replaced or its idle gate hidden");
 next(false,250,true,false);movement::Process(Fixture::At(f.input));
 Check(f.crcCalls==0&&!movement::observation.freshJump&&std::string_view(movement::status.load()).find("Jump rising")!=std::string_view::npos,
  "Held rising takeoff was suppressed or its idle gate hidden");
 next(false,0,true,false);movement::Process(Fixture::At(f.input));
 Check(f.crcCalls==1&&f.Base().subticks.size==4&&std::string_view(movement::status.load()).find("Jumpbug")!=std::string_view::npos,
  "Held apex with flat landing failed to commit screenshot-options jumpbug");
 movement::Process(Fixture::At(f.input));Check(f.crcCalls==1&&f.Base().subticks.size==4,"Replayed jumpbug window accumulated messages");
 // Miss the last airborne hull forecast. Grounded held input must recover.
 next(false,-50,true,false);f.manager=0;movement::Process(Fixture::At(f.input));
 const auto released=*reinterpret_cast<ButtonState*>(f.command.data()+kCommandButtons);
 Check(f.crcCalls==2&&!(released.value&jump)&&f.Base().subticks.size==0,"Held descent without hull did not release jump");
 next(true,0,true,false);f.manager=0xbeef;movement::Process(Fixture::At(f.input));
 Check(f.crcCalls==3&&f.Base().subticks.size==2&&f.messages[0].payload.button==jump&&!f.messages[0].payload.pressed&&
  f.messages[0].payload.when==0&&f.messages[1].payload.button==jump&&f.messages[1].payload.pressed&&
  f.messages[1].payload.when==1.f/64.f,"Held ground after missed forecast failed to rearm release/press");
 movement::Process(Fixture::At(f.input));Check(f.crcCalls==3&&f.Base().subticks.size==2,"Grounded rearm replay accumulated pulses");
 next(false,250,true,false);movement::Process(Fixture::At(f.input));Check(f.crcCalls==3,"Second held takeoff was suppressed");
 next(false,-50,true,false);f.rayMode=6;movement::Process(Fixture::At(f.input));
 Check(f.crcCalls==4&&f.Base().subticks.size==2&&f.messages[0].payload.button==movement_policy::Duck&&
  std::string_view(movement::status.load()).find("Edgebug")!=std::string_view::npos,"Exact screenshot options lost edge-landing duck window");
 next(true,0,false,true);movement::Process(Fixture::At(f.input));Check(f.crcCalls==4&&f.Base().subticks.size==0,"Actual manual jump release was reversed");
 // Never invent held input when a rebuilt native command carries no Jump.
 next(true,0,false,false);movement::Process(Fixture::At(f.input));Check(f.crcCalls==4,"Absent native held intent was synthesized from previous hop");
 // Ground recovery owns only Jump. An unrelated late mixed reload/duck event
 // must survive and follow the earlier release/press pair in native order.
 next(true,0,true,false);f.messages[0].payload.bits=7;f.messages[0].payload.button=(1ull<<13)|movement_policy::Duck;
 f.messages[0].payload.pressed=true;f.messages[0].payload.when=.8f;const auto unrelated=f.messages[0];
 auto base=f.Base();base.subticks.size=1;f.Base(base);movement::Process(Fixture::At(f.input));
 Check(f.crcCalls==5&&f.Base().subticks.size==3&&!std::memcmp(&unrelated,&f.messages[0],sizeof(unrelated))&&
  f.initial.entries[0]==reinterpret_cast<Address>(&f.messages[1])&&f.initial.entries[1]==reinterpret_cast<Address>(&f.messages[2])&&
  f.initial.entries[2]==reinterpret_cast<Address>(&f.messages[0]),"Ground recovery erased or reordered unrelated native action");
 movement::Reset();movement::settings={};movement::menuOpen=true;
}
void PhysicalSpaceRecovery(){
 Fixture f;Bind(f);movement::Reset();movement::settings={};movement::settings.options.bhop=true;movement::menuOpen=false;
 constexpr auto jump=movement_policy::Jump;
 fixture_input::SetKey(VK_SPACE,true);f.manager=0;
 const auto next=[&](bool ground,float vz,bool nativeJump,bool fresh=false){
  ++f.identity.sequence;f.Put(f.command,kCommandSequence,f.identity.sequence);
  f.Put(f.pawn,0x3f4,std::uint32_t{0x10u|(ground?1u:0u)});f.Put(f.pawn,0x430,Vec3{200,15,vz});
  f.Put(f.command,kCommandButtons,ButtonState{0,nativeJump?jump:0,fresh?jump:0,0});
  const ButtonPayload pb{7,0,nativeJump?jump:0,fresh?jump:0,0};std::memcpy(&f.buttonMessage.payload,&pb,sizeof(pb));
  auto base=f.Base();base.subticks={0x77,0,128,reinterpret_cast<Address>(&f.initial)};f.initial.allocated=128;
  for(int i=0;i<128;++i){f.initial.entries[i]=reinterpret_cast<Address>(&f.messages[i]);f.messages[i]={};}f.Base(base);
 };
 next(true,0,false);movement::Process(Fixture::At(f.input));
 Check(!movement::spaceJumpArmed&&f.crcCalls==0,"Unconfirmed Space binding synthesized jump");
 next(true,0,true,true);movement::Process(Fixture::At(f.input));
 Check(movement::spaceJumpArmed&&f.crcCalls==0,"Native Space takeoff did not arm continuous held input");
 next(false,-50,true);movement::Process(Fixture::At(f.input));
 Check(f.crcCalls==1&&movement::spaceJumpArmed&&!(reinterpret_cast<ButtonState*>(f.command.data()+kCommandButtons)->value&jump),
  "Descending bhop did not release Jump while retaining physical held intent");
 // The command bytes still contain our release when prediction now lands.
 // A change of grounded state must bypass byte-only intact-command dedup.
 f.Put(f.pawn,0x3f4,std::uint32_t{0x11});f.Put(f.pawn,0x430,Vec3{200,15,0});movement::Process(Fixture::At(f.input));
 Check(f.crcCalls==2&&f.Base().subticks.size==2&&f.messages[1].payload.pressed&&movement::observation.heldJump,
  "Actual landing with unchanged released command bytes lost held-Space recovery");
 movement::Process(Fixture::At(f.input));Check(f.crcCalls==2,"Intact recovered landing emitted duplicate pulses");
 // A subsequent native rebuild can also carry no Jump after our release.
 next(true,0,false);movement::Process(Fixture::At(f.input));
 Check(f.crcCalls==3&&f.Base().subticks.size==2&&movement::observation.heldJump,
  "Rebuilt command with erased native Jump stopped a confirmed Space hold");
 next(true,0,true,true);movement::Process(Fixture::At(f.input));
 Check(f.crcCalls==4&&f.Base().subticks.size==2&&!movement::observation.freshJump,
  "Native reassertion of held Jump was mistaken for another physical Space press");
 next(true,0,true);f.Put(f.command,kCommandButtons,ButtonState{0,jump,0,jump});movement::Process(Fixture::At(f.input));
 Check(f.crcCalls==4&&movement::observation.freshJump,"Confirmed Space hold intercepted a real wheel jump");
 next(true,0,true);f.messages[0].payload.bits=7;f.messages[0].payload.button=jump;
 f.messages[0].payload.pressed=true;f.messages[0].payload.when=.5f;auto manual=f.Base();manual.subticks.size=1;f.Base(manual);
 movement::Process(Fixture::At(f.input));Check(f.crcCalls==5&&!movement::observation.freshJump&&f.Base().subticks.size==3,
  "Native reassertion of a continuous Space hold blocked landing recovery");
 f.pending=true;movement::Process(Fixture::At(f.input));
 Check(movement::spaceJumpArmed&&f.crcCalls==5&&!movement::committed.valid,"Transient weapon-selection gap erased confirmed Space intent");
 f.pending=false;next(true,0,false);movement::Process(Fixture::At(f.input));
 Check(f.crcCalls==6&&f.Base().subticks.size==2,"Held-Space bhop did not recover after a transient native command gap");
 fixture_input::SetKey(VK_SPACE,false);next(true,0,false);movement::Process(Fixture::At(f.input));
 Check(!movement::spaceJumpArmed&&f.crcCalls==6&&f.Base().subticks.size==0,"Physical Space release kept generating hops");
 fixture_input::SetKey(VK_SPACE,true);next(true,0,false);movement::Process(Fixture::At(f.input));
 Check(!movement::spaceJumpArmed&&f.crcCalls==6,"New Space hold without native Jump reused an old binding");
 next(true,0,true,true);movement::Process(Fixture::At(f.input));fixture_input::SetFocus(false);
 movement::Process(Fixture::At(f.input));Check(!movement::spaceJumpArmed,"Focus loss retained physical jump intent");
 fixture_input::SetFocus(true);next(true,0,false);movement::Process(Fixture::At(f.input));
 Check(f.crcCalls==6&&!movement::spaceJumpArmed,"Focus recovery invented a held jump");
 fixture_input::SetKey(VK_SPACE,false);movement::Reset();movement::settings={};movement::menuOpen=true;
}
void AutomaticQuantizedNativeWindow(){
 Fixture f;Bind(f);f.quantized=true;movement::Reset();movement::settings={};movement::settings.options.airStrafe=true;movement::menuOpen=false;
 f.Put(f.pawn,0x3f4,std::uint32_t{0x10});f.Put(f.command,kCommandButtons,ButtonState{});
 const ButtonPayload pb{7,0,0,0,0};std::memcpy(&f.buttonMessage.payload,&pb,sizeof(pb));
 auto base=f.Base();base.forward=base.left=0;base.subticks={0x77,0,128,reinterpret_cast<Address>(&f.initial)};f.initial.allocated=128;
 for(int i=0;i<128;++i)f.initial.entries[i]=reinterpret_cast<Address>(&f.messages[i]);f.Base(base);
 const auto before=movement::native::Read(Fixture::At(f.input),false);
 Check(before.state.quantizeMovement,"Native quantized fixture did not capture the typed convar");
 const auto stamp=movement::Process(Fixture::At(f.input));
 Check(stamp&&stamp->command==Fixture::At(f.command)&&stamp->sequence==f.sequence&&f.crcCalls==1&&
  f.Base().subticks.size==17&&f.Base().forward==1&&f.Base().left==0&&movement::committed.camera,
  "Ordinary air strafe did not commit quantized-compatible movement and camera preservation");
 Check(reinterpret_cast<ButtonState*>(f.command.data()+kCommandButtons)->value&movement_policy::Forward,
  "Quantized air strafe omitted digital forward input");
 float previous=-1;unsigned analog{},yaw{};
 for(int i=0;i<f.Base().subticks.size;++i){const auto raw=f.initial.entries[i];const auto &step=reinterpret_cast<Raw*>(raw)->payload;
  Check(step.when>=previous&&step.when<1,"Automatic quantized window was not chronologically ordered");previous=step.when;
  if(step.bits&0x18){++analog;Check(step.when==0&&std::abs(step.forward-(1-before.state.lastImpulses.x))<.00001f&&
   std::abs(step.left+before.state.lastImpulses.y)<.00001f,"Quantized movement impulse used the wrong starting input");}
  if(step.bits&0x40)++yaw;
 }
 Check(analog==1&&yaw==16&&f.view.x==10&&f.view.y==20,"Quantized native input lost its yaw window or moved the visible view");
 movement::FinishCommand(Fixture::At(f.input));movement::Process(Fixture::At(f.input));
 Check(f.crcCalls==1&&f.Base().subticks.size==17,"Replayed automatic quantized window accumulated native messages");
 // Same-command mouse updates must replace the whole owned window rather
 // than treating its last generated fraction as new native input timing.
 for(unsigned pass=0;pass<8;++pass){
  f.view.y+=1;const auto updated=movement::Process(Fixture::At(f.input));
  Check(updated&&f.Base().subticks.size==17,"Mouse update lost or accumulated its quantized window");
  const auto &firstYaw=reinterpret_cast<Raw*>(f.initial.entries[1])->payload;
  Check(std::abs(firstYaw.when-1.f/17)<.00001f&&
   reinterpret_cast<AnglePayload*>(&f.angleMessage.payload)->yaw==f.view.y,
   "Mouse replay compressed the strafe window or retained a stale command angle");
  movement::FinishCommand(Fixture::At(f.input));
 }
 const auto beforeAA=f.crcCalls;
 movement::Process(Fixture::At(f.input)); // Actual caller applies AA before FinishCommand.
 Check(combat_command::Edit(Fixture::At(f.command),Fixture::At(f.rawBase)+16,f.sequence,std::array{10.f,f.view.y-180.f,0.f},
  false,combat_access::CommandIO(),{},combat_command::AngleMode::BaseOnly),"Quantized window blocked base-only anti aim");
 movement::FinishCommand(Fixture::At(f.input));movement::Process(Fixture::At(f.input));
 Check(f.crcCalls==beforeAA+1&&f.Base().subticks.size==17&&f.Base().forward==-1&&f.view.y==28,
  "Post-AA quantized strafe lost movement, visible camera or replay ownership");
 movement::Reset();movement::settings={};movement::menuOpen=true;
}
void PrimeMovementWindow(Fixture &f,bool nativeJump){
 auto base=f.Base();base.subticks={0x77,nativeJump?1:0,128,reinterpret_cast<Address>(&f.initial)};f.initial.allocated=128;
 for(int index=0;index<128;++index){f.initial.entries[index]=reinterpret_cast<Address>(&f.messages[index]);f.messages[index]={};}
 if(nativeJump){auto &step=f.messages[0].payload;step.bits=7;step.button=movement_policy::Jump;step.pressed=true;step.when=1.f;}
 f.Base(base);
}
void RepeatedSpaceNativeEdges(){
 Fixture f;Bind(f);movement::Reset();movement::settings={};movement::settings.options.bhop=true;movement::menuOpen=false;
 fixture_input::SetKey(VK_SPACE,true);f.manager=0;constexpr auto jump=movement_policy::Jump;
 const auto next=[&](bool grounded,float vz,bool held){
  ++f.identity.sequence;f.Put(f.command,kCommandSequence,f.identity.sequence);
  f.Put(f.pawn,0x3f4,std::uint32_t{0x10u|(grounded?1u:0u)});f.Put(f.pawn,0x430,Vec3{250,0,vz});
  f.Put(f.command,kCommandButtons,ButtonState{0,held?jump:0,jump,0});
  // The next native command can still carry the previous protobuf release.
  const ButtonPayload released{7,0,0,0,0};std::memcpy(&f.buttonMessage.payload,&released,sizeof(released));
  PrimeMovementWindow(f,held);
 };
 next(true,0,true);movement::Process(Fixture::At(f.input));
 Check(f.crcCalls==0&&movement::spaceJumpArmed,"First physical Space press was changed or never confirmed");
 for(unsigned cycle=0;cycle<5;++cycle){
  next(false,250,true);movement::Process(Fixture::At(f.input));
  Check(f.crcCalls==cycle*2,"Continuous rising hop was treated as a new landing");
  next(false,-80,true);movement::Process(Fixture::At(f.input));
  Check(f.crcCalls==cycle*2+1&&!movement::observation.freshJump&&
   !(reinterpret_cast<ButtonState*>(f.command.data()+kCommandButtons)->value&jump),
   "Repeated native Jump press blocked the airborne release");
  next(true,0,true);movement::Process(Fixture::At(f.input));
  const auto raw=*reinterpret_cast<ButtonState*>(f.command.data()+kCommandButtons);
  const auto pb=*reinterpret_cast<ButtonPayload*>(&f.buttonMessage.payload);
  Check(f.crcCalls==cycle*2+2&&!movement::observation.freshJump&&f.Base().subticks.size==3&&
   (raw.value&jump)&&(pb.value&jump)&&((raw.changed^pb.changed)&jump)==0&&
   !f.messages[1].payload.pressed&&f.messages[2].payload.pressed,
   "Bunnyhop stopped after one jump or native/protobuf landing input diverged");
  movement::FinishCommand(Fixture::At(f.input));movement::Process(Fixture::At(f.input));
  Check(f.crcCalls==cycle*2+2&&f.Base().subticks.size==3,"Replayed landing emitted duplicate jump pulses");
 }
 fixture_input::SetKey(VK_SPACE,false);next(true,0,false);movement::Process(Fixture::At(f.input));
 Check(f.crcCalls==10&&!movement::spaceJumpArmed,"Physical release continued the multi-hop cycle");
 movement::Reset();movement::settings={};movement::menuOpen=true;
}
movement_policy::Vec2 NativeAirVelocity(const Fixture &f){
 const auto base=f.Base();const auto angles=*reinterpret_cast<const AnglePayload*>(&f.angleMessage.payload);
 bool analog=false;for(int index=0;index<base.subticks.size;++index){const auto raw=f.initial.entries[index];analog|=(reinterpret_cast<const Raw*>(raw)->payload.bits&0x18)!=0;}
 double vx=250,vy=0,yaw=angles.yaw,forward=analog?0:base.forward,left=analog?0:base.left;
 // Integrate the committed wire representation with independent air-accel
 // math; this catches commands whose deltas were rotated into the wrong yaw.
 for(int index=0;index<base.subticks.size;++index){
  const auto &step=reinterpret_cast<const Raw*>(f.initial.entries[index])->payload;
  if(step.bits&8)forward+=step.forward;if(step.bits&16)left+=step.left;if(step.bits&0x40)yaw+=step.yaw;
  const double until=index+1<base.subticks.size?reinterpret_cast<const Raw*>(f.initial.entries[index+1])->payload.when:1;
  const double dt=(until-step.when)/64,rad=yaw*std::numbers::pi/180;
  double wx=std::cos(rad)*forward-std::sin(rad)*left,wy=std::sin(rad)*forward+std::cos(rad)*left;
  const double length=std::hypot(wx,wy);if(length<1e-8||dt<=0)continue;wx/=length;wy/=length;
  const double add=std::min(30.,length*250)-(vx*wx+vy*wy);
  if(add>0){const double gain=std::min(12*250*dt,add);vx+=wx*gain;vy+=wy*gain;}
 }
 return {static_cast<float>(vx),static_cast<float>(vy)};
}
float NativeAirSpeed(const Fixture &f){const auto velocity=NativeAirVelocity(f);return std::hypot(velocity.x,velocity.y);}
void AirborneWireAndYaw(){
 for(const std::uint64_t buttons:{movement_policy::Forward,movement_policy::MoveLeft,movement_policy::MoveRight,
      movement_policy::Forward|movement_policy::MoveLeft,std::uint64_t{0}}){
  Fixture f;Bind(f);movement::Reset();movement::settings={};movement::settings.options.airStrafe=true;movement::menuOpen=false;
  f.Put(f.pawn,0x3f4,std::uint32_t{0x10});f.Put(f.pawn,0x430,Vec3{250,0,-50});f.Put(f.services,0x1a0,0.f);f.Put(f.services,0x1a4,0.f);
  f.Put(f.command,kCommandButtons,ButtonState{0,buttons,buttons,movement_policy::Duck});
  ButtonPayload stale{7,0,movement_policy::Back,0,0};std::memcpy(&f.buttonMessage.payload,&stale,sizeof(stale));
  AnglePayload retained{7,0,10,-160,0};std::memcpy(&f.angleMessage.payload,&retained,sizeof(retained));PrimeMovementWindow(f,true);
  const auto applied=movement::Process(Fixture::At(f.input));
  Check(applied.has_value()&&f.crcCalls==1&&f.Base().subticks.size==33&&
   reinterpret_cast<AnglePayload*>(&f.angleMessage.payload)->yaw==f.view.y&&NativeAirSpeed(f)>250,
   "Analog autostrafe failed to accelerate its committed airborne wire input after a retained yaw");
  const auto velocity=NativeAirVelocity(f);
  Check(buttons==movement_policy::MoveRight?velocity.y<0:velocity.y>0,
   "Committed analog strafe turned against the visible yaw or held direction");
  const auto raw=*reinterpret_cast<ButtonState*>(f.command.data()+kCommandButtons);const auto pb=*reinterpret_cast<ButtonPayload*>(&f.buttonMessage.payload);
  constexpr auto mask=movement_policy::DirectionButtons;
  Check(((raw.value^pb.value)&mask)==0&&((raw.changed^pb.changed)&mask)==0&&((raw.scroll^pb.scroll)&mask)==0&&
   (raw.scroll&movement_policy::Duck)&&!(pb.scroll&movement_policy::Duck)&&
   f.messages[0].payload.button==movement_policy::Jump&&f.messages[0].payload.pressed,
   "Autostrafe left stale protobuf movement or consumed the native jump event");
  movement::FinishCommand(Fixture::At(f.input));movement::Process(Fixture::At(f.input));
  Check(f.crcCalls==1&&f.Base().subticks.size==33,"Repeated airborne command appended another strafe window");
  f.view.y+=5;Check(movement::Process(Fixture::At(f.input)).has_value()&&f.crcCalls==2&&f.Base().subticks.size==33&&NativeAirSpeed(f)>250,
   "Mouse update lost or accumulated the analog strafe window");
 }
 for(bool explicitStrafer:{false,true}){
  Fixture f;Bind(f);f.quantized=true;movement::Reset();movement::settings={};movement::settings.options.airStrafe=!explicitStrafer;
  movement::settings.options.testStrafer=explicitStrafer;movement::menuOpen=false;
  f.Put(f.pawn,0x3f4,std::uint32_t{0x10});f.Put(f.pawn,0x430,Vec3{250,0,-50});f.Put(f.services,0x1a0,0.f);f.Put(f.services,0x1a4,0.f);
  AnglePayload retained{7,0,10,-160,0};std::memcpy(&f.angleMessage.payload,&retained,sizeof(retained));PrimeMovementWindow(f,true);
  const auto context=movement::native::Read(Fixture::At(f.input),false);
  Check(context.valid&&context.state.maxSubtickWhen==0,"Late native Jump event blocked the quantized angle window");
  Check(movement::Process(Fixture::At(f.input)).has_value()&&f.crcCalls==1&&f.Base().subticks.size==(explicitStrafer?17:18)&&
   reinterpret_cast<AnglePayload*>(&f.angleMessage.payload)->yaw==f.view.y&&NativeAirSpeed(f)>250&&
   f.messages[0].payload.button==movement_policy::Jump&&f.messages[0].payload.pressed,
   "Late Jump or retained anti-aim yaw disabled quantized autostrafe");
 }
 movement::Reset();movement::settings={};movement::menuOpen=true;
}
void SubtickNativeOwnership(){
 Fixture f;Bind(f);auto base=f.Base();base.subticks={0x77,1,2,reinterpret_cast<Address>(&f.initial)};f.initial.allocated=2;
 for(unsigned i=0;i<2;++i){f.initial.entries[i]=reinterpret_cast<Address>(&f.messages[i]);f.messages[i].payload.bits=0x7f;f.messages[i].payload.cachedSize=123+i;f.messages[i].header.fill(std::byte{0x68});}
 f.Base(base);const auto untouched=f.messages[1];const auto reused=movement::native::AcquireSubtick(f.FieldAddress());
 Check(reused==reinterpret_cast<Address>(&f.messages[1])&&f.allocateCalls==0&&f.appendCalls==0&&f.Base().subticks.size==2&&
  std::memcmp(&untouched,&f.messages[1],sizeof(untouched))==0,"native inactive reuse changed payload/header before commit");
 const auto appended=movement::native::AcquireSubtick(f.FieldAddress());Check(appended&&f.allocateCalls==1&&f.appendCalls==1&&f.Base().subticks.size==3&&
  f.Base().subticks.capacity==16&&f.Base().subticks.entries==reinterpret_cast<Address>(&f.grown)&&f.grown.entries[0]==f.initial.entries[0],
  "native append lost allocated storage/growth ownership");
 auto current=f.Base();current.subticks.size=1;f.Base(current);Check(movement::native::AcquireSubtick(f.FieldAddress())==reused&&f.allocateCalls==1,
  "retained inactive allocation was not reusable");
 f.grown.allocated=17;Check(!movement::native::AcquireSubtick(f.FieldAddress())&&f.allocateCalls==1,"invalid allocated count reached native allocator");
 f.grown.allocated=2;current=f.Base();current.subticks.size=2;f.Base(current);f.failAllocate=true;Check(!movement::native::AcquireSubtick(f.FieldAddress()),"failed native allocation accepted");
}
void MovementMenuControls(){
 struct Capture final:gui_capture::Sink {
  std::unordered_map<std::string,std::string> values;
  std::unordered_map<std::string,bool> booleans;std::optional<bool> master;
  bool Boolean(const char *label,bool *value)override{
   booleans[label]=*value;
   if(std::string_view(label)!="Enable movement assists"||!master||*master==*value)return false;
   *value=*master;return true;
  }
  bool Number(const char*,int,void*,int,const void*,const void*,const char*,double)override{return false;}
  bool Color(const char*,float*,int)override{return false;}
  bool Text(const char *label,char *value,std::size_t,bool readOnly)override{
   Check(readOnly,"Movement diagnostic was editable");values[label]=value;return false;
  }
  bool Action(const char*,bool)override{return false;}
  bool Radio(const char*,bool)override{return false;}
  void BeginSelect(const char*,const char*)override{}
  bool Option(const char*,bool,bool)override{return false;}
  void EndSelect()override{}
  void Section(const char*)override{}
  void BeginGroup(const char*,const char*,unsigned)override{}
  void EndGroup()override{}
  bool Keybind(const char*,int*)override{return false;}
  bool Card(const char*,const char*,bool,bool)override{return false;}
  void ReportError(const char*)override{}
 } capture;
 Fixture f;Bind(f);movement::Reset();movement::settings={};movement::settings.options.airStrafe=true;movement::menuOpen=false;
 f.Put(f.pawn,0x3f4,std::uint32_t{0x10});f.missingCvar=true;
 movement::Process(Fixture::At(f.input));
 Check(f.crcCalls==0&&movement::observation.valid&&!movement::observation.airPhysics,
  "Missing movement physics was not reflected in its gameplay snapshot");
 movement::SetPipelineStatus("Input binding unavailable.");movement::SetInputState(true);movement::Process(Fixture::At(f.input));
 ImGui::CreateContext();auto &io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize={800,600};io.DeltaTime=1.f/60;
 unsigned char *pixels;int width,height;io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
 const auto draw=[&]{ImGui::NewFrame();ImGui::Begin("Movement controls");gui_capture::active=&capture;movement::DrawMenu();gui_capture::active=nullptr;
  ImGui::End();ImGui::EndFrame();};
 draw();Check(capture.values.empty()&&capture.booleans["Enable movement assists"],"Movement menu exposed debug data or omitted its enabled master toggle");
 Check(std::string_view(movement::pipelineStatus.load())=="Input binding unavailable."&&
  std::string_view(movement::gameplayStatus.load()).find("valid movement physics")!=std::string_view::npos,
  "Removing product debug fields erased internal pipeline/gameplay evidence");
 capture.master=false;draw();capture.master.reset();draw();
 Check(!capture.booleans["Enable movement assists"]&&!movement::Get().enabled&&movement::Get().options.airStrafe,
  "Embedded movement toggle failed read-set-read or page viewing re-enabled movement");
 movement::SetInputState(false);movement::spaceJumpArmed=true;movement::direction.previous=movement_policy::Forward;
 movement::Process(Fixture::At(f.input));
 Check(f.crcCalls==0&&!movement::spaceJumpArmed&&!movement::direction.previous&&!movement::committed.valid,
  "Disabled movement master did not gate processing and reset transient input");
 capture.master=true;draw();capture.master.reset();draw();
 Check(capture.booleans["Enable movement assists"]&&movement::Get().enabled,"Embedded movement master could not be re-enabled");
 ImGui::DestroyContext();
 movement::SetPipelineStatus(nullptr);movement::Reset();movement::settings={};movement::menuOpen=true;
}
void GroundShotPreparation(){
 Fixture f;Bind(f);f.Equip();movement::Reset();movement::settings={};movement::settings.options.slowWalk=true;movement::menuOpen=false;
 movement::settings.enabled=false; // Rage auto-stop has its own independent switch.
 auto base=f.Base();base.subticks={0x77,0,2,reinterpret_cast<Address>(&f.initial)};f.initial.allocated=2;
 f.initial.entries[0]=reinterpret_cast<Address>(&f.messages[0]);f.initial.entries[1]=reinterpret_cast<Address>(&f.messages[1]);f.Base(base);
 // A repeated native command can retain the preceding anti-aim angle even
 // though the input adapter still observes the player's visible yaw.
 AnglePayload priorAngles{7,0,10,-160,0};std::memcpy(&f.angleMessage.payload,&priorAngles,sizeof(priorAngles));
 f.Put(f.command,kCommandButtons,ButtonState{0,8|kAttack,0,0});
 const auto initialProto=*reinterpret_cast<const CommandProto*>(f.command.data()+kCommandProto);
 const auto prepared=movement::PrepareShot(Fixture::At(f.input));const auto stopped=f.Base();
 ButtonState buttons{};std::memcpy(&buttons,f.command.data()+kCommandButtons,sizeof(buttons));
 Check(prepared&&prepared->sequence==f.sequence&&f.crcCalls==1&&stopped.forward<0&&stopped.subticks.size==1&&
  (buttons.value&kAttack)&&movement::committed.shotPreparation&&movement::committed.camera,
  "Ground shot preparation did not countersteer or preserve held attack during cooldown");
 Check(!std::memcmp(&initialProto,f.command.data()+kCommandProto,sizeof(initialProto)),"Shot preparation changed shot history or command proto");
 AnglePayload stoppedAngles{};std::memcpy(&stoppedAngles,&f.angleMessage.payload,sizeof(stoppedAngles));
 Check(stoppedAngles.pitch==f.view.x&&stoppedAngles.yaw==f.view.y,"Shot preparation did not anchor countersteer to visible angles after a native replay");
 const double yaw=stoppedAngles.yaw*std::numbers::pi/180;
 const double worldX=std::cos(yaw)*stopped.forward-std::sin(yaw)*stopped.left;
 const double worldY=std::sin(yaw)*stopped.forward+std::cos(yaw)*stopped.left;
 Check(worldX*200+worldY*15<0,"Shot preparation movement accelerated along current velocity");
 Check(movement::Process(Fixture::At(f.input),true).has_value(),"Replayed shot preparation lost visible-camera restoration");
 movement::FinishCommand(Fixture::At(f.input));
 movement::Process(Fixture::At(f.input),true);
 Check(f.crcCalls==1&&f.Base().forward==stopped.forward&&movement::committed.shotPreparation,
  "Ordinary slowwalk or command replay replaced shot preparation");
 Check(combat_command::Edit(Fixture::At(f.command),Fixture::At(f.rawBase)+16,f.sequence,std::array{10.f,-160.f,0.f},
  false,combat_access::CommandIO(),{},combat_command::AngleMode::BaseOnly),"Anti aim after shot preparation could not commit");
 const auto rotated=f.Base();AnglePayload rotatedAngles{};std::memcpy(&rotatedAngles,&f.angleMessage.payload,sizeof(rotatedAngles));
 const double outgoingYaw=rotatedAngles.yaw*std::numbers::pi/180;
 const double outgoingX=std::cos(outgoingYaw)*rotated.forward-std::sin(outgoingYaw)*rotated.left;
 const double outgoingY=std::sin(outgoingYaw)*rotated.forward+std::cos(outgoingYaw)*rotated.left;
 Check(outgoingX*200+outgoingY*15<0&&f.view.y==20,"Anti aim reversed shot preparation or moved the visible camera");
 movement::FinishCommand(Fixture::At(f.input));movement::Process(Fixture::At(f.input),true);
 Check(f.crcCalls==2&&f.Base().forward==rotated.forward&&movement::committed.shotPreparation,
  "Finalized anti-aim shot preparation accumulated another movement window");
 movement::Reset();f.crcCalls=0;f.Put(f.pawn,0x3f4,std::uint32_t{0x10});
 Check(!movement::PrepareShot(Fixture::At(f.input))&&f.crcCalls==0,"Airborne shot preparation changed movement");
 f.Put(f.pawn,0x3f4,std::uint32_t{0x11});f.Put(f.command,kCommandButtons,ButtonState{0,8|movement_policy::Jump,movement_policy::Jump,0});
 Check(!movement::PrepareShot(Fixture::At(f.input))&&f.crcCalls==0,"Shot preparation consumed manual jumping");
 f.Put(f.command,kCommandButtons,ButtonState{0,8,0,0});f.missingCvar=true;movement::native::Reset();
 Check(!movement::PrepareShot(Fixture::At(f.input))&&f.crcCalls==0,"Shot preparation synthesized unavailable ground physics");
 f.missingCvar=false;movement::native::Reset();f.failCrc=true;const auto before=f.Base();const auto beforeAngles=f.angleMessage;
 Check(!movement::PrepareShot(Fixture::At(f.input))&&f.crcCalls==2,"Failed shot preparation checksum reported a committed command");
 const auto after=f.Base();Check(!std::memcmp(&before,&after,sizeof(before)),"Failed shot preparation checksum changed movement");
 Check(!std::memcmp(&beforeAngles,&f.angleMessage,sizeof(beforeAngles)),"Failed shot preparation checksum changed view angles");
 movement::Reset();movement::settings={};movement::menuOpen=true;
}
void QualifiedShotPreparation(){
 const auto setup=[](Fixture &f){Bind(f);f.Equip();movement::Reset();movement::settings={};movement::menuOpen=false;
  f.Put(f.weapon,0x100,100);f.Put(f.command,kCommandButtons,ButtonState{0,8,0,0});
  const ButtonPayload buttons{7,0,8,0,0};std::memcpy(&f.buttonMessage.payload,&buttons,sizeof(buttons));PrimeMovementWindow(f,false);};
 const auto action=[](Fixture &f,int source,std::uint64_t bits){
  if(source==0){f.Put(f.command,kCommandButtons,ButtonState{0,8|bits,bits,bits});}
  if(source==1){const ButtonPayload buttons{7,0,8|bits,bits,bits};std::memcpy(&f.buttonMessage.payload,&buttons,sizeof(buttons));}
  if(source==2){f.messages[0].payload={};auto &step=f.messages[0].payload;step.bits=7;step.button=bits;step.pressed=true;step.when=.6f;
   auto base=f.Base();base.subticks.size=1;f.Base(base);}
 };
 for(int source=0;source<3;++source){Fixture f;setup(f);action(f,source,kAttack);
  const auto before=f.Base();const auto raw=*reinterpret_cast<const ButtonState*>(f.command.data()+kCommandButtons);
  const auto pb=*reinterpret_cast<const ButtonPayload*>(&f.buttonMessage.payload);const auto native=f.messages[0];
  Check(!movement::PrepareShot(Fixture::At(f.input))&&!f.crcCalls,
   "Default shot preparation intercepted a ready primary attack without qualified intent");
  const auto unchanged=f.Base();Check(!std::memcmp(&before,&unchanged,sizeof(before)),"Rejected preparation changed the command");
  Check(movement::PrepareShot(Fixture::At(f.input),true,false).has_value()&&f.crcCalls==1&&f.Base().forward<0,
   "Qualified accuracy preparation failed to brake a ready held primary attack");
  const auto afterRaw=*reinterpret_cast<const ButtonState*>(f.command.data()+kCommandButtons);
  const auto afterPb=*reinterpret_cast<const ButtonPayload*>(&f.buttonMessage.payload);
  Check((afterRaw.value&kAttack)==(raw.value&kAttack)&&(afterRaw.changed&kAttack)==(raw.changed&kAttack)&&
   (afterRaw.scroll&kAttack)==(raw.scroll&kAttack)&&(afterPb.value&kAttack)==(pb.value&kAttack)&&
   (afterPb.changed&kAttack)==(pb.changed&kAttack)&&(afterPb.scroll&kAttack)==(pb.scroll&kAttack),
   "Qualified preparation changed independent primary attack input");
  if(source==2)Check(!std::memcmp(&native,&f.messages[0],sizeof(native)),"Qualified preparation erased the native primary edge");
  movement::FinishCommand(Fixture::At(f.input));const auto stopped=f.Base();
  Check(movement::PrepareShot(Fixture::At(f.input),true,false).has_value()&&f.crcCalls==1,
   "Replayed qualified preparation accumulated another braking window");
  Check(!movement::PrepareShot(Fixture::At(f.input),true,true)&&f.crcCalls==1&&movement::finishPending,
   "Preparation changed a committed shot or cleared its pending movement finalization");
  const auto protectedBase=f.Base();Check(!std::memcmp(&stopped,&protectedBase,sizeof(stopped)),"Committed-shot gate changed base input");
 }
 for(const std::uint64_t interaction:{movement_policy::Use,1ull<<11})for(int source=0;source<3;++source){
  Fixture f;setup(f);action(f,source,interaction);
  const auto before=f.Base();const auto raw=f.command;const auto pb=f.buttonMessage;const auto native=f.messages[0];
  Check(!movement::PrepareShot(Fixture::At(f.input),true,false)&&!f.crcCalls,
   "Qualified primary intent overrode Use or secondary attack input");
  const auto after=f.Base();Check(!std::memcmp(&before,&after,sizeof(before))&&raw==f.command&&
   !std::memcmp(&pb,&f.buttonMessage,sizeof(pb))&&!std::memcmp(&native,&f.messages[0],sizeof(native)),
   "Interaction preparation gate changed protected input");
 }
 for(int source=0;source<3;++source){Fixture f;setup(f);action(f,source,movement_policy::Jump);
  Check(!movement::PrepareShot(Fixture::At(f.input),true,false)&&!f.crcCalls,"Qualified primary intent consumed manual Jump");}
 Fixture committed;setup(committed);const auto before=committed.Base();
 Check(!movement::PrepareShot(Fixture::At(committed.input),true,true)&&!committed.crcCalls,
  "Explicit committed-shot protection required held primary bytes");
 const auto after=committed.Base();Check(!std::memcmp(&before,&after,sizeof(before)),"Explicit shot protection changed movement");
 movement::Reset();movement::settings={};movement::menuOpen=true;
}
void MovementEligibilityReasons(){
 Fixture f;Bind(f);movement::Reset();movement::settings={};movement::settings.options.bhop=movement::settings.options.airStrafe=true;
 movement::menuOpen=false;f.Put(f.pawn,0x430,Vec3{200,15,0});movement::Process(Fixture::At(f.input));
 Check(f.crcCalls==0&&std::string_view(movement::status.load()).find("Hold jump")!=std::string_view::npos,
  "Mixed bunnyhop/air settings hid the required jump input on ground");
 movement::settings={};movement::Process(Fixture::At(f.input));
 Check(std::string_view(movement::status.load())=="Enable a movement assist.","Disabled assists retained a misleading active status");
 movement::settings.options.slowWalk=true;movement::settings.keys[3]=VK_SHIFT;fixture_input::SetKey(VK_SHIFT,false);
 movement::Process(Fixture::At(f.input));
 Check(std::string_view(movement::status.load()).find("activation key")!=std::string_view::npos,
  "An unheld feature key looked like an unavailable movement hook");
 movement::SetInputState(true);movement::Reset();
 Check(std::string_view(movement::status.load()).find("menu is open")!=std::string_view::npos,
  "The shared hook's repeated pause reset erased the menu-open status");
 movement::settings={};movement::menuOpen=true;
}
void MovementHotPathWork(){
 Fixture stop;Bind(stop);stop.Equip();movement::Reset();movement::settings={};movement::menuOpen=false;
 movement::settings.options.bhop=movement::settings.options.airStrafe=true;
 stop.Put(stop.weapon,0x100,100);PrimeMovementWindow(stop,false);
 Check(movement::PrepareShot(Fixture::At(stop.input),true,false).has_value(),"Hot-path fixture did not prepare a qualified ground stop");
 movement::Process(Fixture::At(stop.input),true);movement::FinishCommand(Fixture::At(stop.input));
 Check(!stop.rayCalls&&stop.crcCalls==1,"Qualified stop/shot movement path traced a hull or rewrote intact braking");
 std::cout<<"  Qualified braking + shot-priority movement: hull traces="<<stop.rayCalls
  <<", checksums="<<stop.crcCalls<<", native reads="<<stop.memoryReads<<", schema lookups="<<stop.schemaReads<<'\n';
 Fixture air;Bind(air);movement::Reset();movement::settings={};movement::settings.options.airStrafe=true;movement::menuOpen=false;
 air.Put(air.pawn,0x3f4,std::uint32_t{0x10});PrimeMovementWindow(air,false);
 auto base=air.Base();base.subticks.size=96;air.Base(base);
 Check(movement::Process(Fixture::At(air.input)).has_value()&&air.Base().subticks.size==128&&air.crcCalls==1,
  "Hot-path fixture did not retain96 native events with its32 generated strafe steps");
 const auto beforeReads=air.memoryReads,beforePointers=air.pointerReads;movement::FinishCommand(Fixture::At(air.input));
 std::cout<<"  Finalize128-step command: native reads="<<air.memoryReads-beforeReads
  <<", pointer-table reads="<<air.pointerReads-beforePointers<<'\n';
 Check(air.pointerReads-beforePointers<=130,"Finalizer reread the native pointer table separately for every owned step");
 Check(movement::committed.valid&&movement::committed.ownedCount==32,"Finalizing a crowded native command lost movement ownership");
 movement::Process(Fixture::At(air.input));Check(air.crcCalls==1,"Finalized crowded command accumulated another strafe window");
 // An external rebuild removing any owned pointer cannot become an intact
 // movement snapshot merely because the remaining entries still validate.
 air.initial.entries[96]=air.initial.entries[0];movement::finishPending=true;movement::FinishCommand(Fixture::At(air.input));
 Check(!movement::committed.valid,"Finalizer retained ownership after a generated native pointer disappeared");
 movement::Reset();movement::settings={};movement::menuOpen=true;
}
}
namespace memory {
bool copy(void *out,Address address,std::size_t count)noexcept{
 if(active){++active->memoryReads;const auto first=reinterpret_cast<Address>(active->initial.entries.data());
  if(address>=first&&address<first+sizeof(active->initial.entries))++active->pointerReads;}
 return out&&game_access::Copy(address,out,count);}
bool store(Address address,const void *in,std::size_t count)noexcept{return game_access::Store(address,in,count);}
Address resolve_cached(std::string_view){return 0;}
}
namespace systems {std::uint32_t schemas::lookup(const char *owner,std::uint32_t hash){
 if(active)++active->schemaReads;
 for(const auto &field:fields)if(std::string_view(owner)==field.owner&&hash==fnv1a::runtime_hash(field.name))return active&&active->missingField?0:field.offset;return 0;}}
namespace fixture_input {
namespace { bool focused=true;std::array<bool,256> keys{}; }
HWND Window(){return reinterpret_cast<HWND>(1);}
DWORD WindowProcess(HWND,DWORD *process){if(process)*process=focused?GetCurrentProcessId():GetCurrentProcessId()+1;return 1;}
SHORT Key(int key){return key>=0&&key<256&&keys[key]?static_cast<SHORT>(0x8000):0;}
void SetKey(int key,bool down){if(key>=0&&key<256)keys[key]=down;}
void SetFocus(bool value){focused=value;}
}
namespace combat_access {
combat_command::IO CommandIO(){return {memory::copy,memory::store,
 +[](Address raw)->Address{return active&&raw==Fixture::At(active->rawBase)?reinterpret_cast<Address>(&active->buttonMessage):0;},
 +[](Address raw,Address destination,const std::uint8_t *bytes,int size){
  if(!active||raw!=Fixture::At(active->rawBase)||destination!=raw+kMessagePayload+offsetof(BaseCommand,moveCrc)||!bytes||size<=0)return false;
  ++active->crcCalls;return !active->failCrc;
 }};}
std::optional<CommandIdentity> ReadInputCommand(Address input){if(!active||input!=Fixture::At(active->input))return {};++active->commandReads;auto result=active->identity;result.weaponSelectionPending=active->pending;return result;}
bool ReadViewAngles(Address input,Vec3 &value){if(!active||input!=Fixture::At(active->input)||active->failView)return false;++active->viewReads;value=active->view;return true;}
bool Finite(const Vec3 &v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
float Convar(const char *name,float fallback){++active->cvarCalls;if(active->missingCvar)return fallback;
 if(std::string_view(name)=="sv_gravity")return 800;if(std::string_view(name)=="sv_standable_normal")return .7f;
 if(std::string_view(name)=="sv_maxspeed")return 250;if(std::string_view(name)=="sv_airaccelerate")return 12;
 if(std::string_view(name)=="sv_air_max_wishspeed")return 30;if(std::string_view(name)=="sv_staminarecoveryrate")return 60;
 if(std::string_view(name)=="sv_stopspeed")return 80;return 5.2f;}
std::optional<bool> ConvarBool(const char *name){++active->cvarCalls;return active->missingCvar?std::nullopt:
 std::optional{std::string_view(name)=="sv_quantize_movement_input"&&active->quantized};}
}
namespace game {bool module_matches(const char *,std::string_view){return false;}}
int main(){try{SnapshotAndManualGates();HeldFireCooldown();FreshManualJumpEdges();GeneratedJumpOwnership();PhysicsSnapshotCache();HullABIAndFailure();RuntimeOwnershipAndMenuLifecycle();PublicRuntimeAdmissionAndCommit();PublicHeldJumpCycle();PhysicalSpaceRecovery();AutomaticQuantizedNativeWindow();RepeatedSpaceNativeEdges();AirborneWireAndYaw();SubtickNativeOwnership();MovementMenuControls();GroundShotPreparation();QualifiedShotPreparation();MovementEligibilityReasons();MovementHotPathWork();movement::native::api={};
 Check(!movement::native::Read(0,false).valid,"unsupported native client bypassed bindings gate");
 std::cout<<"Movement native regression: PASS\n";return 0;}catch(const std::exception &e){std::cerr<<"Movement native regression: FAIL: "<<e.what()<<'\n';return 1;}}
