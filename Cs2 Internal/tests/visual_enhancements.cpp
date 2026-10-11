#include "features/visuals/enhancements.cpp"
#include "features/visuals/gun_images.h"
#include "features/visuals/chams.cpp"
#include "features/visuals/chams_materials.cpp"
#include "game/sdk/ChamsBindings.h"
#include "features/visuals/detail/primitive_buffer.hpp"
#include "features/combat/detail/hvh_input.hpp"
#include <iostream>
#include <functional>
#include <limits>
#include <stdexcept>
void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
unsigned imageCalls{};
unsigned chamsCalls{}, materialCreates{}, materialDestroys{};
std::function<void()> chamsTransition;
std::vector<std::string> materialSources;
std::array<std::uintptr_t,static_cast<std::size_t>(visuals::chams::Material::Count)*2> materialBindings{};
cstypes::key_values3* __fastcall AllocateMaterial(cstypes::key_values3* tree,unsigned,unsigned){return tree;}
void __fastcall DestroyMaterial(cstypes::key_values3*,unsigned){++materialDestroys;}
bool __fastcall LoadMaterial(cstypes::key_values3* tree,const char*,const char* source,const cstypes::kv3_id*,const char*,unsigned){
 tree->payload=materialSources.size();materialSources.emplace_back(source);return true;
}
void* __fastcall CreateMaterial(std::uintptr_t,cstypes::strong_handle* handle,const char*,cstypes::key_values3* tree,int,bool){
 const auto index=static_cast<std::size_t>(tree->payload);
 Check(index<materialBindings.size(),"Material fixture overflow");
 materialBindings[index]=0x1000+index;
 handle->binding=&materialBindings[index];++materialCreates;return nullptr;
}
void __fastcall ChamsPrimitives(std::uintptr_t,std::uintptr_t scene,std::uintptr_t,std::uintptr_t address){
 using namespace visuals::chams::detail;
 auto& buffer=*reinterpret_cast<Output*>(address);
 auto* entries=reinterpret_cast<Primitive*>(buffer.fixed);
 Check(buffer.fixedCount+2<=buffer.fixedCapacity,"Chams fixture overflow");
 entries[buffer.fixedCount].scene=scene;entries[buffer.fixedCount].material=entries[buffer.fixedCount].materialCopy=23;
 entries[buffer.fixedCount+1].scene=scene+1;entries[buffer.fixedCount+1].material=entries[buffer.fixedCount+1].materialCopy=42;
 buffer.fixedCount+=2;++chamsCalls;
 if(chamsTransition)chamsTransition();
}
void ChamsStartupAndDisable(){
 using namespace visuals::chams;
 using namespace visuals::chams::detail;
 namespace material=visuals::chams::materials;
 const auto surfaces=visuals::penetration_overlay::Get();auto baseline=surfaces;baseline.enabled=false;
 visuals::penetration_overlay::Save(baseline);
 const auto enemy=GetConfiguration(Target::Enemy),weapon=GetConfiguration(Target::ViewmodelWeapon);
 Check(enemy.enabled&&enemy.visible.enabled&&enemy.hidden.enabled&&weapon.enabled&&weapon.visible.enabled&&!weapon.hidden.enabled,
  "Requested chams are not active at startup");
 for(const auto target:{Target::Teammate,Target::Local,Target::EnemyRagdoll,Target::TeamRagdoll,Target::LocalRagdoll,Target::Arms,Target::Weapon})
  Check(!GetConfiguration(target).enabled,"Unrequested chams are active at startup");
 material::api={reinterpret_cast<std::uintptr_t>(AllocateMaterial),reinterpret_cast<std::uintptr_t>(DestroyMaterial),
  reinterpret_cast<std::uintptr_t>(LoadMaterial),reinterpret_cast<std::uintptr_t>(CreateMaterial)};
 material::supported=true;visuals::chams::ready=true;
 Update(false);Check(!materialCreates,"Materials loaded outside a world");
 Update(true);Check(materialCreates==3&&materialDestroys==3,"Enabled defaults failed to preload all three material passes");
 Update(true);Check(materialCreates==3,"Material preload recreated cached resources");
 const auto hidden=material::Find(enemy.hidden.material,true),visible=material::Find(enemy.visible.material,false),gun=material::Find(weapon.visible.material,false);
 Check(hidden&&visible&&gun&&hidden!=visible&&visible!=gun,"Depth/style material slots were aliased");
 Check(materialSources[0].find("F_DISABLE_Z_BUFFERING = 1")!=std::string::npos&&materialSources[0].find("F_DISABLE_Z_WRITE = 1")!=std::string::npos,
  "Hidden silhouette still uses depth testing");
 Check(materialSources[1].find("generic.vfx")!=std::string::npos&&materialSources[1].find("unlitgeneric.vfx")==std::string::npos,
  "Visible model lost lit surface shading");
 Check(materialSources[2].find("csgo_character.vfx")!=std::string::npos&&materialSources[2].find("F_IRIDESCENCE = 1")!=std::string::npos,
  "Weapon no longer uses the lit iridescent material");
 std::array<std::byte,128> sceneBytes{};
 const auto scene=reinterpret_cast<std::uintptr_t>(sceneBytes.data());
 memory::safe_write(scene+chams_bindings::kSceneFlags,std::uint8_t{0xff});
 std::array<Primitive,5> entries{};entries[0].scene=scene;entries[0].material=17;
 Output buffer{reinterpret_cast<std::uintptr_t>(entries.data()),5,1};
 Check(Apply(enemy,0,scene,0,reinterpret_cast<std::uintptr_t>(&buffer),ChamsPrimitives),"Default enemy material passes failed");
 Check(chamsCalls==2&&entries[0].material==17&&entries[1].material==hidden&&entries[3].material==visible&&entries[2].material==42&&entries[4].material==42,
  "Enemy passes have the wrong order or affected unrelated primitives");
 const auto packed=[](visuals::color c){return c.r|(std::uint32_t{c.g}<<8)|(std::uint32_t{c.b}<<16)|(std::uint32_t{c.a}<<24);};
 Check(entries[1].tint==packed(enemy.hidden.tint)&&entries[3].tint==packed(enemy.visible.tint)&&enemy.hidden.tint.a<enemy.visible.tint.a,
  "Through-wall and lit visible tints did not reach their respective passes");
 Check(memory::read<std::uint8_t>(scene+chams_bindings::kSceneFlags)==0xff,"Enemy passes did not restore scene flags");
 buffer.fixedCount=1;chamsCalls=0;
 Check(Apply(weapon,0,scene,0,reinterpret_cast<std::uintptr_t>(&buffer),ChamsPrimitives)&&chamsCalls==1&&entries[1].material==gun,
  "Weapon emitted a through-wall or missing visible pass");
 const auto first=AnimatedTint(weapon.visible,-1.25),second=AnimatedTint(weapon.visible,1.25);
 Check(packed(first)!=packed(second)&&first.a==weapon.visible.tint.a&&second.a==first.a,"Weapon color wave did not preserve opacity");
 auto disabledEnemy=enemy,disabledWeapon=weapon;disabledEnemy.enabled=false;disabledWeapon.enabled=false;
 SetConfiguration(Target::Enemy,disabledEnemy);SetConfiguration(Target::ViewmodelWeapon,disabledWeapon);
 Check(!visuals::chams::anyEnabled,"Disabling both requested targets left the render fast-path active");
 chamsCalls=0;Check(!Apply(GetConfiguration(Target::Enemy),0,scene,0,reinterpret_cast<std::uintptr_t>(&buffer),ChamsPrimitives)&&!chamsCalls,
  "Disabled chams still generated primitives");
 Reset();Update(true);Check(!GetConfiguration(Target::Enemy).enabled&&!GetConfiguration(Target::ViewmodelWeapon).enabled&&materialCreates==3,
  "World reset reenabled user-disabled defaults");
 SetConfiguration(Target::Enemy,enemy);SetConfiguration(Target::ViewmodelWeapon,weapon);
 Check(visuals::chams::anyEnabled,"Reenabling requested targets did not restore the render fast-path");
 Shutdown();
 visuals::penetration_overlay::Save(surfaces);
}
void __fastcall ImagePrimitives(std::uintptr_t, std::uintptr_t scene, std::uintptr_t, std::uintptr_t buffer){
 using namespace visuals::chams::detail;
 auto *output=reinterpret_cast<Output*>(buffer);
 auto *entries=reinterpret_cast<Primitive*>(output->fixed);
 entries[1].scene=scene;entries[1].material=entries[1].materialCopy=23;
 entries[2].scene=scene+1;entries[2].material=entries[2].materialCopy=42;
 output->fixedCount=3;++imageCalls;
}

// Use the real schema reader, entity resolver, prop adapter and chams renderer.
// The native fields are backed by ordinary memory and callable schema vtables;
// the fixture never replaces Read/Remember/Generate with its own implementation.
struct PropSchemaFixture;
PropSchemaFixture* propSchema{};
struct PropSchemaFixture {
 using Address=std::uintptr_t;
 static constexpr std::uint32_t Node=0x150,Collision=0x160,Origin=0x50,Angles=0x64;
 std::array<Address,14> systemTable{};
 std::array<Address,3> scopeTable{};
 std::array<Address,1> system{},scope{};
 std::array<std::byte,0x40> entityClass{},nodeClass{};
 std::array<std::byte,0x80> entityFields{};
 std::array<std::byte,0x40> nodeFields{};
 Address previous{};
 template<class T> static void Write(Address object,std::size_t offset,T value){
  Check(memory::safe_write(object+offset,value),"Native prop fixture write failed");
 }
 template<class T> static Address At(T& object){return reinterpret_cast<Address>(object.data());}
 static Address __fastcall Scope(Address,const char* module,void*){
  Check(std::string_view(module)=="client.dll","Unexpected native schema module");return At(propSchema->scope);
 }
 static void __fastcall Class(Address,Address* out,const char* name){
  *out=std::string_view(name)=="C_BaseEntity"?At(propSchema->entityClass):
       std::string_view(name)=="CGameSceneNode"?At(propSchema->nodeClass):0;
 }
 void Field(Address fields,unsigned index,const char* name,std::uint32_t offset){
  Write(fields,index*0x20,reinterpret_cast<Address>(name));Write(fields,index*0x20+0x10,offset);
 }
 PropSchemaFixture(){
  previous=addresses::globals::schema_system;propSchema=this;
  systemTable[13]=reinterpret_cast<Address>(Scope);scopeTable[2]=reinterpret_cast<Address>(Class);
  system[0]=At(systemTable);scope[0]=At(scopeTable);
  Field(At(entityFields),0,"m_pGameSceneNode",Node);Field(At(entityFields),1,"m_pCollision",Collision);
  Field(At(entityFields),2,"m_iHealth",game_access::field::C_BaseEntity::m_iHealth);
  Field(At(entityFields),3,"m_iTeamNum",game_access::field::C_BaseEntity::m_iTeamNum);
  Field(At(nodeFields),0,"m_vecAbsOrigin",Origin);Field(At(nodeFields),1,"m_angAbsRotation",Angles);
  Write(At(entityClass),0x24,std::uint16_t{4});Write(At(entityClass),0x30,At(entityFields));
  Write(At(nodeClass),0x24,std::uint16_t{2});Write(At(nodeClass),0x30,At(nodeFields));
  addresses::globals::schema_system=At(system);
 }
 ~PropSchemaFixture(){addresses::globals::schema_system=previous;propSchema=nullptr;}
};

void PersistentPropFlatPass(){
 using namespace visuals::chams;
 using namespace visuals::chams::detail;
 namespace props=visuals::penetrable_props;
 namespace materials=visuals::chams::materials;
 using Native=PropSchemaFixture;
 using Address=Native::Address;
 Native schema;
 std::vector<std::byte> clientMemory(game_access::offset::dwEntityList+8);
 std::array<std::byte,0x210> list{};
 std::array<std::byte,512*0x70> page{};
 std::array<std::byte,0x400> prop{},replacement{},enemy{},pawn{},node{},otherNode{};
 std::array<std::byte,0x1000> controller{};
 std::array<std::byte,0x80> identity{},enemyIdentity{},entityClass{},classInfo{};
 std::array<std::byte,0x200> sceneMemory{},enemySceneMemory{},model{},otherModel{};
 std::array<std::byte,0x40> collision{},otherCollision{};
 std::array<Address,1> binding{},otherBinding{};
 const auto entity=Native::At(prop),scene=Native::At(sceneMemory),nodeAddress=Native::At(node),listAddress=Native::At(list);
 constexpr std::uint32_t propHandle=0x10002,enemyHandle=0x18003,localHandle=0x8001;
 const auto epoch=hvh_input::worldEpoch.load();
 const auto previousClient=visuals::chams::client;
 const auto previousSettings=visuals::penetration_overlay::Get();
 binding[0]=Native::At(model);otherBinding[0]=Native::At(otherModel);
 Native::Write(listAddress,0x10,Native::At(page));
 auto slot=[&](unsigned index,Address object,std::uint32_t handle){
  Native::Write(Native::At(page),index*0x70,object);Native::Write(Native::At(page),index*0x70+0x10,handle);
 };
 slot(1,Native::At(pawn),localHandle);slot(2,entity,propHandle);slot(3,Native::At(enemy),enemyHandle);
 Native::Write(entity,0x10,Native::At(identity));Native::Write(Native::At(identity),0x10,propHandle);
 Native::Write(entity,Native::Node,nodeAddress);Native::Write(entity,Native::Collision,Native::At(collision));
 const auto modelField=game_access::field::CSkeletonInstance::m_modelState+game_access::field::CModelState::m_hModel;
 Native::Write(nodeAddress,modelField,Native::At(binding));
 const math::vector3 origin{100,20,30},angles{0,25,0};
 Native::Write(nodeAddress,Native::Origin,origin);Native::Write(nodeAddress,Native::Angles,angles);
 Native::Write(scene,chams_bindings::kSceneOwner,propHandle);Native::Write(scene,chams_bindings::kSceneFlags,std::uint8_t{0xff});
 // A real local/hostile pawn permits the existing enemy path after the new
 // prop path declines that scene. This exercises coexistence through Generate.
 Native::Write(Native::At(clientMemory),game_access::offset::dwEntityList,listAddress);
 Native::Write(Native::At(clientMemory),game_access::offset::dwLocalPlayerController,Native::At(controller));
 Native::Write(Native::At(controller),game_access::field::CCSPlayerController::m_hPlayerPawn,localHandle);
 Native::Write(Native::At(pawn),game_access::field::C_BaseEntity::m_iHealth,100);
 Native::Write(Native::At(pawn),game_access::field::C_BaseEntity::m_iTeamNum,std::uint8_t{2});
 Native::Write(Native::At(enemy),game_access::field::C_BaseEntity::m_iHealth,100);
 Native::Write(Native::At(enemy),game_access::field::C_BaseEntity::m_iTeamNum,std::uint8_t{3});
 Native::Write(Native::At(enemy),0x10,Native::At(enemyIdentity));
 Native::Write(Native::At(enemyIdentity),0x10,enemyHandle);
 Native::Write(Native::At(enemyIdentity),8,Native::At(entityClass));
 Native::Write(Native::At(entityClass),0x58,Native::At(classInfo));
 Native::Write(Native::At(classInfo),8,reinterpret_cast<Address>("C_CSPlayerPawn"));
 Native::Write(Native::At(enemySceneMemory),chams_bindings::kSceneOwner,enemyHandle);
 Native::Write(Native::At(enemySceneMemory),chams_bindings::kSceneFlags,std::uint8_t{0xff});
 visuals::chams::client=Native::At(clientMemory);visuals::chams::ready=true;materials::supported=true;
 auto settings=previousSettings;settings.enabled=true;settings.props=true;settings.color={.2f,.7f,1.f,.8f};
 visuals::penetration_overlay::Save(settings);
 props::Clear();props::BeginMap(epoch,listAddress);
 autowall_probe::Result hit;
 hit.state=autowall_probe::State::Pass;hit.reason=autowall_probe::Reason::Penetrated;
 hit.kind=autowall_probe::SurfaceKind::Prop;hit.surface=true;hit.hitEntity=entity;hit.hitHandle=propHandle;
 hit.sceneNode=nodeAddress;hit.entityOrigin=origin;hit.entityRotation=angles;
 hit.physicsObject=Native::At(collision);hit.physicsShape=Native::At(model);
 hit.stamp.session=epoch;hit.stamp.list=listAddress;
 auto rejected=hit;rejected.state=autowall_probe::State::Blocked;props::Remember(rejected);
 rejected=hit;rejected.kind=autowall_probe::SurfaceKind::World;props::Remember(rejected);
 rejected=hit;rejected.surface=false;props::Remember(rejected);
 rejected=hit;rejected.hitHandle^=0x8000;props::Remember(rejected);
 Check(!props::Count(),"Unqualified or stale detection selected a prop");
 props::Remember(hit);Check(props::Count()==1,"Successful native detection was not retained");
 std::array<Primitive,5> primitives{};
 Output output{Native::At(primitives),5,1};
 auto resetOutput=[&]{primitives={};primitives[0].scene=scene;primitives[0].material=17;output.fixedCount=1;chamsCalls=0;};
 auto draw=[&]{return visuals::chams::Generate(0,scene,0,reinterpret_cast<Address>(&output),ChamsPrimitives);};
 // Newly confirmed props must wait for their real native material resource.
 resetOutput();Check(!props::Generate(0,scene,0,reinterpret_cast<Address>(&output),ChamsPrimitives,{1,2,3,4})&&!chamsCalls,
  "Prop emitted a fake or absent Flat material");
 Update(true);
 const auto flat=materials::Find(Material::Flat,false);
 Check(flat&&flat!=materials::Find(Material::Flat,true),"Prop reused a through-wall material");
 Check(materialSources.back().find("unlitgeneric.vfx")!=std::string::npos&&materialSources.back().find("F_DISABLE_Z_BUFFERING = 1")==std::string::npos,
  "Persistent prop material is not depth-tested Flat");
 resetOutput();Check(draw()&&chamsCalls==1&&output.fixedCount==3,"Confirmed prop did not emit one Flat pass");
 Check(primitives[0].material==17&&primitives[1].material==flat&&primitives[1].materialCopy==flat&&primitives[2].material==42,
  "Flat prop pass modified old or another scene's primitives");
 Check(primitives[1].tint==(51u|(178u<<8)|(255u<<16)|(204u<<24)),"Prop tint did not reach the owned primitive");
 Check(memory::read<std::uint8_t>(scene+chams_bindings::kSceneFlags)==0xff,"Prop pass leaked scene flags");
 auto changedDuringOriginal=[&](const char* message,std::function<void()> transition,std::function<void()> restore){
  resetOutput();chamsTransition=std::move(transition);
  const bool generated=draw();chamsTransition={};restore();
  Check(generated&&chamsCalls==1&&output.fixedCount==3,message);
  Check(primitives[0].material==17&&primitives[1].material==23&&primitives[1].materialCopy==23&&
        primitives[2].material==42,"Callback transition recolored stale or unrelated native primitives");
  Check(memory::read<std::uint8_t>(scene+chams_bindings::kSceneFlags)==0xff,"Callback transition leaked scene flags");
  props::BeginMap(epoch,listAddress);props::Remember(hit);
  Check(props::Count()==1,"Callback transition fixture failed to restore discovery state");
 };
 changedDuringOriginal("Map epoch transition duplicated or skipped the native draw",
  [&]{hvh_input::worldEpoch.store(epoch+1);},[&]{hvh_input::worldEpoch.store(epoch);});
 changedDuringOriginal("Cache clear transition duplicated or skipped the native draw",
  [&]{props::Clear();},[]{});
 changedDuringOriginal("Model resource transition duplicated or skipped the native draw",
  [&]{binding[0]=Native::At(otherModel);},[&]{binding[0]=Native::At(model);});
 changedDuringOriginal("Scene owner transition duplicated or skipped the native draw",
  [&]{Native::Write(scene,chams_bindings::kSceneOwner,propHandle^0x8000);},
  [&]{Native::Write(scene,chams_bindings::kSceneOwner,propHandle);});
 auto expectRejected=[&](const char* message){resetOutput();Check(!draw()&&!chamsCalls&&output.fixedCount==1,message);};
 slot(2,entity,propHandle^0x8000);expectRejected("Reused full handle rendered an old selection");slot(2,entity,propHandle);
 slot(2,Native::At(replacement),propHandle);expectRejected("Replacement entity rendered an old selection");slot(2,entity,propHandle);
 Native::Write(scene,chams_bindings::kSceneOwner,propHandle^0x8000);expectRejected("Different native scene owner inherited Flat");Native::Write(scene,chams_bindings::kSceneOwner,propHandle);
 Native::Write(entity,Native::Node,Native::At(otherNode));expectRejected("Replacement scene node inherited Flat");Native::Write(entity,Native::Node,nodeAddress);
 Native::Write(nodeAddress,modelField,Native::At(otherBinding));expectRejected("Replacement model binding inherited Flat");Native::Write(nodeAddress,modelField,Native::At(binding));
 binding[0]=Native::At(otherModel);expectRejected("Replacement resident model inherited Flat");binding[0]=Native::At(model);
 Native::Write(entity,Native::Collision,Native::At(otherCollision));expectRejected("Replacement collision inherited Flat");Native::Write(entity,Native::Collision,Native::At(collision));
 Native::Write(nodeAddress,Native::Origin,math::vector3{100.001f,20,30});expectRejected("Moved prop inherited the old ballistic selection");Native::Write(nodeAddress,Native::Origin,origin);
 Native::Write(nodeAddress,Native::Angles,math::vector3{0,25.001f,0});expectRejected("Rotated prop inherited the old ballistic selection");Native::Write(nodeAddress,Native::Angles,angles);
 Native::Write(nodeAddress,Native::Origin,math::vector3{std::numeric_limits<float>::quiet_NaN(),20,30});expectRejected("Nonfinite native transform rendered");Native::Write(nodeAddress,Native::Origin,origin);
 // Camera, shooter and weapon changes do not evict historical map discovery.
 hit.stamp.weapon=Native::At(otherModel);hit.stamp.pawn=Native::At(pawn);hit.stamp.damage=1;
 hit.cameraOrigin={999,888,777};hit.eyeOrigin={-100,-200,-300};
 props::Remember(hit);Check(props::Count()==1,"Repeated detection duplicated a prop");
 resetOutput();Check(draw()&&props::Count()==1,"Weapon/player pose evicted a confirmed map prop");
 resetOutput();Check(visuals::chams::Generate(0,Native::At(enemySceneMemory),0,reinterpret_cast<Address>(&output),ChamsPrimitives)&&chamsCalls==2,
  "Persistent prop controls intercepted existing enemy chams");
 const auto enemyConfig=GetConfiguration(Target::Enemy);
 Check(primitives[1].material==materials::Find(enemyConfig.hidden.material,true)&&primitives[3].material==materials::Find(enemyConfig.visible.material,false)&&props::Count()==1,
  "Persistent prop pass altered existing enemy depth/style passes");
 auto disabledEnemy=enemyConfig,gunConfig=GetConfiguration(Target::ViewmodelWeapon),disabledGun=gunConfig;
 disabledEnemy.enabled=false;disabledGun.enabled=false;SetConfiguration(Target::Enemy,disabledEnemy);SetConfiguration(Target::ViewmodelWeapon,disabledGun);
 resetOutput();Check(draw()&&chamsCalls==1,"Disabling player/weapon chams disabled persistent props");
 SetConfiguration(Target::Enemy,enemyConfig);SetConfiguration(Target::ViewmodelWeapon,gunConfig);
 settings.props=false;visuals::penetration_overlay::Save(settings);expectRejected("Disabled prop target still emitted Flat");
 settings.props=true;visuals::penetration_overlay::Save(settings);
 props::BeginMap(epoch,listAddress);props::Remember(hit);
 hvh_input::worldEpoch.store(epoch+1);expectRejected("Map epoch change rendered stale props");
 props::BeginMap(epoch+1,listAddress);Check(!props::Count(),"New map retained old selections");
 props::Remember(hit);Check(!props::Count(),"Late old-map result repopulated the new map");
 hvh_input::worldEpoch.store(epoch);props::BeginMap(epoch,listAddress);props::Remember(hit);
 std::array<std::byte,0x210> otherList{};props::BeginMap(epoch,Native::At(otherList));
 Check(!props::Count(),"Entity-list replacement retained old selections");expectRejected("Replaced entity list rendered old props");
 props::BeginMap(epoch,listAddress);props::Remember(hit);props::Clear();expectRejected("Explicit cache clear still rendered Flat");
 visuals::penetration_overlay::Save(previousSettings);visuals::chams::client=previousClient;Shutdown();
}
int main(){try{
 using namespace visuals::enhancements;
 for(int definition=43;definition<=48;++definition)Check(ThrowableIndex(definition)==definition-43,"Throwable mapping");
 Check(ThrowableIndex(42)==-1&&ThrowableIndex(49)==-1,"Non-throwable mapping");
 const auto previousThrowables=Get();auto individual=previousThrowables;
 individual.throwables[2].trajectory=false;individual.throwables[2].width=4;
 individual.throwables[4].width=std::numeric_limits<float>::quiet_NaN();individual.throwables[4].markerRadius=99;
 Set(individual);const auto appliedThrowables=Get();
 Check(!appliedThrowables.throwables[2].trajectory&&appliedThrowables.throwables[2].width==4&&appliedThrowables.throwables[0].trajectory&&appliedThrowables.throwables[0].width==2,"Throwable settings are not independent");
 Check(appliedThrowables.throwables[4].width==2&&appliedThrowables.throwables[4].markerRadius==40,"Invalid throwable style bounds");
 Set(previousThrowables);
 ChamsStartupAndDisable();
 PersistentPropFlatPass();
 Check(std::abs(SpreadRadius(.01f,.02f,90,1920)-28.8f)<.001f,"Spread projection");
 Check(SpreadRadius(.01f,.02f,45,1920)>SpreadRadius(.01f,.02f,90,1920),"Scoped spread projection");
 Check(SpreadRadius(-1,0,90,1920)==0&&SpreadRadius(0,0,90,1920)==0,"Invalid spread");
 Check(SpreadRadius(.1f,std::numeric_limits<float>::quiet_NaN(),90,1920)==0,"NaN spread");
 std::array<std::byte,512> object{},identity{};
 const auto entity=reinterpret_cast<Address>(object.data()),id=reinterpret_cast<Address>(identity.data());
 memory::safe_write(entity+0x10,id);memory::safe_write(id+0x10,0x10001u);
 const auto target=entity+0x100;memory::safe_write(target,23.f);
 ++frameNumber;SetField(entity,target,0.f);Check(memory::read<float>(target)==0,"Field override");Reset();Check(memory::read<float>(target)==23,"Field restore");
 SetField(entity,target,0.f);memory::safe_write(target,42.f);SetField(entity,target,0.f);Reset();Check(memory::read<float>(target)==42,"Restore clobbered an engine update");
 SetField(entity,target,0.f);memory::safe_write(id+0x10,0x20001u);memory::safe_write(target,18.f);Reset();Check(memory::read<float>(target)==18,"Restore clobbered a reused entity");
 std::array<std::byte,128> system{},nodes{},variable{};const char name[]="ragdoll_gravity_scale";
 cvarSystem=reinterpret_cast<Address>(system.data());const auto var=reinterpret_cast<Address>(variable.data());
 memory::safe_write(cvarSystem+0x4a,std::uint16_t{1});memory::safe_write(cvarSystem+0x50,reinterpret_cast<Address>(nodes.data()));memory::safe_write(cvarSystem+0x58,std::uint16_t{});
 memory::safe_write(reinterpret_cast<Address>(nodes.data()),var);memory::safe_write(var,reinterpret_cast<Address>(name));memory::safe_write(var+0x58,1.f);memory::safe_write(var+0x30,0x102ull);
 Override(name,true,-1.f);Check(memory::read<float>(var+0x58)==-1,"Gravity override");Check(memory::read<std::uint64_t>(var+0x30)==0x100,"Cvar flags");
 memory::safe_write(var+0x30,0x108ull);Override(name,false,0);Check(memory::read<float>(var+0x58)==1&&memory::read<std::uint64_t>(var+0x30)==0x10a,"Cvar restoration");
 Override(name,true,0);memory::safe_write(var+0x58,3.f);Reset();Check(memory::read<float>(var+0x58)==3,"Cvar restore clobbered engine update");
 Configuration settings;settings.gravityScale=std::numeric_limits<float>::infinity();settings.jumpDuration=-2;Set(settings);Check(Get().gravityScale==0&&Get().jumpDuration==.2f,"Invalid settings");
 Check(visuals::gun_images::Source("materials/../../invalid.vtex",false).empty(),"Texture traversal");
 Check(visuals::gun_images::Source("materials/x\".vtex",false).empty(),"Texture source injection");
 visuals::gun_images::Configuration image;
 Check(!visuals::gun_images::CanUpdate(image,false),"Disabled image became active");
 for(std::size_t i=0;i<image_finishes::kPresets.size();++i){
  Check(visuals::gun_images::SelectPreset(image,i)&&!image.followSkybox&&image.enabled,"Camo preset follows skybox");
  Check(visuals::gun_images::CanUpdate(image,false),"Camo disabled without a map/skybox");
  Check(!visuals::gun_images::Source(image.texture.data(),false).empty(),"Invalid camo texture path");
 }
 Check(!visuals::gun_images::SelectPreset(image,image_finishes::kPresets.size()),"Invalid preset accepted");
 image.followSkybox=true;
 Check(!visuals::gun_images::CanUpdate(image,false)&&visuals::gun_images::CanUpdate(image,true),"Skybox follow gating");
 using namespace visuals::chams;
 Check(IsWeaponPreview("C_CSGO_PreviewModel","WEAPONS\\models\\ak47\\weapon_rif_ak47.vmdl_c"),"Lobby stock gun rejected");
 Check(IsWeaponPreview("C_CSGO_TeamPreviewModel","models/custom/mixed_pack/awp_rail/awp_rail.vmdl"),"Lobby custom gun rejected");
 Check(!IsWeaponPreview("C_CSGO_PreviewPlayer","weapons/models/ak47/weapon_rif_ak47.vmdl"),"Image applied to lobby player");
 Check(IsPreviewPlayer("C_CSGO_PreviewPlayer")&&IsPreviewPlayer("C_CSGO_PreviewPlayerAlias_csgo_player_previewmodel")&&!IsPreviewPlayer("C_CSPlayerPawn"),"Weapon preview parent accepts gameplay players");
 Check(!IsWeaponPreview("C_CSGO_PreviewModel","agents/models/ctm_sas/ctm_sas.vmdl"),"Image applied to agent preview");
 Check(!IsWeaponPreview("C_CSGO_PreviewModel","weapons/models/../../weapon_rif_ak47.vmdl"),"Unsafe lobby model path");
 EntityInfo lobby{};lobby.owner=entity;lobby.previewWeapon=true;
 Check(Classify(lobby)==Target::ViewmodelWeapon,"Lobby requires a live pawn");
 using namespace visuals::chams::detail;
 std::array<Primitive,3> primitives{};
 primitives[0].scene=entity;primitives[0].material=17;
 Output buffer{reinterpret_cast<Address>(primitives.data()),3,1};
 memory::safe_write(entity+chams_bindings::kSceneFlags,std::uint8_t{0xff});
 Check(ApplyImage(0,entity,0,reinterpret_cast<Address>(&buffer),ImagePrimitives,77,{200,180,160,255}),"Image pass failed");
 Check(imageCalls==1&&primitives[0].material==17&&primitives[1].material==77&&primitives[1].materialCopy==77&&primitives[2].material==42,"Image affected old or unrelated primitives");
 Check(memory::read<std::uint8_t>(entity+chams_bindings::kSceneFlags)==0xff,"Image did not restore scene flags");
 RequestStop();Frame();Check(Stopped()&&patches.empty()&&cvars.empty(),"Game-thread cleanup");
 std::cout<<"PASS: native persistent prop discovery, Flat primitive isolation, full identity/map rejection and existing chams coexistence; chams startup, depth/lit passes, disable lifecycle; accuracy, restoration, entity reuse; eight camo presets, lobby weapon isolation, scene flags and unload cleanup\n";
 return 0;}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
