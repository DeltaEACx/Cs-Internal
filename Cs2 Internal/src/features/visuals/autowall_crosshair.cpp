#include "core/user_defaults.hpp"
#include "engine.hpp"
#include "autowall_crosshair.h"
#include "detail/surface_square.hpp"
#include "detail/surface_square_projection.hpp"
#include "game/autowall_probe.h"

namespace visuals::autowall_crosshair {
namespace {
    namespace square = detail::surface_square;
    std::mutex settingsMutex, snapshotMutex, workMutex;
    Settings settings=user_defaults::Crosshair(Settings{});
    std::atomic<bool> stopping{true}, clean{true};
    struct Snapshot { autowall_probe::Result result; ULONGLONG time{}; };
    Snapshot snapshot;

    void ClearSnapshot() { std::lock_guard lock(snapshotMutex); snapshot={}; }
    bool Release() {
        ClearSnapshot();
        if(clean.load(std::memory_order_acquire)) return true;
        const bool released=autowall_probe::Reset();
        clean.store(released,std::memory_order_release);
        return released;
    }
    square::Vec3 Point(const math::vector3 &p) { return {p.x,p.y,p.z}; }
    ImU32 Color(autowall_probe::State state,int alpha) {
        if(state==autowall_probe::State::Pass) return IM_COL32(80,230,130,alpha);
        if(state==autowall_probe::State::Blocked) return IM_COL32(245,80,85,alpha);
        return IM_COL32(245,190,75,alpha);
    }
}
Settings Get() { std::lock_guard lock(settingsMutex); return settings; }
void Save(Settings value) {
    value.size=std::isfinite(value.size)?std::clamp(value.size,2.f,64.f):8.f;
    std::lock_guard lock(settingsMutex); settings=value;
}
void Initialize() {
    std::lock_guard lock(workMutex);
    ClearSnapshot(); stopping.store(false,std::memory_order_release);
}
void Frame(bool inWorld) {
    std::lock_guard work(workMutex);
    const auto value=Get();
    if(stopping.load(std::memory_order_acquire)||!value.enabled||!inWorld) { Release(); return; }
    // Mark storage owned before the probe; failed native calls may retain buffers.
    clean.store(false,std::memory_order_release);
    Snapshot current;
    current.result=autowall_probe::Probe();
    current.time=GetTickCount64();
    std::lock_guard lock(snapshotMutex); snapshot=current;
}
bool Reset() { std::lock_guard lock(workMutex); return Release(); }
void RequestStop() {
    stopping.store(true,std::memory_order_release);
    // Wait for any probe already in flight. No subsequent frame may allocate.
    std::lock_guard lock(workMutex); ClearSnapshot();
}
bool Stopped() { return clean.load(std::memory_order_acquire); }
void Render(const frame_snapshot &frame) {
    const auto value=Get();
    if(!value.enabled||stopping.load(std::memory_order_acquire)||!systems::g_view.has_camera()) return;
    Snapshot current;
    { std::lock_guard lock(snapshotMutex); current=snapshot; }
    if(!current.time||GetTickCount64()-current.time>250||!current.result.surface) return;
    square::Corners corners;
    if(!square::Build(Point(current.result.position),Point(current.result.normal),
                      Point(current.result.direction),value.size,corners)) return;
    const auto matrix=systems::g_view.matrix();
    square::Polygon polygon;
    if(!square::Project(corners,matrix.mat,frame.screen_width,frame.screen_height,polygon)) return;
    std::array<ImVec2,12> points;
    double area{};
    for(std::size_t i=0;i<polygon.count;++i) {
        const auto &a=polygon.points[i],&b=polygon.points[(i+1)%polygon.count];
        points[i]={a.x,a.y}; area+=double(a.x)*b.y-double(b.x)*a.y;
    }
    if(std::abs(area)<.2) return;
    // ImGui's convex anti-aliasing expects clockwise screen-space winding.
    if(area<0) std::reverse(points.begin(),points.begin()+polygon.count);
    auto *draw=ImGui::GetBackgroundDrawList();
    const int count=static_cast<int>(polygon.count);
    draw->AddConvexPolyFilled(points.data(),count,Color(current.result.state,30));
    draw->AddPolyline(points.data(),count,IM_COL32(8,10,14,220),true,3.5f);
    draw->AddPolyline(points.data(),count,Color(current.result.state,245),true,1.8f);
}
void DrawMenu() {
    if(!ImGui::CollapsingHeader("Autowall",ImGuiTreeNodeFlags_DefaultOpen)) return;
    auto value=Get();
    bool changed=ImGui::Checkbox("Surface crosshair",&value.enabled);
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Projects a square onto the surface you aim at. Green: penetrable. Red: blocked. Amber: unavailable.");
    changed|=ImGui::SliderFloat("Square size",&value.size,2.f,64.f,"%.1f units");
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Measured on the surface; appears smaller at a distance.");
    if(changed) Save(value);
}
}
