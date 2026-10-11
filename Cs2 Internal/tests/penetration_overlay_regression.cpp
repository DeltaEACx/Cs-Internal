#include "features/visuals/engine.hpp"
#include "game/autowall_probe.h"
#include <bit>
#include <condition_variable>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace fixture {
ULONGLONG now=1000;
ULONGLONG Clock(){return now;}
math::matrix4x4 matrix;
bool cameraValid=true;
autowall_probe::Frame live;
std::atomic<unsigned> captures{},probes{},resets{},checks{};
bool failReset{},mismatch{},unavailable{},blocked{},clearRay{},splitShapes{},sourceBasis{},thinStripe{},surfacePresent=true,surfaceFresh=true;
bool discoverProp{};
autowall_probe::SurfaceKind kind=autowall_probe::SurfaceKind::World;
autowall_probe::Reason describedReason=autowall_probe::Reason::Unavailable;
float damage=35,distance=100;
int delayMs{};
std::mutex gateMutex;
std::condition_variable gate;
bool blockProbe{},entered{},leave{};
}
#define GetTickCount64 fixture::Clock
// Exercise the actual scan, cache, lifecycle and ImGui projection code. Only
// its native read/probe adapters and elapsed millisecond clock are injected.
#include "features/visuals/penetration_overlay.cpp"
#undef GetTickCount64

namespace systems {
math::matrix4x4 view::matrix() const{return fixture::matrix;}
void view::update_matrix(){m_matrix_valid.store(fixture::cameraValid);}
}
namespace autowall_probe {
Frame CaptureProfile(std::uint64_t session){++fixture::captures;auto value=fixture::live;value.stamp.session=session;return value;}
bool IsCurrent(const Frame &frame){
    ++fixture::checks;auto current=fixture::live;current.stamp.session=hvh_input::worldEpoch.load();
    return current.valid&&frame.valid&&current.stamp==frame.stamp&&
        current.eyeOrigin.x==frame.eyeOrigin.x&&current.eyeOrigin.y==frame.eyeOrigin.y&&current.eyeOrigin.z==frame.eyeOrigin.z;
}
bool IsSurfaceCurrent(const Result &result){return fixture::surfaceFresh&&result.physicsObject&&result.physicsShape;}
Result ProbeRay(const Frame &frame,const Vec3& origin,const Vec3& direction){
    const auto& view=visuals::penetration_overlay::scan.view;
    float axis=std::max({std::abs(direction.x),std::abs(direction.y),std::abs(direction.z)});
    const Vec3 position{origin.x+direction.x*100/axis,origin.y+direction.y*100/axis,origin.z+direction.z*100/axis};
    const auto ndc=visuals::detail::penetration_surface::Project(view.camera,{position.x,position.y,position.z});
    const auto x=ndc?ndc->x:0,y=ndc?ndc->y:0;

    ++fixture::probes;
    {std::unique_lock lock(fixture::gateMutex);if(fixture::blockProbe){fixture::entered=true;fixture::gate.notify_all();fixture::gate.wait(lock,[]{return fixture::leave;});}}
    if(fixture::delayMs)std::this_thread::sleep_for(std::chrono::milliseconds(fixture::delayMs));
    Result result;result.stamp=frame.stamp;if(fixture::mismatch)++result.stamp.weaponHandle;
    result.kind=fixture::kind;result.physicsObject=0x1234;result.physicsShape=0x5678;result.hitHandle=0x10001;
    if(fixture::splitShapes)result.physicsShape=(std::uint64_t{std::bit_cast<std::uint32_t>(x)}<<32|std::bit_cast<std::uint32_t>(y))+1;
    result.position=position;
    result.normal={-view.camera.matrix[3][0],-view.camera.matrix[3][1],-view.camera.matrix[3][2]};
    result.direction=direction;
    result.surface=fixture::surfacePresent&&!fixture::unavailable&&!fixture::clearRay;result.distance=fixture::distance;result.damage=fixture::damage;
    const bool blocked=fixture::blocked||(fixture::thinStripe&&std::abs(x)>.025f);
    result.state=fixture::unavailable?State::Unavailable:fixture::clearRay?State::Clear:blocked?State::Blocked:State::Pass;
    result.reason=fixture::unavailable?Reason::NoMaterialRecord:fixture::clearRay?Reason::Clear:blocked?Reason::Stopped:Reason::Penetrated;
    return result;
}
bool Reset(){++fixture::resets;return !fixture::failReset;}
const char* ReasonText(Reason reason){fixture::describedReason=reason;return "Native fixture state";}
}
namespace visuals::penetrable_props {
std::size_t selected{};
std::uint64_t epoch{};
std::uintptr_t list{};
void BeginMap(std::uint64_t next,std::uintptr_t nextList){if(next!=epoch||nextList!=list)selected=0;epoch=next;list=nextList;}
void Remember(const autowall_probe::Result& result){if(result.kind==autowall_probe::SurfaceKind::Prop)selected=1;}
void Clear(){selected=0;epoch=0;list=0;}
std::size_t Count(){return selected;}
bool Discover(const autowall_probe::Frame&,float,float){
    if(!fixture::discoverProp)return false;
    ++fixture::probes;
    if(fixture::delayMs)std::this_thread::sleep_for(std::chrono::milliseconds(fixture::delayMs));
    return true;
}
}
namespace fixture {
namespace overlay=visuals::penetration_overlay;
void Check(bool value,const char *text){if(!value)throw std::runtime_error(text);}
void Setup(bool nativeCamera=true){
    now=1000;captures=probes=resets=checks=0;cameraValid=true;
    matrix={};matrix.mat[0][0]=matrix.mat[1][1]=matrix.mat[2][2]=matrix.mat[3][2]=1;
    sourceBasis=nativeCamera;
    if(sourceBasis){matrix={};matrix.mat[0][1]=-1;matrix.mat[1][2]=matrix.mat[2][0]=matrix.mat[3][0]=1;}
    live={};live.valid=true;live.viewMatrix=matrix;live.reason=autowall_probe::Reason::Unavailable;
    live.stamp={1,0x10000,0x20000,0x30000,0x40000,0x50000,0x10001,0x20002,2,80,2,8192,.98f};
    failReset=mismatch=unavailable=blocked=clearRay=splitShapes=thinStripe=discoverProp=false;surfacePresent=surfaceFresh=true;
    kind=autowall_probe::SurfaceKind::World;describedReason=autowall_probe::Reason::Unavailable;damage=35;distance=100;delayMs=0;
    blockProbe=entered=leave=false;hvh_input::worldEpoch=1;systems::g_view.update_matrix();
    auto disabled=overlay::Settings{};disabled.enabled=false;overlay::Save(disabled);overlay::Initialize();
}
overlay::Settings Enabled(){overlay::Settings value;value.enabled=true;value.quality=0;value.probesPerFrame=64;value.budgetMs=4;return value;}
void Fill(){for(int i=0;i<4;++i){overlay::Frame(true);now+=16;}}
int Draw(){
    auto &io=ImGui::GetIO();io.DisplaySize={800,600};io.DeltaTime=1.f/60;
    ImGui::NewFrame();visuals::frame_snapshot frame;frame.screen_width=800;frame.screen_height=600;
    overlay::Render(frame);const int count=ImGui::GetBackgroundDrawList()->VtxBuffer.Size;ImGui::EndFrame();return count;
}
auto Snapshot(){std::lock_guard lock(overlay::snapshotMutex);return overlay::snapshot;}
enum class Outcome{Pass,Blocked,Clear,Unavailable,Kind,Distance,Damage,Stale};
void Classification(const overlay::Scan &value,Outcome expected,unsigned count){
    const std::array<unsigned,8> classified{value.passed,value.blocked,value.clear,value.unavailable,
        value.filteredKind,value.filteredDistance,value.filteredDamage,value.stale};
    Check(value.tested==count&&std::accumulate(classified.begin(),classified.end(),0u)==count,
        "Diagnostic categories dropped/double-counted native probes");
    for(std::size_t index=0;index<classified.size();++index)
        Check(classified[index]==(index==static_cast<std::size_t>(expected)?count:0u),
            "Native result was assigned the wrong diagnostic category/priority");
    Check(std::accumulate(value.reasons.begin(),value.reasons.end(),0u)==count,
        "Native reason histogram omitted or double-counted a query");
}
void DiagnosticsAndPriority(){
    using Kind=autowall_probe::SurfaceKind;using Reason=autowall_probe::Reason;
    const auto run=[](Outcome expected,Reason reason,auto change){
        Setup();auto settings=Enabled();settings.probesPerFrame=4;change(settings);overlay::Save(settings);
        overlay::Frame(true);const auto value=Snapshot();Classification(value,expected,4);
        Check(value.reasons[static_cast<std::size_t>(reason)]==4,"Native reason histogram lost source reason");
        Check(probes==4&&captures==1,"Diagnostic accounting added native work");
    };
    run(Outcome::Pass,Reason::Penetrated,[](auto&){});
    run(Outcome::Blocked,Reason::Stopped,[](auto&){blocked=true;distance=9999;damage=0;kind=Kind::Excluded;});
    run(Outcome::Clear,Reason::Clear,[](auto&){clearRay=true;blocked=true;kind=Kind::Unknown;});
    run(Outcome::Unavailable,Reason::NoMaterialRecord,[](auto&){unavailable=true;clearRay=blocked=true;kind=Kind::Excluded;});
    run(Outcome::Kind,Reason::Penetrated,[](auto& settings){settings.walls=false;});
    run(Outcome::Kind,Reason::Penetrated,[](auto&){surfacePresent=false;distance=9999;damage=0;});
    run(Outcome::Distance,Reason::Penetrated,[](auto&){distance=2501;damage=0;});
    run(Outcome::Damage,Reason::Penetrated,[](auto&){damage=.5f;});
    run(Outcome::Stale,Reason::Penetrated,[](auto&){mismatch=true;kind=Kind::Excluded;});
    run(Outcome::Stale,Reason::NoMaterialRecord,[](auto&){mismatch=true;unavailable=true;});
    Setup();auto settings=Enabled();settings.probesPerFrame=4;overlay::Save(settings);overlay::Frame(true);
    now+=16;blocked=true;overlay::Frame(true);Classification(Snapshot(),Outcome::Blocked,4);
    Check(Snapshot().freshPassed==4,"Later blocked probe erased a previous successful discovery sample");
}
void AdmissionAndBudgets(){
    Setup();overlay::Frame(true);Check(!captures&&!probes&&!resets,"Disabled overlay ran native work");
    auto settings=Enabled();overlay::Save(settings);overlay::Frame(false);
    Check(!captures&&!probes&&Draw()==0,"Out-of-world overlay admitted work");
    settings.probesPerFrame=7;overlay::Save(settings);overlay::Frame(true);
    Check(captures==1&&probes==7,"Per-frame native probe ceiling not honored");
    delayMs=3;settings.probesPerFrame=64;settings.budgetMs=.25f;overlay::Save(settings);const auto before=probes.load();
    overlay::Frame(true);Check(probes==before+1,"Elapsed frame budget admitted another expensive probe");
    delayMs=0;overlay::Frame(false);Check(overlay::Stopped()&&resets==1,"Native scratch not released outside world");
    overlay::Frame(false);Check(resets==1,"Clean scratch released repeatedly");
    Setup();settings=Enabled();settings.probesPerFrame=7;overlay::Save(settings);discoverProp=true;
    overlay::Frame(true);Check(probes==7&&Snapshot().tested==6,"Loaded prop query exceeded shared per-frame count");
    delayMs=3;settings.budgetMs=.25f;overlay::Save(settings);const auto sharedBefore=probes.load();
    overlay::Frame(true);Check(probes==sharedBefore+1&&Snapshot().tested==0,"Loaded prop query exceeded shared elapsed budget");
}
void PersistentDiscovery(){
    Setup();auto settings=Enabled();settings.quality=2;settings.probesPerFrame=32;overlay::Save(settings);
    overlay::Frame(true);Check(Draw()>0,"First native-camera discovery batch produced no validated geometry");
    const auto first=overlay::patches.size();Check(first>0,"First completed cells were not retained");
    now+=60000;Check(Draw()>0&&overlay::patches.size()==first,"350ms / frame TTL still expires retained geometry");
    matrix.mat[0][3]=-20;live.eyeOrigin.z=2;Check(Draw()>0,"Camera/eye motion erased retained world coverage");
    const auto origin=overlay::scan.origin;
    for(int frame=0;frame<29;++frame){overlay::Frame(true);now+=500;}
    Check(overlay::scan.cursor==941&&probes==941,"Detailed face did not finish its bounded unique rays");
    Check(overlay::patches.size()==28*16,"Slow complete face lost valid cells to old TTL or camera motion");
    Check(overlay::scan.origin.x==origin.x&&overlay::scan.origin.z==origin.z,"Pending face mixed moved shooter origins");
    const auto old=overlay::patches.size();
    live.eyeOrigin={};
    ++live.stamp.weaponHandle;live.stamp.damage=22;overlay::Frame(true);
    Check(overlay::patches.size()==old&&Draw()>0,"Changing weapons erased historical detections or duplicated same coverage");
    live.valid=false;overlay::Frame(true);Check(overlay::patches.size()==old&&Draw()>0,"Temporary unsupported weapon/death erased map cache");
    live.valid=true;cameraValid=false;systems::g_view.update_matrix();Check(Draw()==0,"Missing render camera admitted polygons");
    cameraValid=true;systems::g_view.update_matrix();
    overlay::rescanRequested=true;settings.probesPerFrame=4;overlay::Save(settings);overlay::Frame(true);
    Check(overlay::patches.empty()&&Draw()==0,"Explicit rescan retained old geometry");
}
void AllDirectionsAndRestart(){
    Setup();auto settings=Enabled();overlay::Save(settings);
    // Discovery does not depend on a valid view matrix or which way we look.
    live.viewMatrix={};
    for(int i=0;i<40&&!overlay::scan.complete;++i){overlay::Frame(true);now+=1000;}
    Check(overlay::scan.complete&&probes==6*213,"Six-face discovery did not visit every planned ray exactly once");
    Check(overlay::patches.size()==6*12*8,"All six fixed perspectives did not yield their own planar coverage");
    const auto before=probes.load();const auto retained=overlay::patches.size();overlay::Frame(true);
    Check(probes==before,"Completed position repeatedly rescanned while stationary");
    live.eyeOrigin.x+=200;blocked=true;overlay::Frame(true);
    Check(probes>before&&overlay::patches.size()==retained,"Exploration did not resume or erased older confirmed surfaces");
    Check(overlay::scan.origin.x==200,"New exploration scan used the old origin");
    blocked=false;overlay::Frame(false);Check(overlay::patches.empty(),"Leaving map retained world geometry");
    overlay::Frame(true);Check(probes>before+64,"Loading into active map did not restart automatic discovery");
}
void FiltersAndIdentity(){
    const auto empty=[](const char* message){overlay::Reset();Fill();Check(overlay::patches.empty()&&Draw()==0,message);};
    Setup();auto settings=Enabled();overlay::Save(settings);Fill();Check(Draw()>0,"Passing geometry did not render");
    mismatch=true;empty("Foreign weapon results entered cache");mismatch=false;
    blocked=true;empty("Blocked geometry entered cache");blocked=false;
    unavailable=true;empty("Unavailable geometry entered cache");unavailable=false;
    splitShapes=true;empty("Unrelated physics shapes were joined");splitShapes=false;
    thinStripe=true;empty("Sparse hits borrowed blocked corners");thinStripe=false;
    kind=autowall_probe::SurfaceKind::Excluded;empty("Actors entered cache");
    kind=autowall_probe::SurfaceKind::Prop;settings.props=false;overlay::Save(settings);empty("Disabled props entered cache");
    Check(visuals::penetrable_props::Count()==0,"Disabled props received Flat selection");
    settings.props=true;overlay::Save(settings);overlay::Reset();Fill();
    Check(visuals::penetrable_props::Count()==1&&Draw()==0,"Passing prop did not select Flat, or double-rendered screen patches");
    kind=autowall_probe::SurfaceKind::World;settings.minDamage=40;overlay::Save(settings);empty("Below-damage geometry entered cache");
    settings.minDamage=1;settings.maxDistance=128;distance=129;overlay::Save(settings);empty("Beyond-distance geometry entered cache");
    distance=100;settings=Enabled();overlay::Save(settings);Fill();Check(Draw()>0,"Cache failed recovery");
    hvh_input::LevelChanged();Check(Draw()==0,"Previous map rendered before reset callback");
    settings.probesPerFrame=4;overlay::Save(settings);overlay::Frame(true);Check(overlay::patches.empty(),"Map epoch reused geometry");
    settings.probesPerFrame=64;overlay::Save(settings);Fill();Check(Draw()>0,"New map failed discovery");
    live.stamp.list+=0x1000;settings.probesPerFrame=4;overlay::Save(settings);overlay::Frame(true);
    Check(overlay::patches.empty(),"New entity list reused old map geometry");
    settings.enabled=false;overlay::Save(settings);overlay::Frame(true);
    Check(overlay::Stopped()&&Draw()==0&&visuals::penetrable_props::Count()==0,"Disable retained native work or selections");
}
void CleanupAndConcurrentStop(){
    Setup();overlay::Save(Enabled());overlay::Frame(true);failReset=true;overlay::RequestStop();
    Check(!overlay::Stopped()&&Draw()==0,"Stop claimed cleanup before native release");
    const auto before=probes.load();overlay::Frame(false);
    Check(!overlay::Stopped()&&probes==before,"Failed cleanup resumed probes or discarded ownership");
    failReset=false;overlay::Frame(false);Check(overlay::Stopped(),"Native cleanup could not retry");
    Setup();overlay::Save(Enabled());blockProbe=true;
    std::thread frame([]{overlay::Frame(true);});
    {std::unique_lock lock(gateMutex);gate.wait(lock,[]{return entered;});}
    std::atomic<bool> finished{};
    std::thread stop([&]{overlay::RequestStop();finished=true;});
    while(!overlay::stopping.load())std::this_thread::yield();
    const bool returnedWhileInFlight=finished.load();
    {std::lock_guard lock(gateMutex);leave=true;}gate.notify_all();frame.join();stop.join();
    Check(!returnedWhileInFlight,"Stop returned while a native probe was in flight");
    Check(finished&&overlay::Stopped()&&Draw()==0,"Late native completion republished after stop");
    const auto completed=probes.load();overlay::Frame(true);Check(probes==completed,"A stopped overlay admitted native work");
}
}
int main(){try{
    fixture::Check(visuals::penetration_overlay::Settings{}.enabled,"Automatic map discovery must default enabled");
    ImGui::CreateContext();ImGui::GetIO().IniFilename=nullptr;
    unsigned char *pixels{};int width{},height{};ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    fixture::AdmissionAndBudgets();fixture::DiagnosticsAndPriority();fixture::PersistentDiscovery();fixture::AllDirectionsAndRestart();
    fixture::FiltersAndIdentity();fixture::CleanupAndConcurrentStop();
    ImGui::DestroyContext();std::cout<<"PASS: persistent surface discovery, six perspectives, frozen origins, map lifetime, Flat prop selection, bounded queries, geometry filters, reset and concurrent stop\n";return 0;
}catch(const std::exception &error){std::cerr<<error.what()<<'\n';if(ImGui::GetCurrentContext())ImGui::DestroyContext();return 1;}}
