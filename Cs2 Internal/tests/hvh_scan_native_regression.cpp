// The production scan and CreateMove scheduler run directly. Only adapter,
// native input and penetration boundaries are replaced; no game is loaded.
#include "game/combat_access.h"
namespace scan_fixture {
HWND Window();
DWORD WindowProcess(HWND,DWORD*);
SHORT Key(int);
ULONGLONG Clock();
}
#define GetForegroundWindow scan_fixture::Window
#define GetWindowThreadProcessId scan_fixture::WindowProcess
#define GetAsyncKeyState scan_fixture::Key
#define GetTickCount64 scan_fixture::Clock
#include "features/combat/hvh.cpp"
#undef GetForegroundWindow
#undef GetWindowThreadProcessId
#undef GetAsyncKeyState
#undef GetTickCount64
#include "player_fixture.h"
#include <iostream>
#include <functional>

namespace scan_fixture {
using Address=std::uintptr_t;
using Vec3=combat_access::Vec3;
using namespace combat_bindings;
struct SchemaField {const char* owner;const char* name;unsigned offset;};
constexpr SchemaField fields[]{
 {"C_BaseEntity","m_iTeamNum",0x30},{"C_BaseEntity","m_iHealth",0x40},
 {"C_BaseEntity","m_lifeState",0x44},{"C_BaseEntity","m_pGameSceneNode",0x50},
 {"C_CSPlayerPawn","m_bGunGameImmunity",0x80},{"C_BaseEntity","m_flSimulationTime",0x84},
 {"CGameSceneNode","m_bDormant",0x20},{"CGameSceneNode","m_vecAbsOrigin",0x30},
 {"CSkeletonInstance","m_modelState",0x100},{"CModelState","m_hModel",0x10}
};
template<class T>Address At(const T& value){return reinterpret_cast<Address>(value.data());}
template<class T,class B>void Put(B& block,std::size_t offset,const T& value){
 Check(offset+sizeof(value)<=block.size(),"scan fixture write outside storage");std::memcpy(block.data()+offset,&value,sizeof(value));
}
struct Enemy {
 std::array<std::byte,0x1000> pawn{},scene{};
 std::array<std::byte,0x200> model{};
 Address modelPointer{};
 std::array<systems::bones::data,128> bones{};
 combat_access::Target target;
 float damage{40.f};bool reachable{true},enumerated{true},penetrated{};
 Enemy(unsigned slot,Vec3 position){
  target.pawn=At(pawn);target.handle=0x8000u|slot;target.team=3;target.health=100;
  modelPointer=At(model);
  Put(pawn,0x30,std::uint8_t{3});Put(pawn,0x40,100);Put(pawn,0x44,std::uint8_t{0});
  Put(pawn,0x50,At(scene));Put(pawn,0x80,false);Put(pawn,0x84,18.75f);
  Put(scene,0x20,false);Put(scene,0x30,position);Put(scene,0x110,reinterpret_cast<Address>(&modelPointer));
  Put(scene,0x180,reinterpret_cast<Address>(bones.data()));Put(scene,0x194,std::uint16_t{128});Put(model,0x178,128);
  SetGeometry(1,position);
 }
 void SetGeometry(int count,Vec3 position){
  target.hitboxes.count=count;
  for(int index=0;index<count;++index){auto &box=target.hitboxes.entries[index];
   box.index=index;box.bone=index;box.group=1;box.radius=6;box.mins={0,0,-3};box.maxs={0,0,3};
   bones[index].position=position;bones[index].scale=1;
  }
 }
 void Move(Vec3 position){
  Put(scene,0x30,position);float time{};std::memcpy(&time,pawn.data()+0x84,sizeof(time));Put(pawn,0x84,time+1.f/64.f);
  for(auto &bone:bones)bone.position=position;
 }
};
struct Fixture:PlayerFixture {
 std::array<std::byte,0x100> command{},base{},weapon{},weaponData{};
 combat_access::Context context;
 std::vector<std::unique_ptr<Enemy>> enemies;
 std::vector<backtrack::Snapshot> history;
 ULONGLONG now{1000};bool focused{true},held{},scopeable{},scoped{},scopeSucceeds{true},historyCurrent{true},overwrite{};
 float spreadAmount{};bool missingSpread{},privateSeed{true},mixedSpread{};
 unsigned contexts{},targetScans{},targetEnumerations{},geometryReads{},boneReads{},skeletonReads{},spreadCalls{},traceCalls{},historicalTraces{};
 unsigned seedCalls{};
 unsigned shots{},shotAttempts{},scopes{},scopeQueries{},preparations{},movementCalls{},finalizations{},originalCalls{},targetResets{};
 bool shotSucceeds{true};
 bool preparationAllowed{},preparationCommitted{};Vec3 shotAngles{};
 std::uint32_t lastTraceTarget{};std::vector<std::uint32_t> tracedTargets;
 std::function<void()> onTrace,onSpread,onGeometry;
 std::function<std::optional<std::uint32_t>(Vec3,int)> seedFunction;
 std::optional<combat_bindings::BacktrackTiming> shotTargetTiming;
 Fixture(){
  Set(page,2*0x70,At(weapon));Set(page,2*0x70+0x10,std::uint32_t{0x8002});
  context.local=game_access::ReadLocalPlayer(Base());context.input=At(input);context.command=At(command);context.base=At(base)+kMessagePayload;
  context.commandNumber=77;context.tick=1200;context.valid=context.canShoot=context.accuracyValid=true;
  context.eye={0,0,64};context.angles={};context.weapon.address=At(weapon);context.weapon.data=At(weaponData);
  context.weapon.handle=0x8002;context.weapon.type=3;context.weapon.definition=7;context.weapon.bullets=1;context.weapon.clip=30;
  context.weapon.damage=40;context.weapon.penetration=1;context.weapon.range=1000;context.weapon.rangeModifier=.98f;
  context.weapon.armorRatio=1;context.weapon.headMultiplier=4;context.shotTiming=HistoryShotTiming{1200,.25f};
  CommandProto proto{};proto.bits=1;proto.base=At(base);Put(command,kCommandProto,proto);
  Put(command,kCommandSequence,context.commandNumber);Put(command,kCommandButtons,ButtonState{0,8,0,0});
  Put(base,kMessagePayload,BaseCommand{});
  AddEnemy({200,0,64});
 }
 Enemy& AddEnemy(Vec3 position){
  const unsigned slot=10+static_cast<unsigned>(enemies.size());
  auto enemy=std::make_unique<Enemy>(slot,position);Set(page,slot*0x70,enemy->target.pawn);Set(page,slot*0x70+0x10,enemy->target.handle);
  enemies.push_back(std::move(enemy));return *enemies.back();
 }
 Enemy* Find(Address pawn){for(auto &enemy:enemies)if(enemy->target.pawn==pawn||At(enemy->scene)==pawn)return enemy.get();return nullptr;}
 void Next(bool ready=true){++context.commandNumber;++context.tick;context.shotTiming->tick=context.tick;context.canShoot=ready;
  Put(command,kCommandSequence,context.commandNumber);overwrite=false;++now;}
};
Fixture* active{};
HWND Window(){return reinterpret_cast<HWND>(1);}
DWORD WindowProcess(HWND,DWORD* process){if(process)*process=active&&active->focused?GetCurrentProcessId():GetCurrentProcessId()+1;return 1;}
SHORT Key(int){return active&&active->held?static_cast<SHORT>(0x8000):0;}
ULONGLONG Clock(){return active?active->now:1000;}
void __fastcall Original(Address,int,bool){++active->originalCalls;}
void __fastcall OriginalView(Address,int){}
void Configure(Fixture& fixture){
 active=&fixture;game_access::verifiedGameplayClient=fixture.Base();
 hvh::running=true;hvh::combatReady=true;hvh::menuOpen=false;hvh::commandEpoch=0;hvh::SynchronizeSession();
 auto setting=hvh::Settings{};setting.activationMode=1;setting.noSpread=false;setting.autoScope=false;setting.autoStop=false;setting.showSpreadPrediction=false;
 hvh::Save(setting);hvh::originalMove=Original;hvh::originalView=OriginalView;
}
void Invoke(){hvh::CreateMove(active->context.input,0,false);}
void BasicProductionShot(){
 Fixture f;Configure(f);Invoke();
 Check(f.originalCalls==1&&f.targetScans==1&&f.traceCalls&&f.spreadCalls&&f.shots==1,
  "Production CreateMove did not scan, qualify and commit a valid target");
 Check(f.movementCalls==1&&f.finalizations==1,"Production scan did not retain movement/finalization ordering");
 f.overwrite=true;Invoke();Check(f.shots==1&&f.originalCalls==1,"Committed silent shot was rescanned by native overwrite");
}
void PromptQualifiedLiveShot(){
 Fixture f;Configure(f);f.mixedSpread=true;
 for(int index=1;index<8;++index)f.AddEnemy({200,static_cast<float>(index*4),64}).SetGeometry(20,{200,static_cast<float>(index*4),64});
 f.enemies[0]->SetGeometry(20,{200,0,64});Invoke();
 Check(f.shots==1&&f.traceCalls==1&&f.spreadCalls==128,
  "A clear nonlethal75% shot that passed configured confidence kept scanning other points");
 std::cout<<"  Qualified75% live shot: traces="<<f.traceCalls<<", spread="<<f.spreadCalls<<"; no later point simulations\n";
 Fixture braking;Configure(braking);braking.spreadAmount=.3f;
 for(int index=1;index<8;++index)braking.AddEnemy({200,static_cast<float>(index*4),64}).SetGeometry(20,{200,static_cast<float>(index*4),64});
 braking.enemies[0]->SetGeometry(20,{200,0,64});auto setting=hvh::Get();setting.autoStop=true;hvh::Save(setting);Invoke();
 Check(!braking.shots&&braking.preparations==1&&braking.traceCalls==1&&braking.spreadCalls<=128,
  "Clear damage-qualified acquisition delayed auto-stop until after other candidate traces");
 std::cout<<"  Immediate qualified braking: traces="<<braking.traceCalls<<", spread="<<braking.spreadCalls<<'\n';
 Fixture alreadyQualified;Configure(alreadyQualified);alreadyQualified.mixedSpread=true;
 alreadyQualified.enemies[0]->Move({20,0,64});alreadyQualified.enemies[0]->target.hitboxes.entries[0].radius=10;
 alreadyQualified.enemies[0]->penetrated=true;alreadyQualified.AddEnemy({200,4,64});
 setting=hvh::Get();setting.autoStop=true;setting.hitchance=90;hvh::Save(setting);Invoke();
 Check(alreadyQualified.shots==1&&!alreadyQualified.preparations,
  "A later accuracy-limited target replaced an already qualified shot with braking");
}
void FailedChanceScheduling(){
 Fixture f;Configure(f);f.spreadAmount=.3f;
 auto setting=hvh::Get();setting.autoStop=true;hvh::Save(setting);Invoke();
 Check(!f.shots&&f.traceCalls&&f.spreadCalls&&f.preparations==1&&f.preparationAllowed&&!f.preparationCommitted,
  "Failed hit chance did not retain a qualified pre-shot stopping request");
 Check(f.traceCalls<=48&&f.spreadCalls<=128,"Production scan exceeded trace or lazy native spread budget");
 const auto scans=f.targetScans,traces=f.traceCalls,spread=f.spreadCalls;
 for(int pass=0;pass<8;++pass)Invoke();
 Check(f.originalCalls==9&&f.targetScans==scans&&f.traceCalls==traces&&f.spreadCalls==spread,
  "Unchanged failed CreateMove command repeated target tracing or spread sampling");
 std::cout<<"  Failed scan: traces="<<traces<<", spread="<<spread
  <<"; eight unchanged callbacks: additional traces="<<f.traceCalls-traces<<", spread="<<f.spreadCalls-spread<<'\n';
 // Native camera input and weapon state must invalidate a repeated attempt.
 f.context.angles.y+=1;Invoke();Check(f.targetScans==scans+1&&f.traceCalls>traces,
  "Same-command mouse update reused a failed target scan");
 const auto before=f.targetScans;f.context.weapon.inaccuracy+=.01f;Invoke();
 Check(f.targetScans==before+1,"Changed weapon accuracy reused a failed target scan");
 const auto beforeEye=f.targetScans;f.context.eye.x+=.1f;Invoke();
 Check(f.targetScans==beforeEye+1,"Changed local shoot eye reused a failed target scan");
 const auto beforeTime=f.targetScans;f.context.shotTiming->fraction+=.125f;Invoke();
 Check(f.targetScans==beforeTime+1,"Changed shot history fraction reused a failed target scan");
 const auto beforeSetting=f.targetScans;setting=hvh::Get();setting.minimumDamage+=1;hvh::Save(setting);Invoke();
 Check(f.targetScans==beforeSetting+1,"Changed targeting settings reused a failed scan");
}
void LivePoseInvalidation(){
 for(int change=0;change<9;++change){
  Fixture f;Configure(f);f.spreadAmount=.3f;Invoke();const auto scans=f.targetScans;
  auto &enemy=*f.enemies[0];std::array<systems::bones::data,128> replacement=enemy.bones;
  std::array<std::byte,0x200> model=enemy.model;
  switch(change){
  case 0: enemy.Move({200,2,64});break;
  case 1: enemy.bones[0].position.y+=1;break;
  case 2: enemy.bones[0].rotation.z=.01f;break;
  case 3: Put(enemy.scene,0x180,reinterpret_cast<Address>(replacement.data()));break;
  case 4: enemy.modelPointer=At(model);break;
  case 5: Put(enemy.scene,0x194,std::uint16_t{127});break;
  case 6: enemy.target.handle+=0x8000;f.Set(f.page,10*0x70+0x10,enemy.target.handle);break;
  case 7: Put(enemy.pawn,0x80,true);break;
  case 8: Put(enemy.model,0x178,129);break;
  }
  Invoke();Check(f.targetScans==scans+1,"Remote pose, bone, resource, full handle or eligibility change reused a failed scan");
 }
 for(int phase=0;phase<3;++phase){Fixture f;Configure(f);bool changed=false;
  const auto change=[&]{if(!changed){changed=true;f.enemies[0]->Move({200,60,64});}};
  if(phase==0)f.onTrace=change;else if(phase==1)f.onSpread=change;else f.onGeometry=change;
  Invoke();Check(f.shots==1&&f.shotAngles.y>10&&f.traceCalls<=48&&f.spreadCalls<=128,
   "One remote pose change failed bounded refresh or committed an old target angle");
  std::cout<<"  Pose change during "<<(phase==0?"trace":phase==1?"spread":"capture")<<": scans="<<f.targetScans
   <<", traces="<<f.traceCalls<<", spread="<<f.spreadCalls<<", committed yaw="<<f.shotAngles.y<<'\n';
 }
 Fixture unstable;Configure(unstable);unstable.onTrace=[&]{unstable.enemies[0]->Move({200,60,64});};Invoke();
 Check(!unstable.shots&&!unstable.preparations,"Continuously changing remote pose authorized a stale shot or stop");
 Fixture capture;Configure(capture);capture.onGeometry=[&]{capture.enemies[0]->Move({200,60,64});};Invoke();
 Check(!capture.traceCalls&&!capture.shots,"Remote source changed during native geometry capture but reached tracing");
}
void CooldownPreparation(){
 Fixture f;Configure(f);f.spreadAmount=.3f;
 auto setting=hvh::Get();setting.autoStop=true;hvh::Save(setting);Invoke();
 Check(f.preparations==1&&!f.shots,"Cooldown fixture did not acquire an accuracy-limited target");
 const auto scans=f.targetScans,traces=f.traceCalls,spread=f.spreadCalls;
 f.Next(false);Invoke();
 Check(f.preparations==2&&f.preparationAllowed&&!f.preparationCommitted&&f.targetScans==scans&&
  f.traceCalls==traces&&f.spreadCalls==spread,"Qualified cooldown preparation rescanned or failed to brake before readiness");
 f.enemies[0]->Move({200,20,64});f.Next(false);Invoke();
 Check(f.preparations==3&&!f.shots&&f.targetScans==scans,
  "Recent nearby target movement lost cheap cooldown preparation");
 f.enemies[0]->Move({200,120,64});f.Next(false);Invoke();
 Check(f.preparations==3&&!f.shots,"Distant moved target retained a stale cooldown stopping acquisition");
 for(bool missing:{false,true}){Fixture invalid;Configure(invalid);
  setting=hvh::Get();setting.autoStop=true;hvh::Save(setting);
  if(missing)invalid.enemies.clear();else invalid.enemies[0]->damage=1;
  Invoke();invalid.Next(false);Invoke();
  Check(!invalid.preparations&&!invalid.shots,"Missing or below-damage target caused cooldown auto-stop");
 }
 Fixture expired;Configure(expired);expired.spreadAmount=.3f;setting=hvh::Get();setting.autoStop=true;hvh::Save(setting);Invoke();
 const auto before=expired.preparations;expired.now+=1000;expired.Next(false);Invoke();
 Check(expired.preparations==before,"Expired acquisition continued stopping during cooldown");
}
void ScopeBeforeAccuracy(){
 Fixture f;Configure(f);f.scopeable=true;f.context.weapon.definition=9;f.spreadAmount=.3f;
 auto setting=hvh::Get();setting.autoScope=true;setting.autoStop=true;hvh::Save(setting);Invoke();
 Check(f.scopes==1&&!f.shots&&!f.spreadCalls&&f.preparations==1&&f.preparationAllowed&&!f.preparationCommitted,
  "Qualified unscoped target performed costly hit-chance work before scoping");
 std::cout<<"  Early scope: traces="<<f.traceCalls<<", spread="<<f.spreadCalls<<'\n';
 f.Next();f.spreadAmount=0;Invoke();
 Check(f.shots==1&&f.scopes==1&&f.spreadCalls<=128,"Scoped firearm did not fire promptly on the next ready command");
}
void CrowdedFairBudget(){
 Fixture f;Configure(f);f.enemies[0]->SetGeometry(20,{200,0,64});f.enemies[0]->reachable=false;
 for(int index=1;index<8;++index){auto &enemy=f.AddEnemy({200,static_cast<float>(index*4),64});
  enemy.SetGeometry(20,{200,static_cast<float>(index*4),64});enemy.reachable=index==7;}
 Invoke();const auto last=f.enemies.back()->target.handle;
 Check(f.shots==1&&std::find(f.tracedTargets.begin(),f.tracedTargets.end(),last)!=f.tracedTargets.end(),
  "Closest target's multipoints starved a later reachable enemy within the trace budget");
 Check(f.traceCalls<=48&&f.spreadCalls<=128,"Crowded production scan exceeded native trace or sample budget");
 std::cout<<"  Crowded scan: traces="<<f.traceCalls<<", spread="<<f.spreadCalls<<'\n';
 Fixture rejected;Configure(rejected);rejected.spreadAmount=.3f;
 for(int index=1;index<8;++index)rejected.AddEnemy({200,static_cast<float>(index*4),64}).SetGeometry(20,{200,static_cast<float>(index*4),64});
 rejected.enemies[0]->SetGeometry(20,{200,0,64});Invoke();
 Check(!rejected.shots&&rejected.traceCalls<=48&&rejected.spreadCalls<=128,
  "Many accuracy-limited targets exceeded the command's shared native sample budget");
 std::cout<<"  Crowded rejected scan: traces="<<rejected.traceCalls<<", spread="<<rejected.spreadCalls<<'\n';
}
void NativeSeedWorkAndFallback(){
 Fixture fixed;Configure(fixed);fixed.privateSeed=false;fixed.spreadAmount=.01f;
 auto setting=hvh::Get();setting.noSpread=true;hvh::Save(setting);Invoke();
 Check(fixed.shots==1&&fixed.seedCalls&&fixed.seedCalls<=64&&fixed.spreadCalls==1,
  "Converged exact correction repeated one native seed sample across points");
 std::cout<<"  Exact correction: hashes="<<fixed.seedCalls<<", spread="<<fixed.spreadCalls<<'\n';
 Fixture failed;Configure(failed);failed.privateSeed=false;
 setting=hvh::Get();setting.noSpread=true;hvh::Save(setting);
 failed.seedFunction=[](Vec3,int)->std::optional<std::uint32_t>{return {};};Invoke();
 Check(failed.shots==1&&failed.seedCalls&&failed.spreadCalls==128,
  "Unavailable exact seed stalled an ordinary high-confidence shot");
 std::cout<<"  Unavailable exact seed fallback: hashes="<<failed.seedCalls<<", spread="<<failed.spreadCalls<<'\n';
 Fixture bounded;Configure(bounded);bounded.privateSeed=false;
 for(int index=1;index<8;++index)bounded.AddEnemy({200,static_cast<float>(index*4),64});
 setting=hvh::Get();setting.noSpread=true;hvh::Save(setting);
 bounded.seedFunction=[&](Vec3,int)->std::optional<std::uint32_t>{return bounded.seedCalls;};Invoke();
 Check(bounded.shots==1&&bounded.seedCalls>0&&bounded.seedCalls<=64&&bounded.spreadCalls<=192&&bounded.traceCalls==1,
  "Nonconvergent native seed search exceeded its shared work budget or stalled fallback");
 std::cout<<"  Nonconvergent solver fallback: hashes="<<bounded.seedCalls<<", spread="<<bounded.spreadCalls<<'\n';
 Fixture exhausted;Configure(exhausted);exhausted.privateSeed=false;exhausted.spreadAmount=.3f;
 for(int index=1;index<8;++index)exhausted.AddEnemy({200,static_cast<float>(index*4),64});
 setting=hvh::Get();setting.noSpread=true;hvh::Save(setting);
 exhausted.seedFunction=[&](Vec3,int)->std::optional<std::uint32_t>{return exhausted.seedCalls;};Invoke();
 Check(!exhausted.shots&&exhausted.seedCalls==64&&exhausted.spreadCalls<=192&&exhausted.traceCalls<=48,
  "Multiple rejected candidates exceeded the shared exact-seed work budget");
 for(bool shotgun:{false,true}){Fixture privateServer;Configure(privateServer);
  setting=hvh::Get();setting.noSpread=true;hvh::Save(setting);
  privateServer.privateSeed=!shotgun;if(shotgun)privateServer.context.weapon.bullets=8;
  Invoke();Check(privateServer.shots==1&&!privateServer.seedCalls&&privateServer.spreadCalls<=128,
   "Private-seed or multi-pellet weapon entered exact seed compensation");
 }
 Fixture missing;Configure(missing);missing.missingSpread=true;
 setting=hvh::Get();setting.autoStop=true;hvh::Save(setting);Invoke();
 Check(!missing.shots&&!missing.preparations&&missing.spreadCalls==1,
  "Unavailable ordinary spread was resampled or authorized a hit-chance stopping request");
}
void PredictionAndFinalSeed(){
 Fixture exact;Configure(exact);exact.privateSeed=false;exact.spreadAmount=.01f;
 auto setting=hvh::Get();setting.noSpread=true;setting.showSpreadPrediction=true;hvh::Save(setting);Invoke();
 auto prediction=hvh::Prediction();
 Check(exact.shots==1 && prediction && prediction->exact && prediction->compensated &&
       prediction->exact->seed==0 && prediction->exact->tick==exact.context.shotTiming->tick &&
       exact.spreadCalls==1 && exact.seedCalls==3,
       "Committed correction did not publish the final seed-verified ray or reused the sampler inefficiently");
 exact.now+=151;Check(!hvh::Prediction(),"An expired predicted shot remained visible");

 Fixture changed;Configure(changed);changed.privateSeed=false;changed.spreadAmount=.01f;
 setting=hvh::Get();setting.noSpread=true;hvh::Save(setting);
 changed.seedFunction=[&](Vec3,int)->std::optional<std::uint32_t>{return changed.seedCalls<3?0:1;};
 Invoke();Check(changed.shots==1 && changed.seedCalls==3 && changed.shotAngles.z==0 && changed.spreadCalls==130,
       "A changed final command seed retained compensation or stalled the ordinary hit-chance fallback");

 Fixture manual;Configure(manual);manual.privateSeed=false;manual.spreadAmount=.05f;manual.context.weapon.spread=.05f;
 setting=hvh::Get();setting.enabled=false;setting.showSpreadPrediction=true;hvh::Save(setting);Invoke();
 prediction=hvh::Prediction();
 Check(prediction && prediction->exact && !prediction->compensated && !manual.shots &&
       !manual.traceCalls && manual.seedCalls==1 && manual.spreadCalls==1 && prediction->exact->direction.y<0,
       "Independent manual marker changed firing or failed to predict actual spread");
 manual.privateSeed=true;manual.Next();Invoke();prediction=hvh::Prediction();
 Check(prediction && !prediction->exact && std::abs(prediction->radius-.05f)<1e-6f && manual.seedCalls==1,
       "Private seed produced an exact ray or discarded the uncertainty envelope");
 manual.privateSeed=false;manual.context.weapon.bullets=8;manual.Next();Invoke();prediction=hvh::Prediction();
 Check(prediction && !prediction->exact && manual.seedCalls==1,"A shotgun marker claimed an exact single-bullet impact");
 manual.context.accuracyValid=false;manual.Next();Invoke();Check(!hvh::Prediction(),"Invalid weapon accuracy produced a marker");
 manual.context.accuracyValid=true;manual.context.weapon.bullets=1;manual.Next();Invoke();
 Check(hvh::Prediction().has_value(),"Valid accuracy did not restore the marker");
 manual.focused=false;Invoke();Check(!hvh::Prediction(),"Unfocused gameplay retained a predicted shot");
 manual.focused=true;manual.Next();Invoke();hvh_input::LevelChanged();
 Check(!hvh::Prediction(),"A map epoch change retained an old spread marker");
}
void NativeReadinessAndActivation(){
 Fixture manual;Configure(manual);auto setting=hvh::Get();setting.autoFire=false;hvh::Save(setting);Invoke();
 Check(!manual.traceCalls&&!manual.spreadCalls&&!manual.shots,"Released manual primary initiated automatic scan");
 manual.held=true;Invoke();Check(manual.shots==1,"Held manual primary did not initiate a ready shot");
 Fixture focus;Configure(focus);focus.focused=false;Invoke();Check(!focus.traceCalls&&!focus.shots,"Unfocused input initiated a scan");
 focus.focused=true;Invoke();Check(focus.shots==1,"Restored gameplay focus did not resume scan scheduling");
 Fixture cooldown;Configure(cooldown);cooldown.context.canShoot=false;Invoke();
 Check(!cooldown.traceCalls&&!cooldown.spreadCalls&&!cooldown.preparations&&!cooldown.shots,
  "Cooldown without a qualified acquisition spent scan work or stopped movement");
 Fixture revolver;Configure(revolver);revolver.context.weapon.definition=64;Invoke();
 Check(!revolver.traceCalls&&!revolver.spreadCalls&&!revolver.shots,"Unsupported revolver cocking entered ordinary shot path");
 for(int change=0;change<4;++change){Fixture stale;Configure(stale);bool changed=false;
  const auto invalidate=[&]{if(changed)return;changed=true;
   if(change==0)stale.context.canShoot=false;
   if(change==1)++stale.context.commandNumber;
   if(change==2)hvh::menuOpen=true;
   if(change==3)stale.Set(stale.page,10*0x70+0x10,std::uint32_t{0x1000a});
  };
  if(change==1)stale.onSpread=invalidate;else stale.onTrace=invalidate;
  Invoke();Check(!stale.shots,"Readiness, command, focus or full target identity changed during a scan but committed");
 }
}
void HistoricalTraceBudget(){
 Fixture f;Configure(f);f.spreadAmount=.3f;
 auto setting=hvh::Get();setting.multipoints=false;hvh::Save(setting);
 for(int index=1;index<8;++index)f.AddEnemy({200,static_cast<float>(index*4),64});
 for(auto &enemy:f.enemies)for(int pose=0;pose<7;++pose){auto record=std::make_shared<backtrack::Record>();
  record->pawn=enemy->target.pawn;record->handle=enemy->target.handle;record->target=enemy->target;
  record->simulationTime=18.f+pose*.01f;record->bones.assign(enemy->bones.begin(),enemy->bones.end());
  for(auto &bone:record->bones)bone.position.y+=static_cast<float>(pose+1);
  f.history.push_back(std::move(record));
 }
 Invoke();Check(!f.shots&&f.historicalTraces&&f.traceCalls<=48&&f.spreadCalls<=128,
  "Historical expansion did not share the production scan's native trace and ordinary sample limits");
 std::cout<<"  Historical scan: traces="<<f.traceCalls<<", historical="<<f.historicalTraces<<", spread="<<f.spreadCalls<<'\n';
 Fixture lethal;Configure(lethal);lethal.enemies[0]->damage=100;
 auto record=std::make_shared<backtrack::Record>();record->pawn=lethal.enemies[0]->target.pawn;
 record->handle=lethal.enemies[0]->target.handle;record->target=lethal.enemies[0]->target;
 record->bones.assign(lethal.enemies[0]->bones.begin(),lethal.enemies[0]->bones.end());lethal.history.push_back(record);
 Invoke();Check(lethal.shots==1&&lethal.traceCalls==1&&!lethal.historicalTraces&&!lethal.shotTargetTiming,
  "Reliable lethal live shot waited for historical points or committed recorded timing");
}
void TransientAndMembershipRecovery(){
 Fixture samples;Configure(samples);samples.missingSpread=true;Invoke();
 Check(!samples.shots&&samples.spreadCalls,"Transient spread failure did not reach the sampling boundary");
 const auto beforeSamples=samples.targetScans;samples.missingSpread=false;Invoke();
 Check(samples.targetScans==beforeSamples+1&&samples.shots==1,"Transient native spread failure cached an unchanged command permanently");
 auto setting=hvh::Get();
 for(bool braking:{false,true}) {
  Fixture seeds;Configure(seeds);seeds.spreadAmount=.3f;seeds.privateSeed=false;
  setting=hvh::Get();setting.noSpread=true;setting.autoStop=braking;hvh::Save(setting);
  seeds.seedFunction=[](Vec3,int)->std::optional<std::uint32_t>{return {};};Invoke();
  Check(!seeds.shots&&seeds.seedCalls&&seeds.preparations==static_cast<unsigned>(braking),
   "Transient exact seed failure did not retain valid immediate braking");
  const auto beforeSeeds=seeds.targetScans;seeds.seedFunction={};Invoke();
  Check(seeds.targetScans==beforeSeeds+1&&seeds.shots==1,
   "Transient native seed failure blocked a corrected same-command shot after braking");
 }
 Fixture geometry;Configure(geometry);geometry.enemies[0]->target.hitboxes.count=0;Invoke();
 Check(!geometry.shots&&!geometry.traceCalls,"Missing native geometry reached a qualified shot");
 const auto beforeGeometry=geometry.targetScans;geometry.enemies[0]->SetGeometry(1,{200,0,64});Invoke();
 Check(geometry.targetScans==beforeGeometry+1&&geometry.shots==1,"Restored native geometry remained hidden by a failed scan cache");
 Fixture partial;Configure(partial);partial.spreadAmount=.3f;partial.enemies[0]->SetGeometry(2,{200,0,64});
 partial.enemies[0]->bones[1].scale=NAN;Invoke();Check(!partial.shots&&partial.traceCalls,"Partial native pose did not exercise valid remaining geometry");
 const auto beforePartial=partial.targetScans;partial.enemies[0]->bones[1].scale=1;Invoke();
 Check(partial.targetScans==beforePartial+1,"Restored partial native bone geometry remained hidden by a failed scan cache");
 Fixture scope;Configure(scope);scope.scopeable=true;scope.scopeSucceeds=false;scope.spreadAmount=.3f;
 setting=hvh::Get();setting.autoScope=true;hvh::Save(setting);Invoke();
 Check(!scope.shots&&scope.scopes&&scope.spreadCalls,"Transient native scope failure did not exercise its hit-chance continuation");
 const auto beforeScope=scope.targetScans,spreadBeforeScope=scope.spreadCalls;scope.scopeSucceeds=true;Invoke();
 Check(scope.targetScans==beforeScope+1&&scope.scoped&&scope.spreadCalls==spreadBeforeScope,
  "Recovered native scope transaction remained blocked by same-command failed hit chance");
 Fixture targets;Configure(targets);targets.spreadAmount=.3f;Invoke();const auto beforeSpawn=targets.targetScans;
 targets.AddEnemy({200,4,64});Invoke();
 Check(targets.targetScans==beforeSpawn+1,"New live target did not invalidate same-command scan membership");
 Fixture eligible;Configure(eligible);eligible.spreadAmount=.3f;
 auto &protectedEnemy=eligible.AddEnemy({200,4,64});protectedEnemy.enumerated=false;Put(protectedEnemy.pawn,0x80,true);
 Invoke();const auto beforeEligibility=eligible.targetScans;protectedEnemy.enumerated=true;Put(protectedEnemy.pawn,0x80,false);Invoke();
 Check(eligible.targetScans==beforeEligibility+1,"Newly eligible existing pawn did not invalidate same-command scan membership");
 Fixture trace;Configure(trace);bool broken=true;trace.onTrace=[&]{if(broken)trace.enemies[0]->target.pawn=0;};
 Invoke();Check(!trace.shots,"Invalid trace target produced a shot");
 trace.enemies[0]->target.pawn=At(trace.enemies[0]->pawn);broken=false;const auto beforeTrace=trace.targetScans;Invoke();
 Check(trace.targetScans==beforeTrace+1&&trace.shots==1,"Transient native trace failure blocked a corrected same-command retry");
}
void DirectionAndActionCacheKeys(){
 Fixture f;Configure(f);f.spreadAmount=.3f;Invoke();
 const auto scans=f.targetScans,traces=f.traceCalls,spread=f.spreadCalls;
 constexpr std::uint64_t directions=(1ull<<3)|(1ull<<4)|(1ull<<9)|(1ull<<10);
 for(const auto rewritten:{std::uint64_t{0},std::uint64_t{1}<<4,std::uint64_t{1}<<9,std::uint64_t{1}<<10}){
  auto buttons=memory::safe_read<ButtonState>(At(f.command)+kCommandButtons).value();
  buttons.value=(buttons.value&~directions)|rewritten;
  buttons.changed=(buttons.changed&~directions)|directions;
  buttons.scroll=(buttons.scroll&~directions)|rewritten;
  Put(f.command,kCommandButtons,buttons);Invoke();
 }
 Check(f.targetScans==scans&&f.traceCalls==traces&&f.spreadCalls==spread,
  "Direction-only raw movement rewrites invalidated an otherwise unchanged shot scan");
 std::cout<<"  Four direction-only rewrites: additional traces="<<f.traceCalls-traces<<", spread="<<f.spreadCalls-spread<<'\n';
 for(const auto action:{kAttack,1ull<<1,1ull<<5,1ull<<11})for(int member=0;member<3;++member){
  Fixture actionInput;Configure(actionInput);actionInput.spreadAmount=.3f;Invoke();const auto before=actionInput.targetScans;
  auto buttons=memory::safe_read<ButtonState>(At(actionInput.command)+kCommandButtons).value();
  if(member==0)buttons.value^=action;
  if(member==1)buttons.changed^=action;
  if(member==2)buttons.scroll^=action;
  Put(actionInput.command,kCommandButtons,buttons);Invoke();
  Check(actionInput.targetScans==before+1,"Primary, secondary, use or jump action change reused a failed shot scan");
 }
}
void FailedCommitAndScopeReadinessRecovery(){
 Fixture shot;Configure(shot);shot.shotSucceeds=false;Invoke();
 Check(!shot.shots&&shot.shotAttempts==1,"Failed shot transaction fixture did not attempt one qualified commit");
 const auto beforeShot=shot.targetScans;shot.shotSucceeds=true;Invoke();
 Check(shot.targetScans==beforeShot+1&&shot.shotAttempts==2&&shot.shots==1,
  "Failed ApplyShot permanently blocked a corrected same-command commit");
 Fixture scope;Configure(scope);scope.scopeable=true;scope.spreadAmount=.3f;
 auto setting=hvh::Get();setting.autoScope=true;hvh::Save(setting);
 scope.onTrace=[&]{scope.context.canShoot=false;};Invoke();
 Check(!scope.shots&&!scope.scopes&&scope.scopeQueries&&scope.spreadCalls,
  "Scope-readiness fixture did not exercise a qualified unscoped target with failed readiness");
 const auto beforeScope=scope.targetScans,spreadBeforeScope=scope.spreadCalls;
 scope.onTrace={};scope.context.canShoot=true;Invoke();
 Check(scope.targetScans==beforeScope+1&&scope.scopes==1&&scope.scoped&&scope.spreadCalls==spreadBeforeScope,
  "Recovered ReadyForShot left an unscoped target blocked by unchanged failed hit chance");
}
} // namespace scan_fixture

namespace memory {
bool copy(void* destination,std::uintptr_t source,std::size_t size)noexcept{return destination&&game_access::Copy(source,destination,size);}
bool store(std::uintptr_t destination,const void* source,std::size_t size)noexcept{return game_access::Store(destination,source,size);}
}
namespace math {
quaternion::quaternion() noexcept:x(0),y(0),z(0),w(1){}
constexpr quaternion::quaternion(float a,float b,float c,float d) noexcept:x(a),y(b),z(c),w(d){}
vector3 vector3::operator+(const vector3& v)const noexcept{return {x+v.x,y+v.y,z+v.z};}
vector3 vector3::operator-(const vector3& v)const noexcept{return {x-v.x,y-v.y,z-v.z};}
vector3 vector3::operator*(float scalar)const noexcept{return {x*scalar,y*scalar,z*scalar};}
float vector3::dot(const vector3& v)const noexcept{return x*v.x+y*v.y+z*v.z;}
float vector3::length_sqr()const noexcept{return dot(*this);}
float vector3::length()const noexcept{return std::sqrt(length_sqr());}
vector3 quaternion::rotate_vector(const vector3& v)const noexcept{
 const vector3 axis{x,y,z};const auto twice=vector3{axis.y*v.z-axis.z*v.y,axis.z*v.x-axis.x*v.z,axis.x*v.y-axis.y*v.x}*2;
 return v+twice*w+vector3{axis.y*twice.z-axis.z*twice.y,axis.z*twice.x-axis.x*twice.z,axis.x*twice.y-axis.y*twice.x};
}
}
namespace systems {
std::uint32_t schemas::lookup(const char* owner,std::uint32_t hash){
 for(const auto &field:scan_fixture::fields)if(std::string_view(owner)==field.owner&&hash==fnv1a::runtime_hash(field.name))return field.offset;return 0;
}
bones::data bones::get(std::uintptr_t pawn,std::uint32_t bone){++scan_fixture::active->boneReads;
 const auto enemy=scan_fixture::active->Find(pawn);return enemy&&bone<enemy->bones.size()?enemy->bones[bone]:data{};}
std::array<bones::data,27> bones::get_skeleton(std::uintptr_t pawn){return get_skeleton(pawn,true);}
std::array<bones::data,27> bones::get_skeleton(std::uintptr_t,bool){++scan_fixture::active->skeletonReads;return {};}
}
namespace combat_access {
using namespace scan_fixture;
bool InitializeInput(){return true;}bool Initialize(){return true;}const char* BindingStatus(){return "Combat bindings connected.";}
Address CreateMoveTarget(){return 0;}Address HandleViewAnglesTarget(){return 0;}
Context ReadContext(Address input,bool){++active->contexts;Check(input==active->context.input,"Scan used wrong input");return active->context;}
std::optional<CommandIdentity> ReadInputCommand(Address input){if(input!=active->context.input)return {};return CommandIdentity{
 active->context.command,active->context.commandNumber,active->context.local.handle,active->overwrite,false};}
bool ReadViewAngles(Address,Vec3& angles){angles=active->context.angles;return true;}
bool WriteViewAngles(Address,const Vec3&){return true;}
OrientationContext ReadOrientationContext(Address,const CommittedShot*){return {};}
std::optional<OrientationTarget> ReadOrientationTarget(const OrientationContext&,Address,std::uint32_t){return {};}
bool ApplyOrientation(const OrientationContext&,const Vec3&,const OrientationTarget*){return false;}
std::vector<Target> ReadTargets(const Context&,TargetScan* scan,bool geometry){++active->targetEnumerations;if(scan)++active->targetScans;std::vector<Target> result;
 for(const auto &enemy:active->enemies){if(!enemy->enumerated)continue;auto target=enemy->target;if(!geometry)target.hitboxes={};result.push_back(target);}
 if(scan){const auto count=static_cast<unsigned>(result.size());*scan={count,count,count,count,geometry?count:0,geometry?count:0};}return result;}
void ResetTargets(){if(active)++active->targetResets;}
Hitboxes ReadHitboxes(Address scene,bool){++active->geometryReads;if(active->onGeometry)active->onGeometry();const auto enemy=active->Find(scene);return enemy?enemy->target.hitboxes:Hitboxes{};}
bool ReadyForShot(const Context& context){return context.valid&&context.canShoot&&active->context.valid&&active->context.canShoot&&
 context.commandNumber==active->context.commandNumber&&context.weapon.handle==active->context.weapon.handle;}
bool AttackDown(const Context&){return active->held;}
std::optional<bool> IsScoped(const Context&){++active->scopeQueries;return active->scopeable?std::optional{active->scoped}:std::nullopt;}
bool Scope(const Context&){++active->scopes;if(!active->scopeSucceeds)return false;active->scoped=true;return true;}
std::optional<combat_bindings::HistoryShotTiming> ShotTiming(const Context& context){return context.shotTiming;}
bool ApplyShot(const Context& context,const Vec3& angles,HistoryShotTiming,std::optional<BacktrackTiming> targetTiming){
 ++active->shotAttempts;if(!active->shotSucceeds||!ReadyForShot(context))return false;
 ++active->shots;active->shotAngles=angles;active->shotTargetTiming=targetTiming;return true;}
math::vector2 Spread(const Weapon&,unsigned seed){++active->spreadCalls;if(active->onSpread)active->onSpread();
 const auto amount=active->mixedSpread?(seed<96?0.f:.3f):active->spreadAmount;
 return active->missingSpread?math::vector2{NAN,NAN}:math::vector2{amount,amount};}
std::optional<std::uint32_t> SpreadSeed(const Vec3& angles,int tick){++active->seedCalls;
 return active->seedFunction?active->seedFunction(angles,tick):std::optional<std::uint32_t>{0};}
std::optional<bool> ConvarBool(const char*){return active->privateSeed;}
bool Finite(const Vec3& v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
int Hitgroup(int){return 1;}
}
namespace autowall_probe {
bool Initialize(){return true;}bool Reset(){return true;}const char* ReasonText(Reason){return "Fixture penetration result.";}
TargetResult EvaluateTarget(game_access::Address pawn,std::uint32_t handle,const Vec3& point,int group,
 std::optional<combat_bindings::HistoryShotTiming>){
 using namespace scan_fixture;++active->traceCalls;active->lastTraceTarget=handle;active->tracedTargets.push_back(handle);
 if(active->onTrace)active->onTrace();const auto enemy=active->Find(pawn);TargetResult result;
 result.valid=enemy!=nullptr;result.reached=enemy&&enemy->reachable;result.penetrated=enemy&&enemy->penetrated;result.rawDamage=result.damage=enemy?enemy->damage:0;
 result.pawn=pawn;result.targetHandle=handle;result.weaponHandle=active->context.weapon.handle;result.hitgroup=group;result.position=point;
 result.eyeOrigin=active->context.eye;result.localHandle=active->context.local.handle;result.reason=Reason::TargetReached;return result;
}
TargetResult EvaluateHistoricalTarget(game_access::Address pawn,std::uint32_t handle,const Vec3& point,
 combat_bindings::HistoryShotTiming timing,int group){++scan_fixture::active->historicalTraces;return EvaluateTarget(pawn,handle,point,group,timing);}
}
namespace movement {
std::optional<CameraStamp> Process(std::uintptr_t,bool){++scan_fixture::active->movementCalls;return {};}
std::optional<CameraStamp> PrepareShot(std::uintptr_t,bool allowHeldPrimary,bool shotCommitted){
 auto &f=*scan_fixture::active;++f.preparations;f.preparationAllowed=allowHeldPrimary;f.preparationCommitted=shotCommitted;
 return shotCommitted?std::nullopt:std::optional{CameraStamp{f.context.command,f.context.commandNumber,f.context.local.handle}};
}
void FinishCommand(std::uintptr_t){++scan_fixture::active->finalizations;}void Reset(){}void SetInputState(bool){}void SetPipelineStatus(const char*){}
}
namespace backtrack {
std::vector<Snapshot> Records(game_access::Address pawn,std::uint32_t handle){std::vector<Snapshot> result;
 for(const auto &record:scan_fixture::active->history)if(record->pawn==pawn&&record->handle==handle)result.push_back(record);return result;}
bool Current(const Record&){return scan_fixture::active->historyCurrent;}
std::optional<combat_bindings::BacktrackTiming> Timing(const Record& record){return combat_bindings::BacktrackTiming{{1200,0},static_cast<int>(record.handle&0x7fff)};}
std::optional<math::vector3> Intersection(const Record&,const math::vector3&,const math::vector3& point){return point;}
}
extern "C" MH_STATUS WINAPI MH_CreateHook(LPVOID,LPVOID,LPVOID*){return MH_ERROR_UNSUPPORTED_FUNCTION;}
extern "C" MH_STATUS WINAPI MH_EnableHook(LPVOID){return MH_OK;}
extern "C" MH_STATUS WINAPI MH_DisableHook(LPVOID){return MH_OK;}

int main(){try{scan_fixture::BasicProductionShot();scan_fixture::PromptQualifiedLiveShot();scan_fixture::FailedChanceScheduling();scan_fixture::LivePoseInvalidation();
 scan_fixture::CooldownPreparation();scan_fixture::ScopeBeforeAccuracy();scan_fixture::CrowdedFairBudget();
 scan_fixture::NativeSeedWorkAndFallback();scan_fixture::NativeReadinessAndActivation();scan_fixture::HistoricalTraceBudget();
 scan_fixture::PredictionAndFinalSeed();
 scan_fixture::TransientAndMembershipRecovery();
 scan_fixture::DirectionAndActionCacheKeys();scan_fixture::FailedCommitAndScopeReadinessRecovery();
 std::cout<<"HVH production scan regression: PASS (real CreateMove/ProcessShot, lazy spread and trace budgets, repeated-command invalidation, live pose freshness, early scope, fair centers and cooldown preparation)\n";return 0;}
 catch(const std::exception& error){std::cerr<<"HVH production scan regression: FAIL: "<<error.what()<<'\n';return 1;}}
