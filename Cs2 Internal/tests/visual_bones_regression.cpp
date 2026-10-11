#include "features/visuals/engine.hpp"
#include "game/visual_access.cpp"
#include "features/combat/detail/hvh_points.hpp"
#include "features/combat/detail/hvh_math.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <xmmintrin.h>

namespace {
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T> std::uintptr_t Address(T& value) { return reinterpret_cast<std::uintptr_t>(&value); }
struct SchemaField { const char* name; std::uintptr_t type{}; std::uint32_t offset{}; std::byte padding[12]{}; };
struct SchemaClass { std::byte padding[0x24]{}; std::uint16_t count{1}; std::byte reserved[10]{}; SchemaField* fields{}; };
static_assert(offsetof(SchemaClass, fields) == 0x30 && sizeof(SchemaField) == 0x20);
SchemaField sceneField{"m_pGameSceneNode", 0, 0x40}, stateField{"m_modelState", 0, 0x80}, modelField{"m_hModel", 0, 0xa0};
SchemaClass sceneClass, stateClass, modelClass;
std::array<std::uintptr_t, 14> schemaTable{};
std::array<std::uintptr_t, 3> scopeTable{};
std::uintptr_t schemaObject{}, scopeObject{};
std::uintptr_t __fastcall GetScope(std::uintptr_t, const char*, void*) { return Address(scopeObject); }
void __fastcall FindClass(std::uintptr_t, std::uintptr_t* output, const char* name) {
    *output = std::strcmp(name, "C_BaseEntity") == 0 ? Address(sceneClass)
        : std::strcmp(name, "CSkeletonInstance") == 0 ? Address(stateClass)
        : std::strcmp(name, "CModelState") == 0 ? Address(modelClass) : 0;
}
struct Entry { std::int32_t hash{-1}; std::uint32_t token{}; std::uint16_t index{}, padding{}; };
struct Header { std::uint32_t grow{}, allocation{256}; std::uintptr_t entries{}; std::int32_t active{21}, buckets{256}; };
// Tokens extracted/checked against Source 2 bone-name values. Native indices
// deliberately differ from the stock virtual slots (custom head is index14).
constexpr std::array<std::uint32_t, 21> tokens{{162587645,2276883802u,110709982,322482005,560415250,2008498285,1462898209,
    18463961,1291619918,730178398,2305728263u,2251241857u,1258956371,542075771,3782808388u,
    475370876,2883344725u,1135864906,609771703,2098558004,236023422}};
constexpr std::array<unsigned,21> slots{{1,2,3,4,23,6,7,8,9,10,11,12,13,14,15,17,18,19,20,21,22}};
struct Fixture {
    std::array<std::byte,128> entity{};
    std::array<std::byte,512> scene{}, model{};
    std::array<systems::bones::data,115> bones{};
    std::array<Entry,256> entries{};
    std::uintptr_t binding{};
    Header header;
    Fixture() {
        for (std::size_t i{}; i < bones.size(); ++i) { bones[i].position = {float(i + 1),float(i + 2),float(i + 3)}; bones[i].scale = 1; }
        header.entries=Address(entries); binding=Address(model);
        for (std::size_t i{}; i < tokens.size(); ++i) entries[i] = {std::int32_t(tokens[i] & 0x3fffffffu),tokens[i],std::uint16_t(i == 6 ? 14 : i + 40),0};
        memory::safe_write(Address(entity)+0x40,Address(scene));
        memory::safe_write(Address(scene)+0x80+0x80,Address(bones));
        memory::safe_write(Address(scene)+0x80+0x88,Address(entries));
        memory::safe_write(Address(scene)+0x80+0x90,128);
        memory::safe_write(Address(scene)+0x80+0x94,std::uint16_t(bones.size()));
        memory::safe_write(Address(scene)+0x80+0x96,std::uint16_t{0xffff});
        memory::safe_write(Address(scene)+0x80+0xa0,Address(binding));
        memory::safe_write(Address(model)+0x178,int(bones.size()));
        Sync();
    }
    void Sync() { memory::safe_write(Address(model)+0x130,header); }
    auto Read(bool namedOnly=false) { return systems::g_bones.get_skeleton(Address(entity),namedOnly); }
};
bool faultAccuracy{};
float __fastcall NativeSpread(std::uintptr_t weapon) { memory::safe_write(weapon+0x84,27.f); return .02f; }
float __fastcall NativeAccuracy(std::uintptr_t weapon, float* first, float* second) {
    if (first || second) RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);
    memory::safe_write(weapon+0x88,81.f);
    if (faultAccuracy) RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);
    return .03f;
}
std::uintptr_t __fastcall NativeView(std::uintptr_t input, int slot) { return slot == 0 ? input : 0; }

void CandidatePoints(Fixture& fixture) {
    auto rotation=[&](float x,float y,float z,float w) {
        auto& value=fixture.bones[73].rotation;value.x=x;value.y=y;value.z=z;value.w=w;
    };
    combat_access::Target target;
    target.pawn=Address(fixture.entity); target.handle=0x10002; target.team=3; target.health=100;
    int nativeCalls=0, skeletonCalls=0;
    auto native=[&](unsigned index) { ++nativeCalls;return systems::g_bones.get(target.pawn,index); };
    auto pose=[&] { ++skeletonCalls;return fixture.Read(true); };
    auto points=hvh_points::Collect(target,native,pose);
    Check(nativeCalls==0&&skeletonCalls==1&&!points.empty(),"Missing hitbox metadata discarded an ESP-readable enemy pose");
    Check(points[0].fallback&&points[0].hitbox==-1&&points[0].position.x==fixture.bones[14].position.x,
          "Fallback head used anatomy7 as native index instead of mapped head14");
    hvh_math::View view;
    view.localPawn=0x10000;view.localHandle=0x10001;view.localTeam=2;view.range=8192;
    const float matrix[4][4]={{0,.5f,0,0},{0,0,.5f,0},{1,0,0,0},{1,0,0,0}};
    view.matrix=matrix;view.width=1920;view.height=1080;
    hvh_math::Candidate candidate;
    candidate.pawn=target.pawn;candidate.handle=target.handle;candidate.team=target.team;candidate.health=target.health;
    candidate.valid=true;candidate.point={points[0].position.x,points[0].position.y,points[0].position.z};
    Check(bool(hvh_math::ProjectCandidate(view,candidate,0,false)),"ESP-readable fallback head is not a projected HVH candidate");

    target.hitboxes.count=1;
    auto& hitbox=target.hitboxes.entries[0];
    hitbox.index=0;hitbox.bone=73;hitbox.radius=3;hitbox.mins={-2,-2,-3};hitbox.maxs={2,2,3};
    rotation(0,0,0,1);
    nativeCalls=skeletonCalls=0;
    points=hvh_points::Collect(target,native,pose);
    Check(points.size()==1&&!points[0].fallback&&points[0].position.x==fixture.bones[73].position.x&&
          nativeCalls==1&&skeletonCalls==0,"Native hitbox bone73 indexed the 27-slot anatomy array or lost priority");
    // A translated hitbox center is transformed by its model quaternion.
    hitbox.mins={2,0,0};hitbox.maxs={2,0,0};
    const float half=std::sqrt(.5f);rotation(0,0,half,half);
    points=hvh_points::Collect(target,native,pose);
    Check(points.size()==1&&std::abs(points[0].position.x-fixture.bones[73].position.x)<1e-5f&&
          std::abs(points[0].position.y-fixture.bones[73].position.y-2)<1e-5f,"Hitbox midpoint did not rotate in native bone space");
    rotation(0,0,0,0);
    skeletonCalls=0;points=hvh_points::Collect(target,native,pose);
    Check(skeletonCalls==1&&!points.empty()&&points[0].fallback,"Invalid native quaternion did not recover verified pose points");
    hitbox.translation_only=true;
    fixture.bones[73].rotation.x=std::numeric_limits<float>::quiet_NaN();
    skeletonCalls=0;points=hvh_points::Collect(target,native,pose);
    Check(points.size()==1&&!points[0].fallback&&skeletonCalls==0&&points[0].position.x==fixture.bones[73].position.x+2,
          "Translation-only hitbox incorrectly required a quaternion");
    hitbox.translation_only=false;rotation(0,0,0,1);
    target.hitboxes.count=2;target.hitboxes.entries[1]=hitbox;
    target.hitboxes.entries[1].index=0;
    points=hvh_points::Collect(target,native,pose);
    Check(points.size()==1,"Duplicate native centers consumed extra trace candidates");
    target.hitboxes.entries[1].index=1;
    points=hvh_points::Collect(target,native,pose);
    Check(points.size()==2&&hvh_points::Accept(points[0],hvh_points::Selection::Head)&&
          !hvh_points::Accept(points[1],hvh_points::Selection::Head),
          "Coincident native neck center replaced a selectable head region");
    target.hitboxes.count=21;nativeCalls=skeletonCalls=0;
    points=hvh_points::Collect(target,native,pose);
    Check(nativeCalls==0&&skeletonCalls==1&&!points.empty()&&points[0].fallback,"Malformed hitbox count read past native array");
    target.hitboxes.count=-1;nativeCalls=0;points=hvh_points::Collect(target,native,pose);
    Check(nativeCalls==0&&!points.empty()&&points[0].fallback,"Negative hitbox count did not use valid pose");
    target.hitboxes.count=1;hitbox.bone=1024;
    nativeCalls=0;points=hvh_points::Collect(target,native,pose);
    Check(nativeCalls==0&&!points.empty()&&points[0].fallback,"Invalid native bone index reached callback");
    hitbox.bone=73;hitbox.radius=std::numeric_limits<float>::quiet_NaN();
    nativeCalls=0;points=hvh_points::Collect(target,native,pose);
    Check(nativeCalls==0&&!points.empty()&&points[0].fallback,"Nonfinite hitbox geometry reached callback");
    target.hitboxes.count=0;
    std::array<systems::bones::data,27> missing{};
    Check(hvh_points::Collect(target,native,[&] {return missing;}).empty(),"Missing pose created invented aim points");
    missing[cstypes::bone_ids::head].position={100,0,64};
    missing[cstypes::bone_ids::neck].position={100.005f,0,64};
    missing[cstypes::bone_ids::pelvis].position={std::numeric_limits<float>::quiet_NaN(),0,0};
    missing[cstypes::bone_ids::left_hand].position={1e8f,0,0};
    missing[cstypes::bone_ids::right_hand].position={100,0,32};
    points=hvh_points::Collect(target,native,[&] {return missing;});
    Check(points.size()==3&&points[0].position.z==64&&points[2].position.z==32&&
          hvh_points::Accept(points[0])&&!hvh_points::Accept(points[1])&&!hvh_points::Accept(points[2]),
          "Fallback invalid/world-bound points or coincident region selection failed");
    rotation(0,0,0,1);
}

void PointRegions(Fixture& fixture) {
    using hvh_points::Region;
    using hvh_points::Selection;
    static_assert(int(Selection::Head)==0&&int(Selection::HeadAndBody)==1&&int(Selection::Body)==2&&int(Selection::All)==3);
    const auto count=[](const std::vector<hvh_points::Point>& points,Selection selection) {
        return std::count_if(points.begin(),points.end(),[&](const auto& point) {return hvh_points::Accept(point,selection);});
    };
    combat_access::Target target;
    target.pawn=Address(fixture.entity);target.hitboxes.count=19;
    for (int i=0;i<target.hitboxes.count;++i) {
        auto& hitbox=target.hitboxes.entries[i];
        hitbox.index=i;hitbox.bone=50+i;hitbox.translation_only=true;
    }
    // Deliberately conflict native indices with anatomical meanings: ordinal0
    // lives at native73, ordinal1 at custom head14, and ordinal2 at stock head7.
    target.hitboxes.entries[0].bone=73;
    target.hitboxes.entries[1].bone=14;
    target.hitboxes.entries[2].bone=7;
    target.hitboxes.entries[13].bone=73; // Coincident limb must not erase the head.
    auto native=[&](unsigned index) {return systems::g_bones.get(target.pawn,index);};
    auto pose=[&] {return fixture.Read(true);};
    auto points=hvh_points::Collect(target,native,pose);
    Check(points.size()==19&&count(points,Selection::Head)==1&&count(points,Selection::HeadAndBody)==6&&
          count(points,Selection::Body)==5&&count(points,Selection::All)==19,
          "Native aim selection did not group head/core body/neck/limbs correctly");
    Check(points[0].region==Region::Head&&!points[0].fallback&&points[0].hitbox==0&&
          points[0].position.x==fixture.bones[73].position.x&&hvh_points::Accept(points[0]),
          "Native head-only default used a bone index instead of hitbox ordinal0");
    Check(points[1].region==Region::Neck&&!hvh_points::Accept(points[1],Selection::HeadAndBody)&&
          points[2].region==Region::Body&&!hvh_points::Accept(points[2],Selection::Head)&&
          points[13].region==Region::Limb&&!hvh_points::Accept(points[13],Selection::HeadAndBody),
          "Native index aliases leaked neck/limb points into head or core-body selection");
    Check(!hvh_points::Accept(points[0],static_cast<Selection>(999)),"Unknown selection accepted a point");

    // A custom head may appear after a torso ordinal. The declared native
    // hitgroup, not the standard hitbox ordering, controls head selection.
    target.hitboxes.count=2;
    target.hitboxes.entries[0].index=0;target.hitboxes.entries[0].group=2;
    target.hitboxes.entries[1].index=11;target.hitboxes.entries[1].group=1;
    target.hitboxes.entries[1].bone=73;
    points=hvh_points::Collect(target,native,pose);
    Check(points.size()==2&&count(points,Selection::Head)==1&&points[0].region==Region::Body&&
          points[1].region==Region::Head&&!points[1].fallback&&points[1].hitbox==11,
          "Reordered custom hitgroups still assumed ordinal0 was the head");
    target.hitboxes.count=1;
    points=hvh_points::Collect(target,native,pose);
    Check(points.size()==2&&count(points,Selection::Head)==1&&!points[0].fallback&&points[1].fallback&&
          points[1].position.x==fixture.bones[14].position.x,
          "Partial native torso metadata suppressed the verified custom head fallback");
    auto absent=fixture.Read(true);absent[cstypes::bone_ids::head]={};
    points=hvh_points::Collect(target,native,[&] {return absent;});
    Check(points.size()==1&&count(points,Selection::Head)==0,"Missing custom head was invented from unrelated torso data");
    target.hitboxes.entries[0].group=0;
    points=hvh_points::Collect(target,native,pose);
    Check(points.size()==2&&points[0].region==Region::Unknown&&!hvh_points::Accept(points[0])&&
          points[1].fallback&&hvh_points::Accept(points[1]),
          "Declared generic custom hitbox0 was assumed to be a standard head");

    // Metadata-free selection uses the real production anatomy name map.
    target.hitboxes.count=0;
    points=hvh_points::Collect(target,native,pose);
    Check(points.size()==21&&count(points,Selection::Head)==1&&count(points,Selection::HeadAndBody)==6&&
          count(points,Selection::Body)==5&&count(points,Selection::All)==21,
          "Anatomical fallback lost explicit aim-point regions");
    Check(points[0].fallback&&points[0].hitbox==-1&&points[0].region==Region::Head&&
          points[0].position.x==fixture.bones[14].position.x&&hvh_points::Accept(points[0]),
          "Head-only fallback failed to select custom mapped native head14");
    std::array<systems::bones::data,27> missingHead{};
    missingHead[cstypes::bone_ids::neck].position={100,0,64};
    missingHead[cstypes::bone_ids::spine_4].position={100,0,48};
    missingHead[cstypes::bone_ids::left_hand].position={110,0,40};
    points=hvh_points::Collect(target,native,[&] {return missingHead;});
    Check(count(points,Selection::Head)==0&&count(points,Selection::HeadAndBody)==1&&
          count(points,Selection::Body)==1&&count(points,Selection::All)==3,
          "Missing fallback head was silently replaced by neck/body/limb");
}

void HitboxMultipoints(Fixture& fixture) {
    const auto previous=fixture.bones[73];
    auto& bone=fixture.bones[73];
    bone.position={100,20,30};
    const float half=std::sqrt(.5f);
    bone.rotation.x=0;bone.rotation.y=0;bone.rotation.z=half;bone.rotation.w=half;
    combat_access::Target target;target.hitboxes.count=1;
    auto& box=target.hitboxes.entries[0];
    box.index=11;box.bone=73;box.group=1;
    box.mins={-2,0,0};box.maxs={2,0,0};box.radius=4;
    const auto native=[&](unsigned index) {return systems::g_bones.get(Address(fixture.entity),index);};
    const auto pose=[&] {return fixture.Read(true);};
    const auto centers=hvh_points::Collect(target,native,pose);
    auto points=hvh_points::Collect(target,native,pose,.65f);
    Check(centers.size()==1&&points.size()==7,"Capsule multipoints did not preserve center and six interior samples");
    Check(std::abs(points[1].position.x-100)<1e-5f&&std::abs(points[1].position.y-22.6f)<1e-5f,
          "Capsule multipoint was not transformed by its native bone quaternion");
    auto inverse=bone.rotation;inverse.x=-inverse.x;inverse.y=-inverse.y;inverse.z=-inverse.z;
    for(const auto& point:points) {
        const auto local=inverse.rotate_vector(point.position-bone.position);
        const auto x=std::clamp(local.x,-2.f,2.f);
        Check((local-math::vector3{x,0,0}).length_sqr()<=16.001f&&point.hitbox==11&&
              point.region==hvh_points::Region::Head&&!point.fallback,
              "Capsule multipoint escaped actual geometry or lost declared custom hitgroup");
    }
    box.radius=0;box.mins={-8,-4,-2};box.maxs={8,4,2};
    points=hvh_points::Collect(target,native,pose,.5f);
    Check(points.size()==7,"Box multipoints did not produce distinct interior face samples");
    Check(std::abs(points[1].position.y-24)<1e-5f,"Box multipoint ignored native rotation or point scale");
    for(const auto& point:points) {
        const auto local=inverse.rotate_vector(point.position-bone.position);
        Check(std::abs(local.x)<=4.001f&&std::abs(local.y)<=2.001f&&std::abs(local.z)<=1.001f,
              "Box multipoint escaped configured interior extents");
    }
    box.translation_only=true;bone.rotation.x=NAN;bone.rotation.y=NAN;bone.rotation.z=NAN;bone.rotation.w=NAN;
    points=hvh_points::Collect(target,native,pose,.5f);
    Check(points.size()==7&&points[1].position.x==104&&points[1].position.y==20,
          "Translation-only multipoint incorrectly required or applied bone rotation");
    for(float scale:{0.f,-.1f,1.1f,NAN,INFINITY})
        Check(hvh_points::Collect(target,native,pose,scale).size()==1,
              "Invalid or disabled point scale produced unbounded multipoints");
    box.mins={};box.maxs={};
    Check(hvh_points::Collect(target,native,pose,.65f).size()==1,
          "Degenerate box generated duplicate trace candidates");
    target.hitboxes.count=0;
    Check(hvh_points::Collect(target,native,pose,.65f).size()==hvh_points::Collect(target,native,pose).size(),
          "Named-bone fallback invented multipoint collision extents");
    fixture.bones[73]=previous;
}

void ScaledHitboxGeometry(Fixture& fixture) {
    const auto previous=fixture.bones[73];
    auto& bone=fixture.bones[73];
    bone.position={100,20,30};bone.scale=2.5f;
    const float half=std::sqrt(.5f);
    bone.rotation.x=0;bone.rotation.y=0;bone.rotation.z=half;bone.rotation.w=half;
    const auto native=systems::g_bones.get(Address(fixture.entity),73);
    Check(native.scale==2.5f&&native.position.x==100&&native.position.y==20&&native.position.z==30,
          "Native pose reader discarded bone scale or scaled its world position twice");
    combat_access::Hitbox source;
    source.index=11;source.bone=73;source.group=1;source.radius=2;
    source.mins={2,-1,-1};source.maxs={6,1,1};
    const auto scaled=hvh_points::ScaleGeometry(source,native.scale);
    Check(scaled&&scaled->mins.x==5&&scaled->mins.y==-2.5f&&scaled->maxs.x==15&&scaled->radius==5&&
          scaled->index==11&&scaled->bone==73&&scaled->group==1&&source.mins.x==2&&source.radius==2,
          "Hitbox scaling changed source metadata or failed its once-scaled extent contract");
    combat_access::Target target;target.hitboxes.count=1;target.hitboxes.entries[0]=*scaled;
    const auto points=hvh_points::Collect(target,[&](unsigned index) {return systems::g_bones.get(Address(fixture.entity),index);},
        [] {return std::array<systems::bones::data,27>{};},.5f);
    Check(points.size()==7&&std::abs(points[0].position.x-100)<1e-5f&&
          std::abs(points[0].position.y-30)<1e-5f&&std::abs(points[1].position.y-32.5f)<1e-5f,
          "Scaled native hitbox center or multipoint applied bone scale incorrectly");
    auto inverse=native.rotation;inverse.x=-inverse.x;inverse.y=-inverse.y;inverse.z=-inverse.z;
    for(const auto& point:points) {
        const auto local=inverse.rotate_vector(point.position-native.position);
        const auto segment=scaled->maxs-scaled->mins;
        const auto relative=local-scaled->mins;
        const float along=std::clamp(relative.dot(segment)/segment.length_sqr(),0.f,1.f);
        Check((local-(scaled->mins+segment*along)).length_sqr()<=25.001f,
              "Scaled capsule multipoint escaped independently computed geometry");
    }
    bone.scale=.5f;source.radius=0;source.mins={-4,-2,-1};source.maxs={4,2,1};
    const auto smaller=hvh_points::ScaleGeometry(source,systems::g_bones.get(Address(fixture.entity),73).scale);
    Check(smaller&&smaller->mins.x==-2&&smaller->maxs.y==1&&smaller->maxs.z==.5f,
          "Sub-unit native scale was lost or applied twice");
    target.hitboxes.entries[0]=*smaller;
    const auto boxPoints=hvh_points::Collect(target,[&](unsigned index) {return systems::g_bones.get(Address(fixture.entity),index);},
        [] {return std::array<systems::bones::data,27>{};},.5f);
    Check(boxPoints.size()==7&&std::abs(boxPoints[1].position.y-21)<1e-5f,
          "Scaled box multipoints ignored smaller native geometry");
    for(const auto& point:boxPoints) {
        const auto local=inverse.rotate_vector(point.position-bone.position);
        Check(std::abs(local.x)<=1.001f&&std::abs(local.y)<=.501f&&std::abs(local.z)<=.251f,
              "Scaled box sample escaped its independent interior bounds");
    }
    for(float scale:{0.f,-1.f,100.1f,NAN,INFINITY})
        Check(!hvh_points::ScaleGeometry(source,scale),"Invalid native scale produced usable hitbox geometry");
    source.radius=NAN;
    Check(!hvh_points::ScaleGeometry(source,1.f),"Nonfinite native radius was scaled into usable geometry");
    source.radius=-1;
    Check(!hvh_points::ScaleGeometry(source,1.f),"Negative native radius was scaled into usable geometry");
    source.radius=1;source.mins.x=NAN;
    Check(!hvh_points::ScaleGeometry(source,1.f),"Nonfinite native extent was scaled into usable geometry");
    volatile float largeValue=std::numeric_limits<float>::max();
    source.mins.x=largeValue;
    Check(!hvh_points::ScaleGeometry(source,100.f),"Overflowing scale conversion produced usable geometry");
    fixture.bones[73]=previous;
}

void BooleanConvars() {
    struct Entry { std::uintptr_t variable{}; std::uint16_t generation{}, next{0xffff}; std::uint32_t links{}; };
    static_assert(sizeof(Entry)==0x10 && offsetof(Entry,next)==0xa);
    std::array<std::byte,0x60> registry{};
    std::array<std::byte,0x70> unrelated{}, policy{};
    std::array<Entry,2> entries{{{Address(unrelated),0,1,0},{Address(policy),0,0xffff,0}}};
    const char unrelatedName[]="sensitivity", policyName[]="sv_usercmd_custom_random_seed";
    memory::safe_write(Address(registry)+0x4a,std::uint16_t{0x8002});
    memory::safe_write(Address(registry)+0x50,Address(entries));
    memory::safe_write(Address(registry)+0x58,std::uint16_t{0});
    memory::safe_write(Address(unrelated),Address(unrelatedName));
    memory::safe_write(Address(policy),Address(policyName));
    memory::safe_write(Address(policy)+0x28,std::int16_t{0});
    const auto read=[&] { return combat_access::ReadBooleanConvar(Address(registry),policyName); };

    // The native server reads one boolean byte. Adjacent union padding must
    // not turn false into true (or into a float NaN and a fallback policy).
    for (const std::uint32_t bytes : {0x00000000u,0x3f800000u,0x7fc00000u,0xffffffffu & ~0xffu}) {
        memory::safe_write(Address(policy)+0x58,bytes);
        const auto value=read();
        Check(value&&!(*value),"False boolean convar was changed by adjacent union padding");
    }
    memory::safe_write(Address(policy)+0x58,std::uint32_t{1});
    const auto oldFloatingMode=_mm_getcsr();
    _mm_setcsr(oldFloatingMode|0x8040); // FTZ+DAZ: the float bit pattern1 would flush to zero.
    const auto underDaz=read();
    _mm_setcsr(oldFloatingMode);
    Check(underDaz&&*underDaz,"True boolean convar became false under native denormal mode");
    memory::safe_write(Address(policy)+0x58,std::uint32_t{0x7fc00001});
    Check(read().value_or(false),"True boolean convar depended on adjacent float NaN bits");
    memory::safe_write(Address(policy)+0x58,std::uint8_t{2});
    Check(read().value_or(false),"Boolean reader did not match native nonzero-byte comparison");
    for (const std::int16_t type : {std::int16_t{-1},std::int16_t{1},std::int16_t{6}}) {
        memory::safe_write(Address(policy)+0x28,type);
        Check(!read(),"Wrong convar type was interpreted as a boolean policy");
    }
    memory::safe_write(Address(policy)+0x28,std::int16_t{0});
    Check(!combat_access::ReadBooleanConvar(Address(registry),"missing")&&
          !combat_access::ReadBooleanConvar(Address(registry),nullptr)&&
          !combat_access::ReadBooleanConvar(Address(registry),"")&&
          !combat_access::ReadBooleanConvar(0,policyName),"Unavailable boolean convar became a known false policy");
    entries[1].variable=0;
    Check(!read(),"Null native convar object accepted");
    entries[1].variable=0x10000;
    Check(!read(),"Unreadable native convar object accepted");
    entries[1].variable=Address(policy);
    memory::safe_write(Address(registry)+0x58,std::uint16_t{2});
    Check(!read(),"Out-of-capacity convar list head accepted");
    memory::safe_write(Address(registry)+0x58,std::uint16_t{0});
    entries[0].next=0;
    Check(!read(),"Cyclic convar traversal escaped its native capacity bound");
    entries[0].next=1;
    memory::safe_write(Address(registry)+0x50,UINTPTR_MAX-4);
    Check(!read(),"Overflowing convar registry span accepted");
    Check(!combat_access::ConvarBool(policyName),"Public boolean reader bypassed client compatibility guard");
}
}
int main() { try {
    sceneClass.fields=&sceneField; stateClass.fields=&stateField; modelClass.fields=&modelField;
    schemaTable[13]=reinterpret_cast<std::uintptr_t>(&GetScope); scopeTable[2]=reinterpret_cast<std::uintptr_t>(&FindClass);
    schemaObject=Address(schemaTable); scopeObject=Address(scopeTable); addresses::globals::schema_system=Address(schemaObject);
    Fixture fixture;
    auto result=fixture.Read();
    Check(result[7].position.x==15,"Custom native head14 did not map to ESP head7");
    for(std::size_t i{};i<slots.size();++i) Check(result[slots[i]].position.x==fixture.bones[fixture.entries[i].index].position.x,"Anatomical slot used stock bone order");
    Check(result[0].position.length_sqr()==0,"Unused virtual slot populated");
    for (const std::uint32_t upper : {1006u,65536u,UINT32_MAX}) {
        memory::safe_write(Address(fixture.scene)+0x80+0x88,(std::uintptr_t{upper}<<32)|0x10000);
        const auto skeleton=fixture.Read();
        Check(skeleton[7].position.x==15&&systems::g_bones.get(Address(fixture.entity),73).position.x==74&&
              systems::g_bones.get(Address(fixture.entity),114).position.x==115&&
              systems::g_bones.get(Address(fixture.entity),115).position.length_sqr()==0,
              "Cache pointer upper bits were interpreted as bone count");
    }
    memory::safe_write(Address(fixture.scene)+0x80+0x94,std::uint16_t{14});
    Check(fixture.Read()[7].position.length_sqr()==0&&
          systems::g_bones.get(Address(fixture.entity),13).position.x==14&&
          systems::g_bones.get(Address(fixture.entity),14).position.length_sqr()==0,
          "Short live pose cache was read using only the larger model count");
    memory::safe_write(Address(fixture.scene)+0x80+0x94,std::uint16_t{0});
    Check(fixture.Read()[7].position.length_sqr()==0&&systems::g_bones.get(Address(fixture.entity),0).position.length_sqr()==0,
          "Empty live pose cache was accepted");
    memory::safe_write(Address(fixture.scene)+0x80+0x94,std::uint16_t{1025});
    Check(fixture.Read()[7].position.length_sqr()==0,"Unbounded live cache count was accepted");
    memory::safe_write(Address(fixture.scene)+0x80+0x94,std::uint16_t{115});
    memory::safe_write(Address(fixture.model)+0x178,74);
    Check(systems::g_bones.get(Address(fixture.entity),73).position.x==74&&
          systems::g_bones.get(Address(fixture.entity),74).position.length_sqr()==0,
          "Model count did not bound the live pose cache");
    memory::safe_write(Address(fixture.model)+0x178,115);
    CandidatePoints(fixture);
    PointRegions(fixture);
    HitboxMultipoints(fixture);
    ScaledHitboxGeometry(fixture);
    BooleanConvars();
    fixture.entries[6].index=73; result=fixture.Read();
    Check(result[7].position.x==74,"Hotreload reused stale model mapping");
    fixture.entries[6].index=115; result=fixture.Read(); Check(result[7].position.length_sqr()==0,"Out-of-range named bone fell back to unrelated stock bone");
    fixture.entries[6].index=14; fixture.entries[6].hash=-1; --fixture.header.active; fixture.Sync();
    Check(fixture.Read()[7].position.length_sqr()==0,"Missing named head used wrong native bone");
    fixture.entries[6]={std::int32_t(tokens[6]&0x3fffffffu),tokens[6],14,0}; ++fixture.header.active; fixture.Sync();
    fixture.bones[14].position.x=std::numeric_limits<float>::quiet_NaN();
    Check(fixture.Read()[7].position.length_sqr()==0,"Nonfinite bone escaped snapshot"); fixture.bones[14].position.x=15;
    fixture.binding=0; result=fixture.Read(); Check(result[7].position.length_sqr()==0,"Missing resident model invented a bone count"); fixture.binding=Address(fixture.model);
    fixture.header.buckets=257; fixture.Sync(); Check(fixture.Read()[7].position.x==8,"Malformed hash capacity was read");
    Check(fixture.Read(true)[7].position.length_sqr()==0,"Named-only HVH anatomy guessed stock head on an invalid custom map");
    fixture.header.buckets=256; fixture.header.entries=UINTPTR_MAX-4; fixture.Sync(); Check(fixture.Read()[7].position.x==8,"Overflowing map pointer was read");
    fixture.header.entries=0x10000; fixture.Sync(); Check(fixture.Read()[7].position.x==8,"Unmapped map pointer escaped memory fence");
    Check(fixture.Read(true)[7].position.length_sqr()==0,"Missing custom map reused a cached named head");
    fixture.header.entries=Address(fixture.entries); fixture.Sync();
    Check(fixture.Read(true)[7].position.x==15,"Resident custom map did not recover after rejoin-style unavailability");
    fixture.entries[6].index=73;
    Check(fixture.Read(true)[7].position.x==74,"Same model/entry allocations reused a stale head mapping after reload");
    fixture.entries[6].index=14;
    memory::safe_write(Address(fixture.model)+0x178,1025); Check(fixture.Read()[7].position.length_sqr()==0,"Unbounded bone cache accepted");
    memory::safe_write(Address(fixture.model)+0x178,0); Check(fixture.Read()[7].position.length_sqr()==0,"Empty model bone count accepted");
    memory::safe_write(Address(fixture.model)+0x178,115); memory::safe_write(Address(fixture.scene)+0x80+0x80,UINTPTR_MAX-4);
    Check(fixture.Read()[7].position.length_sqr()==0,"Overflowing bone cache accepted");
    Check(systems::g_bones.get_skeleton(0)[7].position.length_sqr()==0,"Null entity accepted");
    // Exercise production HUD native-call fences after removing the vendor
    // bridge: GetInaccuracy receives its two output pointers and restores state.
    std::array<std::byte,256> weapon{};
    const auto before=weapon;
    float spread=-1,inaccuracy=-1;
    Check(combat_access::Accuracy(Address(weapon),reinterpret_cast<std::uintptr_t>(&NativeSpread),reinterpret_cast<std::uintptr_t>(&NativeAccuracy),0x80,0x14,spread,inaccuracy),"HUD native accuracy ABI failed");
    Check(spread==.02f&&inaccuracy==.03f&&weapon==before,"HUD accuracy observation mutated weapon state");
    faultAccuracy=true;spread=inaccuracy=-1;
    Check(!combat_access::Accuracy(Address(weapon),reinterpret_cast<std::uintptr_t>(&NativeSpread),reinterpret_cast<std::uintptr_t>(&NativeAccuracy),0x80,0x14,spread,inaccuracy),"HUD native fault escaped fence");
    Check(weapon==before&&spread==-1&&inaccuracy==-1,"HUD fault failed state restoration");
    math::vector3 sourceAngles{12,45,0},angles;
    Check(combat_access::View(reinterpret_cast<std::uintptr_t>(&NativeView),Address(sourceAngles),angles)&&angles.x==12&&angles.y==45,"HUD view native ABI failed");
    sourceAngles.x=std::numeric_limits<float>::quiet_NaN();
    Check(!combat_access::View(reinterpret_cast<std::uintptr_t>(&NativeView),Address(sourceAngles),angles),"HUD accepted nonfinite camera");
    Check(combat_access::Convar("sv_gravity",800)==800&&!combat_access::ReadViewAngles(angles),"HUD read unverified client");
    std::cout<<"PASS: pose bounds/mapping, native/fallback aim points, typed boolean convars, HUD ABI/state restoration\n"; return 0;
} catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; } }
