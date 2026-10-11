// Exercise the production overlay without a game or a graphics device.
#include "features/visuals/engine.hpp"
#include "features/combat/hvh.h"
#include "game/autowall_probe.h"
#include <iostream>
#include <stdexcept>
namespace fixture {
std::optional<hvh::SpreadPrediction> prediction;
autowall_probe::Frame profile;
bool current{true},rayFails{},clearRay{};
unsigned rays{};
ULONGLONG now{1000};
math::vector3 lastDirection;
ULONGLONG Clock() { return now; }
void Check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
}
#define GetTickCount64 fixture::Clock
#include "features/visuals/spread_prediction.cpp"
#undef GetTickCount64

namespace hvh {
std::optional<SpreadPrediction> Prediction() { return fixture::prediction; }
}
namespace autowall_probe {
Frame CaptureProfile(std::uint64_t) { return fixture::profile; }
bool IsCurrent(const Frame&) { return fixture::current; }
std::optional<ShotContact> TraceShot(const Frame&,const Vec3& origin,const Vec3& direction,float range) {
    ++fixture::rays; fixture::lastDirection=direction;
    if (fixture::rayFails) return {};
    return ShotContact{origin+direction*(fixture::clearRay?range:100.f),!fixture::clearRay};
}
}
namespace systems {
void view::update_matrix() { m_matrix_valid=true; }
view::projection view::project_full(const math::vector3& value) const {
    if (value.x<.01f) return {{},value.x,false};
    const math::vector2 point{320.f-value.y/value.x*320.f,180.f-value.z/value.x*320.f};
    return {point,value.x,point.x>=0&&point.x<=640&&point.y>=0&&point.y<=360};
}
}
namespace math {
vector3 vector3::operator+(const vector3& value) const noexcept { return {x+value.x,y+value.y,z+value.z}; }
vector3 vector3::operator-(const vector3& value) const noexcept { return {x-value.x,y-value.y,z-value.z}; }
vector3 vector3::operator*(float scalar) const noexcept { return {x*scalar,y*scalar,z*scalar}; }
float vector3::length_sqr() const noexcept { return x*x+y*y+z*z; }
float vector3::length() const noexcept { return std::sqrt(length_sqr()); }
}
namespace fixture {
int Render() {
    ImGui::NewFrame();
    const auto before=rays;
    visuals::spread_prediction::Render({});
    const auto vertices=ImGui::GetBackgroundDrawList()->VtxBuffer.Size;
    Check(rays==before,"Rendering performed a native trace");
    ImGui::Render(); return vertices;
}
bool ContainsColor(ImU32 color) {
    for (const auto& vertex:ImGui::GetBackgroundDrawList()->VtxBuffer)
        if (vertex.col==color) return true;
    return false;
}
}
int main() {
    using namespace fixture;
    try {
        ImGui::CreateContext();auto& io=ImGui::GetIO();io.DisplaySize={640,360};io.DeltaTime=1.f/60.f;
        io.IniFilename=nullptr;unsigned char* pixels;int width,height;
        io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
        systems::g_view.update_matrix();
        hvh::SpreadPrediction forecast;forecast.range=1000;forecast.radius=.04f;forecast.time=now;
        forecast.localHandle=0x8001;forecast.weaponHandle=0x8002;
        forecast.epoch=hvh_input::worldEpoch.load();
        forecast.exact=hvh_math::PredictedShot{{},{1,0,0},{},0,1200};
        prediction=forecast;profile.valid=true;profile.stamp.localHandle=forecast.localHandle;
        profile.stamp.weaponHandle=forecast.weaponHandle;
        visuals::spread_prediction::Frame(true);
        Check(rays==1&&lastDirection.x==1&&Render()>0&&ContainsColor(IM_COL32(80,230,160,240)),
              "Known seed did not render a green native-contact marker");
        prediction->exact.reset();visuals::spread_prediction::Frame(true);
        Check(Render()>0&&ContainsColor(IM_COL32(245,190,75,220))&&
              !ContainsColor(IM_COL32(80,230,160,240)),"Unknown seed rendered an exact marker instead of the amber envelope");
        rayFails=true;visuals::spread_prediction::Frame(true);Check(Render()==0,"Failed trace left a marker on screen");
        rayFails=false;visuals::spread_prediction::Frame(true);now+=151;
        Check(Render()==0,"Expired marker remained visible");
        now=1000;current=false;Check(Render()==0,"Stale player/weapon frame remained visible");current=true;
        ++profile.stamp.weaponHandle;const auto before=rays;visuals::spread_prediction::Frame(true);
        Check(rays==before&&Render()==0,"Changed weapon traced or displayed the old prediction");
        profile.stamp.weaponHandle=forecast.weaponHandle;visuals::spread_prediction::Frame(false);
        Check(Render()==0,"World shutdown retained the marker");
        prediction.reset();Check(Render()==0,"Disabled predictor retained the marker");
        ImGui::DestroyContext();std::cout<<"Spread overlay regression: PASS\n";return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
