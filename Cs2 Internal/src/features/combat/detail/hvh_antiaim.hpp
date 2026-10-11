#pragma once
#include "features/combat/detail/hvh_math.hpp"

namespace hvh_antiaim
{
    enum class YawMode { Backward = 0, Spin = 1 };
    enum class PitchMode { Keep = 0, Down = 1, Up = 2 };
    struct Options
    {
        YawMode yaw{YawMode::Spin};
        PitchMode pitch{PitchMode::Down};
        float yawOffset{}, spinSpeed{360};
    };
    struct Threat {std::size_t index{};float yaw{};double score{},distance{};};
    inline std::optional<float> Bearing(hvh_math::Vec3 origin,hvh_math::Vec3 target) noexcept
    {
        if(!hvh_math::detail::Finite(origin)||!hvh_math::detail::Finite(target))return {};
        const double x=double(target.x)-origin.x,y=double(target.y)-origin.y;
        if(std::hypot(x,y)<=1e-4)return {};
        return static_cast<float>(std::atan2(y,x)*hvh_math::detail::kDegrees);
    }
    // No visibility or firearm gate: enemies behind the camera/walls still
    // provide a body-facing direction. A committed HVH target takes priority.
    inline std::optional<Threat> SelectThreat(const hvh_math::View& view,std::span<const hvh_math::Candidate> candidates,
        std::uintptr_t preferredPawn=0,std::uint32_t preferredHandle=0xffffffffu) noexcept
    {
        using namespace hvh_math::detail;
        if(!view.localPawn||!Handle(view.localHandle)||(view.localTeam!=2&&view.localTeam!=3)||
           !Finite(view.eye)||!Finite(view.angles))return {};
        std::optional<Threat> best;
        for(std::size_t i=0;i<candidates.size();++i){const auto& c=candidates[i];
            if(!c.valid||!c.pawn||c.pawn==view.localPawn||!Handle(c.handle)||c.handle==view.localHandle||
               c.health<=0||c.dormant||c.immune||(c.team!=2&&c.team!=3)||c.team==view.localTeam)continue;
            const auto yaw=Bearing(view.eye,c.point);if(!yaw)continue;
            const auto delta=Convert(c.point)-Convert(view.eye);
            const double distance=std::sqrt(Dot(delta,delta));
            if(!std::isfinite(distance)||distance>32768)continue;
            const double score=std::abs(std::remainder(double(*yaw)-view.angles.y,360.));
            const Threat next{i,*yaw,score,distance};
            if(c.pawn==preferredPawn&&c.handle==preferredHandle)return next;
            if(!best||score<best->score||(score==best->score&&(distance<best->distance||
               (distance==best->distance&&c.handle<candidates[best->index].handle))))best=next;
        }
        return best;
    }

    // Angle policy only: the caller owns shot/use/movement gates, movement
    // correction and visible-camera preservation. Native command time makes
    // the spin phase independent of render rate and repeated callback passes.
    inline std::optional<hvh_math::Vec3> Angles(hvh_math::Vec3 view, int commandTick,
                                               const Options& options,
                                               float tickInterval = 1.f / 64.f,
                                               std::optional<float> targetYaw = std::nullopt) noexcept
    {
        if (!hvh_math::detail::Finite(view) || commandTick < 0 ||
            !std::isfinite(tickInterval) || tickInterval <= 0 || tickInterval > 1 ||
            !std::isfinite(options.yawOffset) || std::abs(options.yawOffset) > 180 ||
            !std::isfinite(options.spinSpeed) || options.spinSpeed < 30 || options.spinSpeed > 1440 ||
            (options.yaw != YawMode::Backward && options.yaw != YawMode::Spin) ||
            (options.pitch != PitchMode::Keep && options.pitch != PitchMode::Down && options.pitch != PitchMode::Up)) return {};
        // Backward always turns the player around: 180 deg away from the
        // threat bearing when a living enemy is known, otherwise 180 deg
        // away from the visible camera. Pitch Down (89, looking at the
        // floor) tilts the head down/back so enemies standing in front see
        // torso instead of head.
        float pitch = std::clamp(view.x, -89.f, 89.f);
        if (options.pitch == PitchMode::Down) pitch = 89;
        else if (options.pitch == PitchMode::Up) pitch = -89;
        const double phase = options.yaw == YawMode::Backward ? 180. :
            std::fmod(static_cast<double>(commandTick) * tickInterval * options.spinSpeed, 360.);
        double basis=view.y;
        if(options.yaw==YawMode::Backward&&targetYaw&&std::isfinite(*targetYaw))basis=*targetYaw;
        const double yaw = std::remainder(std::remainder(basis, 360.) +
                                          options.yawOffset + phase, 360.);
        if (!std::isfinite(yaw)) return {};
        return hvh_math::Vec3{pitch, static_cast<float>(yaw), 0};
    }
}
