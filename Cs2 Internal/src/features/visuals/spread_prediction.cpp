#include "engine.hpp"
#include "spread_prediction.h"
#include "features/combat/hvh.h"
#include "features/combat/detail/hvh_input.hpp"
#include "game/autowall_probe.h"

namespace visuals::spread_prediction {
namespace {
struct Snapshot {
    hvh::SpreadPrediction prediction;
    autowall_probe::Frame profile;
    math::vector3 contact{};
    bool hit{};
};
std::mutex snapshotMutex;
std::optional<Snapshot> snapshot;
math::vector3 Point(hvh_math::Vec3 value) { return {value.x,value.y,value.z}; }
void Clear() { std::lock_guard lock(snapshotMutex); snapshot.reset(); }
bool Project(const math::vector3& point,ImVec2& screen) {
    const auto projected=systems::g_view.project_full(point);
    if (projected.w<.01f || !std::isfinite(projected.w) ||
        !std::isfinite(projected.screen.x) || !std::isfinite(projected.screen.y)) return false;
    screen={projected.screen.x,projected.screen.y}; return true;
}
}
void Frame(bool inWorld) {
    const auto prediction=inWorld?hvh::Prediction():std::nullopt;
    if (!prediction) { Clear(); return; }
    const auto profile=autowall_probe::CaptureProfile(prediction->epoch);
    if (!profile.valid || profile.stamp.localHandle!=prediction->localHandle ||
        profile.stamp.weaponHandle!=prediction->weaponHandle ||
        (profile.eyeOrigin-Point(prediction->origin)).length_sqr()>64.f) { Clear(); return; }
    const auto direction=prediction->exact?std::optional{prediction->exact->direction}:
        hvh_math::ShotDirection(prediction->angles,{});
    if (!direction) { Clear(); return; }
    const auto contact=autowall_probe::TraceShot(profile,Point(prediction->origin),Point(*direction),prediction->range);
    if (!contact) { Clear(); return; }
    Snapshot next{*prediction,profile,contact->position,contact->hit};
    std::lock_guard lock(snapshotMutex); snapshot=next;
}
void Render(const frame_snapshot&) {
    if (!hvh::Prediction() || !systems::g_view.has_camera()) return;
    std::optional<Snapshot> current;
    { std::lock_guard lock(snapshotMutex); current=snapshot; }
    if (!current || GetTickCount64()-current->prediction.time>150 ||
        current->prediction.epoch!=hvh_input::worldEpoch.load(std::memory_order_acquire) ||
        !autowall_probe::IsCurrent(current->profile)) return;
    auto* draw=ImGui::GetBackgroundDrawList();
    if (!draw) return;
    const auto& forecast=current->prediction;
    ImVec2 center;
    if (!Project(current->contact,center)) return;
    const auto color=forecast.exact?IM_COL32(80,230,160,240):IM_COL32(245,190,75,220);
    if (forecast.exact) {
        draw->AddCircle(center,6.f,IM_COL32(8,10,14,230),20,4.f);
        draw->AddCircle(center,6.f,color,20,1.8f);
        draw->AddLine({center.x-3.f,center.y},{center.x+3.f,center.y},color,1.f);
        draw->AddLine({center.x,center.y-3.f},{center.x,center.y+3.f},color,1.f);
        draw->AddText({center.x+10.f,center.y+7.f},color,
            forecast.compensated?(current->hit?"Compensated contact":"Compensated direction"):
            current->hit?"Predicted contact":"Predicted direction");
        return;
    }
    // This is the maximum spread envelope at the center ray's contact plane,
    // not a guessed server seed or a set of claimed impacts on nearby geometry.
    const auto basis=hvh_math::MakeShotBasis(forecast.angles);
    const auto distance=(current->contact-Point(forecast.origin)).length();
    if (!basis || !std::isfinite(distance)) return;
    const auto origin=hvh_math::detail::Convert(forecast.origin);
    const auto plane=origin+basis->forward*distance;
    const double radius=double(forecast.radius)*distance;
    for (unsigned index=0;index<48;++index) {
        const auto boundary=[&](unsigned point) {
            const double angle=point*2.*std::numbers::pi/48.;
            const auto world=plane+basis->left*(radius*std::cos(angle))+basis->up*(radius*std::sin(angle));
            return math::vector3{float(world.x),float(world.y),float(world.z)};
        };
        ImVec2 a,b;
        if (Project(boundary(index),a) && Project(boundary(index+1),b)) draw->AddLine(a,b,color,1.5f);
    }
    draw->AddText({center.x+10.f,center.y+7.f},color,"Spread envelope (seed unknown)");
}
}
