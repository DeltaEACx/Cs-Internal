#include "core/user_defaults.hpp"
#include "engine.hpp"
#include "penetration_overlay.h"
#include "detail/penetration_surface.hpp"
#include "detail/surface_square_projection.hpp"
#include "features/combat/detail/hvh_input.hpp"
#include "game/autowall_probe.h"
#include "penetrable_props.h"
#include <chrono>
#include <numeric>

namespace visuals::penetration_overlay {
namespace {
namespace surface=detail::penetration_surface;
namespace projection=detail::surface_square;
std::mutex settingsMutex,workMutex,snapshotMutex;
Settings settings=user_defaults::Surfaces(Settings{});
std::atomic<bool> stopping{true},clean{true};
std::atomic<unsigned> drawnPatches{};
std::atomic<bool> rescanRequested{},active{};
std::atomic<const char*> status{"Penetrable surfaces are disabled."};
struct Scan {
    autowall_probe::Frame frame;
    surface::Frame view;
    Settings settings;
    int columns{},rows{};
    std::vector<surface::Sample> samples;
    std::vector<autowall_probe::Result> results;
    std::vector<std::size_t> order;
    std::vector<bool> committed;
    std::size_t cursor{};
    unsigned tested{},passed{};
    unsigned blocked{},clear{},unavailable{},filteredDistance{},filteredDamage{},filteredKind{},stale{},freshPassed{};
    std::array<unsigned,static_cast<std::size_t>(autowall_probe::Reason::InvalidSurfaceIdentity)+1> reasons{};
    ULONGLONG updated{};
    math::vector3 origin{};
    unsigned face{};
    bool complete{};
};
Scan scan,snapshot;
struct DetectedPatch {surface::Patch patch;surface::Identity identity;};
std::vector<DetectedPatch> patches;
std::uint64_t mapEpoch{};
std::uintptr_t mapList{};
Settings discoverySettings;
constexpr std::size_t patchLimit=16384;
std::uint64_t generation{};
surface::Vec3 Point(const math::vector3& point){return {point.x,point.y,point.z};}
bool SameStamp(const autowall_probe::FrameStamp& a,const autowall_probe::FrameStamp& b){
    return a.session==b.session&&a.list==b.list&&a.controller==b.controller&&a.pawn==b.pawn&&
        a.weapon==b.weapon&&a.weaponData==b.weaponData&&a.localHandle==b.localHandle&&
        a.weaponHandle==b.weaponHandle&&a.team==b.team&&a.damage==b.damage&&
        a.penetration==b.penetration&&a.range==b.range&&a.rangeModifier==b.rangeModifier;
}
bool SameScanSettings(const Settings& a,const Settings& b){
    return a.enabled==b.enabled&&a.walls==b.walls&&a.props==b.props&&a.quality==b.quality&&
        a.maxDistance==b.maxDistance&&a.minDamage==b.minDamage;
}
void ClearSnapshot(){std::lock_guard lock(snapshotMutex);snapshot={};drawnPatches=0;}
void Clear(){
    scan={};++generation;ClearSnapshot();
    {std::lock_guard lock(snapshotMutex);patches.clear();mapEpoch=0;mapList=0;}
    penetrable_props::Clear();active=false;
}
bool Release(){
    Clear();
    if(clean.load(std::memory_order_acquire))return true;
    const bool released=autowall_probe::Reset();clean.store(released,std::memory_order_release);return released;
}
std::size_t Vertex(const Scan& value,int x,int y){return static_cast<std::size_t>(y)*(value.columns+1)+x;}
std::size_t Center(const Scan& value,int x,int y){return static_cast<std::size_t>(value.columns+1)*(value.rows+1)+y*value.columns+x;}
surface::Vec2 Coordinates(const Scan& value,std::size_t index){
    const auto vertices=static_cast<std::size_t>(value.columns+1)*(value.rows+1);
    float x{},y{};
    if(index<vertices){x=static_cast<float>(index%(value.columns+1));y=static_cast<float>(index/(value.columns+1));}
    else{index-=vertices;x=static_cast<float>(index%value.columns)+.5f;y=static_cast<float>(index/value.columns)+.5f;}
    return {x/value.columns*2.f-1.f,1.f-y/value.rows*2.f};
}
void Grid(Scan& value,int quality){
    constexpr std::array<std::array<int,2>,3> sizes{{{12,8},{20,12},{28,16}}};
    value.columns=sizes[quality][0];value.rows=sizes[quality][1];
    const auto count=static_cast<std::size_t>(value.columns+1)*(value.rows+1)+value.columns*value.rows;
    value.samples.resize(count);value.results.resize(count);value.order.reserve(count);
    std::vector<int> cells(value.columns*value.rows);std::iota(cells.begin(),cells.end(),0);
    const auto distance=[&](int cell){const float x=(cell%value.columns+.5f)/value.columns-.5f;
        const float y=(cell/value.columns+.5f)/value.rows-.5f;return x*x+y*y;};
    std::stable_sort(cells.begin(),cells.end(),[&](int a,int b){return distance(a)<distance(b);});
    std::vector<bool> seen(count);
    const auto add=[&](std::size_t index){if(!seen[index]){seen[index]=true;value.order.push_back(index);}};
    // Scan each complete patch near the scan face's center first, sharing corner
    // probes with adjacent patches instead of running duplicate native traces.
    for(int cell:cells){const int x=cell%value.columns,y=cell/value.columns;
        add(Center(value,x,y));add(Vertex(value,x,y));add(Vertex(value,x+1,y));
        add(Vertex(value,x+1,y+1));add(Vertex(value,x,y+1));}
}
surface::Limits Limits(){
    surface::Limits limits;
    // All samples in a discovery face share its fixed acquisition basis.
    // BuildCell still validates five rays, identity, winding and planarity.
    limits.ttlMs=350;
    return limits;
}
// Six fixed perspective faces cover all directions from one discovery origin.
// They deliberately do not follow the rendered camera. Every pending cell
// uses one origin, basis and weapon profile until it has all five samples.
surface::Camera FaceCamera(const math::vector3& origin,unsigned face){
    constexpr float rows[6][3][3]={
        {{0,-1,0},{0,0,1},{1,0,0}},{{0,1,0},{0,0,1},{-1,0,0}},
        {{1,0,0},{0,0,1},{0,1,0}},{{-1,0,0},{0,0,1},{0,-1,0}},
        {{1,0,0},{0,-1,0},{0,0,1}},{{1,0,0},{0,1,0},{0,0,-1}}};
    surface::Camera camera;camera.origin=Point(origin);
    for(unsigned row=0;row<3;++row){const auto out=row==2?3:row;
        for(unsigned axis=0;axis<3;++axis)camera.matrix[out][axis]=rows[face][row][axis];
        camera.matrix[out][3]=-(rows[face][row][0]*origin.x+rows[face][row][1]*origin.y+rows[face][row][2]*origin.z);
    }
    return camera;
}
void BeginFace(){
    std::fill(scan.samples.begin(),scan.samples.end(),surface::Sample{});
    std::fill(scan.results.begin(),scan.results.end(),autowall_probe::Result{});
    scan.cursor=0;
    scan.committed.assign(static_cast<std::size_t>(scan.columns)*scan.rows,false);
    // The timestamp here belongs to a fixed geometry acquisition, not a live
    // current-eye verdict. Completed geometry is retained for this map.
    scan.view={{generation,scan.frame.stamp.localHandle,scan.frame.stamp.weaponHandle,generation},
        FaceCamera(scan.origin,scan.face),Point(scan.origin),1};
}
void StartScan(const autowall_probe::Frame& frame,const Settings& value){
    scan={};++generation;scan.frame=frame;scan.origin=frame.eyeOrigin;scan.settings=value;
    Grid(scan,value.quality);BeginFace();
}
bool SameGeometry(const DetectedPatch& old,const surface::Patch& patch,const autowall_probe::Result& result){
    if(old.identity.object!=result.physicsObject||old.identity.shape!=result.physicsShape||
       old.identity.ownerHandle!=result.hitHandle)return false;
    using namespace surface::policy_detail;
    const auto n=Convert(old.patch.normal),delta=Convert(patch.points[4])-Convert(old.patch.points[4]);
    if(Dot(n,Convert(patch.normal))<.999||std::abs(Dot(n,delta))>1)return false;
    // Do not stack rediscovered coverage on itself when returning to a place.
    // Only suppress the new patch if every corner is inside the old polygon.
    for(unsigned point=0;point<4;++point){double winding{};
        for(unsigned edge=0;edge<4;++edge){
            const auto a=Convert(old.patch.points[edge]);
            const auto b=Convert(old.patch.points[(edge+1)%4]);
            const auto side=Dot(Cross(b-a,Convert(patch.points[point])-a),n);
            if(std::abs(side)<1e-5)continue;
            if(winding&&side*winding<0)return false;
            winding=side>0?1:-1;
        }
    }
    return true;
}
void KeepCompletedCells(){
    if(!scan.settings.walls)return;
    std::lock_guard lock(snapshotMutex);
    for(int y=0;y<scan.rows;++y)for(int x=0;x<scan.columns;++x){
        const auto cell=static_cast<std::size_t>(y)*scan.columns+x;
        if(scan.committed[cell])continue;
        const auto center=Center(scan,x,y);const auto& result=scan.results[center];
        if(result.kind!=autowall_probe::SurfaceKind::World)continue;
        const std::array corners{scan.samples[Vertex(scan,x,y)],scan.samples[Vertex(scan,x+1,y)],
            scan.samples[Vertex(scan,x+1,y+1)],scan.samples[Vertex(scan,x,y+1)]};
        const auto patch=surface::BuildCell(corners,scan.samples[center],scan.view,Limits());
        if(!patch)continue;
        scan.committed[cell]=true;
        if(std::any_of(patches.begin(),patches.end(),[&](const auto& old){return SameGeometry(old,*patch,result);}))continue;
        if(patches.size()>=patchLimit)return;
        patches.push_back({*patch,scan.samples[center].identity});
    }
}
bool DrawPatch(const surface::Patch& patch,const math::matrix4x4& matrix,
               const frame_snapshot& frame,ImU32 tint){
    projection::Corners corners{};
    for(std::size_t i=0;i<corners.size();++i){const auto p=patch.points[i];corners[i]={p.x,p.y,p.z};}
    projection::Polygon polygon;
    if(!projection::Project(corners,matrix.mat,frame.screen_width,frame.screen_height,polygon))return false;
    std::array<ImVec2,12> points{};double area{};
    for(std::size_t i=0;i<polygon.count;++i){const auto p=polygon.points[i],q=polygon.points[(i+1)%polygon.count];
        points[i]={p.x,p.y};area+=double(p.x)*q.y-double(q.x)*p.y;}
    if(std::abs(area)<.2)return false;
    if(area<0)std::reverse(points.begin(),points.begin()+polygon.count);
    ImGui::GetBackgroundDrawList()->AddConvexPolyFilled(points.data(),static_cast<int>(polygon.count),tint);
    return true;
}
}
Settings Get(){std::lock_guard lock(settingsMutex);return settings;}
void Save(Settings value){
    const auto bounded=[](float v,float fallback,float lo,float hi){return std::isfinite(v)?std::clamp(v,lo,hi):fallback;};
    value.quality=std::clamp(value.quality,0,2);value.probesPerFrame=std::clamp(value.probesPerFrame,4,64);
    value.budgetMs=bounded(value.budgetMs,2.f,.25f,4.f);value.maxDistance=bounded(value.maxDistance,2500.f,128.f,8192.f);
    value.minDamage=bounded(value.minDamage,1.f,1.f,100.f);
    for(std::size_t i=0;i<value.color.size();++i)value.color[i]=bounded(value.color[i],i==3?.3f:1.f,0.f,1.f);
    {std::lock_guard lock(settingsMutex);settings=value;}
    ClearSnapshot();
}
void Initialize(){std::lock_guard lock(workMutex);Clear();stopping.store(false,std::memory_order_release);}
void Frame(bool inWorld){
    std::lock_guard work(workMutex);const auto value=Get();
    if(stopping.load(std::memory_order_acquire)||!value.enabled||!inWorld||(!value.walls&&!value.props)){
        Release();status=value.enabled?"Waiting for a map and an enabled surface type.":"Penetrable surfaces are disabled.";return;}
    const auto start=std::chrono::steady_clock::now();
    const auto epoch=hvh_input::worldEpoch.load(std::memory_order_acquire);
    if(rescanRequested.exchange(false)||mapEpoch!=epoch||!SameScanSettings(discoverySettings,value))Clear();
    const auto current=autowall_probe::CaptureProfile(epoch);
    if(!current.valid){status=autowall_probe::ReasonText(current.reason);return;}
    if(mapList&&mapList!=current.stamp.list)Clear();
    {std::lock_guard lock(snapshotMutex);mapEpoch=epoch;mapList=current.stamp.list;discoverySettings=value;}
    penetrable_props::BeginMap(epoch,current.stamp.list);active=true;
    const auto dx=scan.origin.x-current.eyeOrigin.x,dy=scan.origin.y-current.eyeOrigin.y,dz=scan.origin.z-current.eyeOrigin.z;
    if(!scan.frame.valid||!SameStamp(scan.frame.stamp,current.stamp)||
       (scan.complete&&dx*dx+dy*dy+dz*dz>128.f*128.f))StartScan(current,value);
    // Loaded prop enumeration is independent of camera visibility. Its one
    // possible native ray shares the same query count and elapsed budget.
    const bool propProbe=value.props&&penetrable_props::Discover(current,value.maxDistance,value.minDamage);
    if(propProbe)clean.store(false,std::memory_order_release);
    if(scan.complete){status="Nearby directional scan complete; checking loaded props and retaining map detections.";return;}
    if(scan.cursor==scan.order.size()){
        ++scan.face;
        if(scan.face==6){scan.complete=true;{std::lock_guard lock(snapshotMutex);snapshot=scan;}return;}
        BeginFace();
    }
    scan.tested=scan.passed=0;
    scan.blocked=scan.clear=scan.unavailable=scan.filteredDistance=scan.filteredDamage=scan.filteredKind=scan.stale=scan.freshPassed=0;
    scan.reasons.fill(0);
    for(int probe=propProbe?1:0;probe<value.probesPerFrame&&scan.cursor<scan.order.size();++probe){
        if(probe&&std::chrono::duration<float,std::milli>(std::chrono::steady_clock::now()-start).count()>=value.budgetMs)break;
        if(stopping.load(std::memory_order_acquire))break;
        const auto index=scan.order[scan.cursor++];
        const auto ndc=Coordinates(scan,index);
        const auto direction=surface::Direction(scan.view.camera,ndc);if(!direction)continue;
        clean.store(false,std::memory_order_release);
        const auto result=autowall_probe::ProbeRay(current,scan.origin,{direction->x,direction->y,direction->z});
        scan.results[index]=result;
        auto& sample=scan.samples[index];sample={};sample.ndc=ndc;sample.frame=scan.view;
        sample.position=Point(result.position);sample.normal=Point(result.normal);
        sample.identity={result.physicsObject,result.physicsShape,result.hitHandle,static_cast<std::uint8_t>(result.kind),
            result.kind==autowall_probe::SurfaceKind::World||result.kind==autowall_probe::SurfaceKind::Prop};
        const bool kind=(value.walls&&result.kind==autowall_probe::SurfaceKind::World)||
            (value.props&&result.kind==autowall_probe::SurfaceKind::Prop);
        bool accepted=false;
        if(!SameStamp(result.stamp,current.stamp))++scan.stale;
        else if(result.state==autowall_probe::State::Unavailable)++scan.unavailable;
        else if(result.state==autowall_probe::State::Clear)++scan.clear;
        else if(result.state==autowall_probe::State::Blocked)++scan.blocked;
        else if(!kind||!result.surface)++scan.filteredKind;
        else if(!std::isfinite(result.distance)||result.distance>value.maxDistance)++scan.filteredDistance;
        else if(!std::isfinite(result.damage)||result.damage<value.minDamage)++scan.filteredDamage;
        else accepted=true;
        const auto reason=static_cast<std::size_t>(result.reason);
        if(reason<scan.reasons.size())++scan.reasons[reason];
        sample.state=accepted?surface::State::Pass:result.state==autowall_probe::State::Blocked?surface::State::Blocked:
            result.state==autowall_probe::State::Clear?surface::State::Clear:surface::State::Unknown;
        ++scan.tested;if(accepted){++scan.passed;penetrable_props::Remember(result);}
    }
    if(stopping.load(std::memory_order_acquire)||!autowall_probe::IsCurrent(current)||
       current.stamp.session!=hvh_input::worldEpoch.load(std::memory_order_acquire)){
        scan={};
        if(stopping.load()||current.stamp.session!=hvh_input::worldEpoch.load())Release();
        status="Discovery paused while the player or weapon changes.";return;}
    scan.updated=GetTickCount64();
    for(const auto& sample:scan.samples)
        if(sample.state==surface::State::Pass&&surface::Fresh(sample,scan.view,Limits()))++scan.freshPassed;
    KeepCompletedCells();
    {std::lock_guard lock(snapshotMutex);snapshot=scan;}
    status=scan.tested&&scan.unavailable==scan.tested?
        "Surface tests are unavailable. The rejection reason is shown below.":
        "Discovering in all directions; keeping completed detections for this map.";
}
bool Reset(){std::lock_guard lock(workMutex);return Release();}
void RequestStop(){stopping.store(true,std::memory_order_release);std::lock_guard lock(workMutex);Clear();}
bool Stopped(){return clean.load(std::memory_order_acquire);}
void Render(const frame_snapshot& frame){
    drawnPatches=0;
    const auto value=Get();
    if(!value.enabled||!value.walls||!active||stopping.load(std::memory_order_acquire)||!systems::g_view.has_camera())return;
    std::vector<DetectedPatch> current;
    {std::lock_guard lock(snapshotMutex);
        if(mapEpoch!=hvh_input::worldEpoch.load(std::memory_order_acquire)||!SameScanSettings(value,discoverySettings))return;
        current=patches;
    }
    const auto matrix=systems::g_view.matrix();const auto camera=surface::MakeCamera(matrix.mat);if(!camera)return;
    const auto tint=ImGui::ColorConvertFloat4ToU32({value.color[0],value.color[1],value.color[2],value.color[3]});
    unsigned emitted{};
    for(const auto& detected:current){
        using namespace surface::policy_detail;
        if(Dot(Convert(detected.patch.normal),Convert(camera->origin)-Convert(detected.patch.points[4]))<=0)continue;
        if(DrawPatch(detected.patch,matrix,frame,tint))++emitted;
    }
    if(!stopping.load(std::memory_order_acquire))drawnPatches=emitted;
}
void DrawMenu(){
    auto value=Get();bool changed=ImGui::Checkbox("Highlight surfaces",&value.enabled);
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Scans on map entry and as you explore. Detections remain until you leave or rescan.");
    changed|=ImGui::Checkbox("Walls",&value.walls);
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Retains sampled wall patches. May show through nearer objects; unseen rooms and complete wall meshes are not covered.");
    changed|=ImGui::Checkbox("Props and doors",&value.props);
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Applies Flat chams to detected props, doors and breakables.");
    changed|=ImGui::ColorEdit4("Surface tint",value.color.data());
    constexpr const char* quality[]{"Fast (12 x 8)","Balanced (20 x 12)","Detailed (28 x 16)"};
    changed|=ImGui::Combo("Scan detail",&value.quality,quality,3);
    changed|=ImGui::SliderFloat("Maximum distance",&value.maxDistance,128.f,8192.f,"%.0f units");
    changed|=ImGui::SliderFloat("Minimum damage",&value.minDamage,1.f,100.f,"%.0f");
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Damage after penetration. A saved detection does not guarantee damage with every weapon or angle.");
    if(ImGui::CollapsingHeader("Scan performance")){
        changed|=ImGui::SliderInt("Probes per frame",&value.probesPerFrame,4,64);
        changed|=ImGui::SliderFloat("Time budget",&value.budgetMs,.25f,4.f,"%.2f ms");
        if(ImGui::IsItemHovered())ImGui::SetTooltip("Discovery is spread across frames. Higher detail takes longer.");
    }
    if(changed)Save(value);
    if(ImGui::Button("Rescan map"))rescanRequested=true;
}
}
