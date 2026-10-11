#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <type_traits>

// Portable command planning adapted from Spectral's nine movement features.
// Native state capture, hull tracing and transactional protobuf writes belong
// to the caller. No camera, entity or weapon state is changed by this header.
namespace movement_policy {
struct Vec2 { float x{}, y{}; };
struct Vec3 { float x{}, y{}, z{}; };
inline constexpr std::uint64_t Jump=1ull<<1, Duck=1ull<<2, Forward=1ull<<3,
    Back=1ull<<4, Use=1ull<<5, MoveLeft=1ull<<9, MoveRight=1ull<<10, Sprint=1ull<<16;
inline constexpr std::uint64_t DirectionButtons=Forward|Back|MoveLeft|MoveRight;
struct Subtick {
    float when{};
    std::uint64_t button{};
    bool pressed{};
    float forwardDelta{}, leftDelta{}, yawDelta{}, pitchDelta{};
};
struct Trace { float fraction{}; Vec3 normal{}, position{}; };
struct Input {
    bool valid{}, onGround{}, quantizeMovement{}, autoBunnyhop{}, shooting{};
    // The adapter observes post-original predicted flags. A fresh native jump
    // edge must survive even if that prediction already left the ground.
    bool jumpPressed{};
    bool airValid{}, groundValid{}, traceValid{};
    int moveType{2}, commandNumber{};
    std::uint64_t buttons{};
    float viewPitch{}, viewYaw{}, forward{}, left{}, interval{1.f/64.f}, maxSubtickWhen{};
    Vec3 velocity{}, origin{}, mins{}, maxs{};
    Vec2 lastImpulses{};
    float duckAmount{}, duckSpeed{}, gravityScale{}, gravity{}, standableNormal{},
        maxSpeed{}, maxServerSpeed{}, airAccelerate{}, airMaxWishspeed{}, staminaRecovery{},
        friction{}, stopSpeed{}, accelerate{}, surfaceFriction{}, stamina{};
    float weaponMaxSpeed{std::numeric_limits<float>::quiet_NaN()};
};
struct Options {
    bool bhop{}, airStrafe{}, fullyDirectional{true}, jumpbug{}, fastLadder{},
        edgeJump{}, edgeStop{}, edgebug{}, slowWalk{}, testStrafer{};
    float slowwalkPercent{33.f};
    int edgebugMode{1}, edgebugPasses{1};
    bool edgebugJumpSteps{};
};
struct Plan {
    std::optional<Vec2> analog;
    std::optional<Vec3> angles;
    std::uint64_t clearButtons{}, setButtons{};
    std::array<Subtick,64> steps{};
    std::size_t count{};
    bool cameraPreservation{}, jumpbugActive{}, edgebugActive{}, testStraferHandled{};
    bool Changed() const noexcept { return analog || angles || clearButtons || setButtons || count; }
};
struct DirectionState {
    std::uint64_t previous{}, pressed{};
    void Reset() noexcept { *this={}; }
    std::uint64_t Update(std::uint64_t buttons) noexcept {
        for(const auto button:{MoveLeft,MoveRight,Forward,Back}) {
            const auto opposite=button==MoveLeft?MoveRight:button==MoveRight?MoveLeft:button==Forward?Back:Forward;
            if((buttons&button)&&(!(previous&button)||!(pressed&opposite))) {
                pressed=(pressed&~opposite)|button;
            } else if(!(buttons&button)) pressed&=~button;
        }
        previous=buttons;
        return pressed;
    }
};
namespace detail {
inline constexpr double Rad=std::numbers::pi/180., Deg=180./std::numbers::pi;
inline bool Finite(Vec3 v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
inline bool Finite(Vec2 v) { return std::isfinite(v.x)&&std::isfinite(v.y); }
inline float Yaw(double yaw) { return static_cast<float>(std::remainder(yaw,360.)); }
inline float Speed(Vec3 v) { return static_cast<float>(std::hypot(double(v.x),double(v.y))); }
inline bool Positive(float n) { return std::isfinite(n)&&n>0; }
inline bool Nonnegative(float n) { return std::isfinite(n)&&n>=0; }
inline bool Valid(const Input& in) {
    return in.valid&&in.commandNumber>0&&Finite(in.velocity)&&Finite(Vec2{in.forward,in.left})&&
        std::abs(in.forward)<=1&&std::abs(in.left)<=1&&std::isfinite(in.viewPitch)&&std::isfinite(in.viewYaw)&&
        Positive(in.interval)&&in.interval<=.1f&&std::isfinite(in.maxSubtickWhen)&&in.maxSubtickWhen>=0&&in.maxSubtickWhen<=1&&
        std::abs(in.velocity.x)<=1e6f&&std::abs(in.velocity.y)<=1e6f&&std::abs(in.velocity.z)<=1e6f;
}
inline bool AirValid(const Input& in) {
    return in.airValid&&Positive(in.maxSpeed)&&in.maxSpeed<=10000&&
        Nonnegative(in.airAccelerate)&&in.airAccelerate<=10000&&Positive(in.airMaxWishspeed)&&in.airMaxWishspeed<=10000&&
        Nonnegative(in.surfaceFriction)&&in.surfaceFriction<=10&&Nonnegative(in.stamina)&&in.stamina<=10000&&
        Nonnegative(in.staminaRecovery)&&in.staminaRecovery<=10000&&Nonnegative(in.gravity)&&in.gravity<=10000&&Finite(in.lastImpulses);
}
inline bool GroundValid(const Input& in) {
    return in.groundValid&&Positive(in.friction)&&in.friction<=100&&Positive(in.stopSpeed)&&in.stopSpeed<=10000&&
        Positive(in.accelerate)&&in.accelerate<=10000&&Positive(in.surfaceFriction)&&in.surfaceFriction<=10&&
        Positive(in.maxSpeed)&&in.maxSpeed<=10000;
}
inline bool HullValid(const Input& in) {
    if(!in.traceValid||!Finite(in.origin)||!Finite(in.mins)||!Finite(in.maxs)||
        !Nonnegative(in.gravity)||in.gravity>10000||!Nonnegative(in.gravityScale)||in.gravityScale>100||
        !Positive(in.standableNormal)||in.standableNormal>1||!Nonnegative(in.duckAmount)||in.duckAmount>1) return false;
    for(const auto v:{in.origin.x,in.origin.y,in.origin.z}) if(std::abs(v)>1e7f) return false;
    return in.mins.x<in.maxs.x&&in.mins.y<in.maxs.y&&in.mins.z<in.maxs.z&&
        std::abs(in.mins.x)<=256&&std::abs(in.mins.y)<=256&&std::abs(in.mins.z)<=256&&
        std::abs(in.maxs.x)<=256&&std::abs(in.maxs.y)<=256&&std::abs(in.maxs.z)<=256;
}
inline bool TraceValid(const Trace& t) {
    if(!std::isfinite(t.fraction)||t.fraction<0||t.fraction>1||!Finite(t.normal)||!Finite(t.position)) return false;
    if(std::abs(t.position.x)>1e7f||std::abs(t.position.y)>1e7f||std::abs(t.position.z)>1e7f)return false;
    const double norm=double(t.normal.x)*t.normal.x+double(t.normal.y)*t.normal.y+double(t.normal.z)*t.normal.z;
    return t.fraction==1||(norm>.25&&norm<1.5);
}
inline bool Add(Plan& out,Subtick step) {
    if(out.count>=out.steps.size()||!std::isfinite(step.when)||step.when<0||step.when>=1||
        !std::isfinite(step.forwardDelta)||!std::isfinite(step.leftDelta)||!std::isfinite(step.yawDelta)||!std::isfinite(step.pitchDelta)) return false;
    out.steps[out.count++]=step;return true;
}
inline Vec2 Against(Vec3 velocity,float yaw,float magnitude) {
    const double relative=(std::atan2(double(velocity.y),double(velocity.x))-double(yaw)*Rad);
    return {static_cast<float>(-std::cos(relative)*magnitude),static_cast<float>(-std::sin(relative)*magnitude)};
}
inline float StopSpeed(const Input& in) {
    return Positive(in.weaponMaxSpeed)&&in.weaponMaxSpeed<=10000?in.weaponMaxSpeed:in.maxSpeed;
}
inline std::optional<Vec2> CounterSteer(const Input& in) {
    if(!Valid(in)||!GroundValid(in)||!in.onGround||in.moveType!=2)return {};
    const float speed=Speed(in.velocity),maxSpeed=StopSpeed(in);
    const float accel=in.accelerate*maxSpeed*in.surfaceFriction*in.interval;
    const float magnitude=speed<accel?speed/maxSpeed:1;
    return Against(in.velocity,in.viewYaw,std::clamp(magnitude,0.f,1.f));
}
inline void Directions(Plan& out,Vec2 movement) {
    out.clearButtons|=DirectionButtons;
    out.setButtons&=~DirectionButtons;
    if(movement.x>0)out.setButtons|=Forward;else if(movement.x<0)out.setButtons|=Back;
    if(movement.y>0)out.setButtons|=MoveLeft;else if(movement.y<0)out.setButtons|=MoveRight;
}
inline float Offset(std::uint64_t pressed) {
    float offset{};
    if(pressed&MoveLeft)offset+=90;if(pressed&MoveRight)offset-=90;
    if(pressed&Forward)offset*=.5f;else if(pressed&Back)offset=-offset*.5f+180;
    return offset;
}
inline Vec2 Slow(Vec2 move,float percent) {
    const double length=std::hypot(double(move.x),double(move.y));
    if(length<.001)return move;
    const double scale=std::min(length,double(percent)/100.)/length;
    return {static_cast<float>(move.x*scale),static_cast<float>(move.y*scale)};
}
template<class NativeTrace>
struct Tracer {
    const Input& input;
    NativeTrace& native;
    unsigned count{};
    std::optional<Trace> operator()(Vec3 start,Vec3 end,Vec3 maxs) {
        if(count++>=64||!Finite(start)||!Finite(end))return {};
        const auto hit=native(start,end,input.mins,maxs);
        return hit&&TraceValid(*hit)?hit:std::nullopt;
    }
    std::optional<Trace> Ground(Vec3 at,float depth=4) {
        return (*this)({at.x,at.y,at.z+2},{at.x,at.y,at.z-depth},input.maxs);
    }
    std::optional<Trace> Landing() {
        auto start=input.origin,maxs=input.maxs;
        if((input.buttons&Duck)&&input.duckAmount>0) {
            start.z-=(72.f-maxs.z)*.5f;maxs.z=72;
        }
        auto velocity=input.velocity;
        velocity.z-=input.gravityScale*input.gravity*input.interval*.5f;
        const Vec3 end{start.x+velocity.x*input.interval,start.y+velocity.y*input.interval,start.z+velocity.z*input.interval-2};
        return (*this)(start,end,maxs);
    }
};
inline bool LandingContact(const Input& in,const Trace& t) {
    const double dot=double(in.velocity.x)*t.normal.x+double(in.velocity.y)*t.normal.y;
    return t.fraction>0&&t.fraction<1&&t.normal.z>=in.standableNormal&&(t.normal.z>=.98f||dot>=0);
}
inline bool EdgeMode(int mode,bool jump,float speed,float vz) {
    if(mode<0||mode>4)return false;
    return mode<2||(mode==2&&!jump)||(mode==3&&speed>15)||(mode==4&&speed>25&&vz<-100);
}
inline void Pulse(Plan& out,std::uint64_t button,bool pressed,float when) { Add(out,{when,button,pressed}); }
inline void AirPath(const Input& in,std::uint64_t pressed,bool fullyDirectional,Plan& out) {
    if(!AirValid(in))return;
    auto velocity=in.velocity;
    auto last=in.lastImpulses;
    if(in.buttons&Sprint) {
        out.analog=Speed(velocity)>10?Against(velocity,in.viewYaw,1):Vec2{};
        Add(out,{0,0,false,out.analog->x-last.x,out.analog->y-last.y});return;
    }
    bool side=last.y>0;
    if(last.y==0)side=(in.commandNumber&1)!=0;
    const float dt=in.interval/32;
    float stamina=in.stamina;
    for(unsigned i=0;i<32;++i) {
        float speed=Speed(velocity);
        if(stamina>0) {
            const float scale=std::clamp(1.f-stamina/100.f,0.f,1.f);speed*=scale*scale;
            stamina=std::max(stamina-dt*in.staminaRecovery,0.f);
        }
        velocity.z-=in.gravity*dt;
        velocity.z+=in.gravity*dt*.5f;
        const float targetYaw=Yaw(double(in.viewYaw)+(fullyDirectional?Offset(pressed):0));
        const double targetRotation=(double(targetYaw)-in.viewYaw)*Rad;
        Vec2 move{static_cast<float>(std::cos(targetRotation)),static_cast<float>(std::sin(targetRotation))};
        if(speed>=10) {
            const float velocityYaw=static_cast<float>(std::atan2(double(velocity.y),double(velocity.x))*Deg);
            const float acceleration=in.airAccelerate*in.maxSpeed*dt*in.surfaceFriction;
            const float wishCap=std::min(in.maxSpeed,in.airMaxWishspeed);
            const float projection=acceleration>=wishCap?wishCap*.5f:wishCap-acceleration;
            const float theta=static_cast<float>(std::acos(std::clamp(projection/std::max(speed,.001f),-1.f,1.f))*Deg);
            const float delta=Yaw(double(targetYaw)-velocityYaw);
            const bool turnLeft=std::abs(delta)>theta*.01f?delta>0:side;
            const float wishYaw=velocityYaw+(turnLeft?theta:-theta);
            const double rotation=(double(wishYaw)-in.viewYaw)*Rad;
            // Choose the world wish direction first. The old negated lateral
            // transform could point behind velocity and brake off-axis input.
            move={static_cast<float>(std::cos(rotation)),static_cast<float>(std::sin(rotation))};
        }
        out.analog=move;
        Add(out,{static_cast<float>(i)/32,0,false,move.x-last.x,move.y-last.y});
        const double yaw=double(in.viewYaw)*Rad;
        const double wx=std::cos(yaw)*move.x-std::sin(yaw)*move.y;
        const double wy=std::sin(yaw)*move.x+std::cos(yaw)*move.y;
        const double add=std::min(double(in.maxSpeed),double(in.airMaxWishspeed))-(velocity.x*wx+velocity.y*wy);
        if(add>0){const double gain=std::min(double(in.airAccelerate)*in.maxSpeed*dt*in.surfaceFriction,add);
            velocity.x+=static_cast<float>(wx*gain);velocity.y+=static_cast<float>(wy*gain);}
        last=move;side=!side;
    }
}
inline void QuantizedPath(const Input& in,std::uint64_t pressed,Plan& out) {
    if(!in.quantizeMovement||!AirValid(in)||
        (in.buttons&Sprint)||!(pressed&DirectionButtons)||in.maxSubtickWhen>=.99f)return;
    const auto before=out.count;
    const float directionOffset=Offset(pressed);
    const float target=Yaw(double(in.viewYaw)+directionOffset);
    auto velocity=in.velocity;
    float accumulated=in.viewYaw;
    const float step=(1-in.maxSubtickWhen)/17,dt=in.interval*step;
    for(unsigned i=1;i<=16;++i) {
        const float speed=Speed(velocity),accel=in.maxSpeed*in.airAccelerate*in.surfaceFriction*dt;
        const float denominator=std::max(speed,.001f);
        const float cosine=std::clamp(accel>=in.airMaxWishspeed?in.airMaxWishspeed/(2*denominator):(in.airMaxWishspeed-accel)/denominator,-1.f,1.f);
        const float theta=std::max(static_cast<float>(std::acos(cosine)*Deg),1.f);
        const float velocityYaw=static_cast<float>(std::atan2(double(velocity.y),double(velocity.x))*Deg);
        const float delta=Yaw(double(target)-velocityYaw);
        const bool side=(static_cast<unsigned>(in.commandNumber)+i)%2==0;
        const float wish=speed<15?target:Yaw(double(velocityYaw)+((std::abs(delta)>2?delta>0:side)?theta:-theta));
        const float next=Yaw(double(wish)-directionOffset),yawDelta=Yaw(double(next)-accumulated);
        if(!Add(out,{in.maxSubtickWhen+i*step,0,false,0,0,yawDelta,0}))break;
        accumulated=next;
        const double x=std::cos(double(wish)*Rad),y=std::sin(double(wish)*Rad);
        const double add=std::min(in.maxSpeed,in.airMaxWishspeed)-(velocity.x*x+velocity.y*y);
        if(add>0) {
            const double gain=std::min(double(in.maxSpeed)*in.airAccelerate*in.surfaceFriction*dt,add);
            velocity.x+=static_cast<float>(x*gain);velocity.y+=static_cast<float>(y*gain);
        }
    }
    out.testStraferHandled=out.count>before;
    out.cameraPreservation=out.testStraferHandled;
    if(out.testStraferHandled)out.angles=Vec3{in.viewPitch,in.viewYaw,0};
}
} // namespace detail

template<class NativeTrace>
Plan Build(const Input& in,const Options& options,DirectionState& directions,NativeTrace&& nativeTrace) {
    using namespace detail;
    Plan out;
    if(!Valid(in)||in.moveType==7)return out;
    const auto pressed=directions.Update(in.buttons);
    if(in.moveType==9) {
        if(!options.fastLadder||in.shooting||(in.buttons&Use)||(in.forward==0&&in.left==0))return out;
        const bool up=std::abs(in.forward)>.01f?in.forward>0:in.viewPitch<0;
        out.angles=Vec3{89,Yaw(double(in.viewYaw)+(up?-90:90)),0};
        out.analog=Vec2{-1,up?1.f:-1.f};Directions(out,*out.analog);out.cameraPreservation=true;return out;
    }
    if(in.moveType!=2)return out;
    if(options.slowWalk&&in.onGround&&std::isfinite(options.slowwalkPercent)&&options.slowwalkPercent>=0&&options.slowwalkPercent<=100)
        out.analog=Slow({in.forward,in.left},options.slowwalkPercent);
    Tracer<std::remove_reference_t<NativeTrace>> trace{in,nativeTrace};
    std::optional<Trace> landing;
    const bool needsLanding=!in.jumpPressed&&(options.edgebug||((in.buttons&Jump)&&(options.jumpbug||(options.bhop&&!in.autoBunnyhop))));
    if(HullValid(in)&&!in.onGround&&in.velocity.z<=0&&needsLanding)landing=trace.Landing();
    if(options.edgebug&&landing&&LandingContact(in,*landing)&&EdgeMode(options.edgebugMode,(in.buttons&Jump)!=0,Speed(in.velocity),in.velocity.z)&&
        options.edgebugPasses>=1&&options.edgebugPasses<=5) {
        bool geometry=options.edgebugMode==0;
        if(!geometry) {
            const float speed=Speed(in.velocity),px=speed>.01f?-in.velocity.y/speed:1,py=speed>.01f?in.velocity.x/speed:0;
            const float distance=std::max(14.f,std::max(std::abs(in.maxs.x),std::abs(in.maxs.y))*1.25f);
            const Vec3 center{landing->position.x,landing->position.y,in.origin.z};
            const auto left=trace.Ground({center.x-px*distance,center.y-py*distance,center.z});
            const auto right=trace.Ground({center.x+px*distance,center.y+py*distance,center.z});
            if(left&&right) {
                const auto ground=[&](const Trace& t){return t.fraction>0&&t.fraction<1&&t.normal.z>=in.standableNormal;};
                geometry=!(ground(*left)&&ground(*right));
            }
        }
        if(geometry) {
            const float when=std::clamp(landing->fraction,.001f,.99f),segment=when/options.edgebugPasses;
            for(int p=0;p<options.edgebugPasses;++p) {Pulse(out,Duck,true,segment*p);Pulse(out,Duck,false,segment*(p+1));}
            if(options.edgebugJumpSteps&&(in.buttons&Jump)) {Pulse(out,Jump,false,when);Pulse(out,Jump,true,when);}
            out.edgebugActive=true;
        }
    }
    if(!out.edgebugActive&&options.jumpbug&&(in.buttons&Jump)&&landing&&LandingContact(in,*landing)&&landing->normal.z>=.985f) {
        const float when=std::clamp(landing->fraction,.001f,.99f);
        Pulse(out,Duck,true,0);Pulse(out,Duck,false,when);Pulse(out,Jump,false,when);Pulse(out,Jump,true,when);
        out.jumpbugActive=true;
    }
    if(!in.jumpPressed&&HullValid(in)&&in.onGround&&Speed(in.velocity)>=1&&(options.edgeJump||options.edgeStop)) {
        const auto at=[&](int ticks) {
            return Vec3{in.origin.x+in.velocity.x*in.interval*ticks,in.origin.y+in.velocity.y*in.interval*ticks,in.origin.z};
        };
        const auto edge=[&](const Trace& t){return t.fraction>=1||t.normal.z<in.standableNormal;};
        const auto first=trace.Ground(at(1));
        if(first&&options.edgeJump) {
            const auto second=trace.Ground(at(2));
            if(second&&!edge(*first)&&edge(*second)) {
                bool stair=false,complete=true;
                const float speed=Speed(in.velocity),px=-in.velocity.y/speed,py=in.velocity.x/speed;
                for(int tick=1;tick<=2&&!stair;++tick)for(float lateral:{0.f,7.f,-7.f,14.f,-14.f,22.f,-22.f,34.f,-34.f}) {
                    auto where=at(tick);where.x+=px*lateral;where.y+=py*lateral;
                    const auto found=trace.Ground(where,110);
                    if(!found){complete=false;break;}
                    const float down=in.origin.z-found->position.z;
                    if(!edge(*found)&&down>=.12f&&down<=48){stair=true;break;}
                }
                if(complete&&!stair)out.setButtons|=Jump;
            }
        } else if(first&&options.edgeStop&&GroundValid(in)) {
            const float speed=Speed(in.velocity);
            const double drop=double(in.friction)*in.surfaceFriction*std::max(speed,in.stopSpeed)*in.interval;
            const int ticks=std::clamp(static_cast<int>(std::min(std::ceil(speed/drop),30.)),2,30);
            bool approaching=edge(*first);
            for(int tick=2;!approaching&&tick<=ticks+2;++tick) {const auto hit=trace.Ground(at(tick));if(!hit)break;approaching=edge(*hit);}
            if(approaching) {
                bool pushingBack=false;
                if(edge(*first)) {
                    const auto move=out.analog.value_or(Vec2{in.forward,in.left});
                    const double yaw=double(in.viewYaw)*Rad;
                    const double wx=std::cos(yaw)*move.x-std::sin(yaw)*move.y,wy=std::sin(yaw)*move.x+std::cos(yaw)*move.y;
                    const double length=std::hypot(wx,wy);
                    if(length>.001) {
                        const auto ahead=trace.Ground({in.origin.x+static_cast<float>(wx/length*4),in.origin.y+static_cast<float>(wy/length*4),in.origin.z});
                        if(ahead&&edge(*ahead)) {
                            out.analog=Against({static_cast<float>(wx),static_cast<float>(wy),0},in.viewYaw,.3f);
                            Directions(out,*out.analog);pushingBack=true;
                        }
                    }
                }
                if(!pushingBack||speed>5) {
                    out.analog=CounterSteer(in);if(out.analog)Directions(out,*out.analog);
                }
            }
        }
    }
    // A landing exploit owns its duck/jump window. Do not append an ordinary
    // bhop or strafe window that fights the successful geometry-based plan.
    if(out.edgebugActive||out.jumpbugActive)return out;
    // A post-original snapshot can first observe a landing after its airborne
    // hull forecast was missed. Rearm held jump on the real grounded state;
    // preserve fresh manual presses, rising takeoffs and server autobhop.
    if(options.bhop&&!in.autoBunnyhop&&in.onGround&&!in.jumpPressed&&in.velocity.z<=0&&(in.buttons&Jump)) {
        Pulse(out,Jump,false,0);Pulse(out,Jump,true,1.f/64.f);
    }
    // Unlike Spectral's pre-prediction flags, this adapter sees a takeoff
    // already predicted by the original call. Preserve held rising hops too;
    // release at the apex/descent before scheduling the next landing pulse.
    if(options.bhop&&!in.autoBunnyhop&&!in.onGround&&!in.jumpPressed&&in.velocity.z<=0&&(in.buttons&Jump)) {
        out.clearButtons|=Jump;
        if(landing&&landing->fraction>0&&landing->fraction<1&&landing->normal.z>=in.standableNormal) {
            const float when=std::clamp(std::round(landing->fraction*64)/64,1.f/64,63.f/64);
            const float release=std::clamp(when-1.f/64,1.f/64,63.f/64);
            if(release<when)Pulse(out,Jump,false,release);Pulse(out,Jump,true,when);
        }
    }
    if(!in.onGround&&!in.shooting&&!(in.buttons&Use)) {
        const auto before=out.count;
        if(options.testStrafer) {QuantizedPath(in,pressed,out);out.testStraferHandled=out.count>before;}
        if(options.airStrafe&&!out.testStraferHandled&&in.quantizeMovement&&!(in.buttons&Sprint)) {
            // Quantized servers discard the small forward component used by
            // the analog path. Use digital movement plus yaw subticks instead,
            // preserving the visible camera through the native view hook.
            auto quantized=in;
            const auto steering=options.fullyDirectional&&(pressed&DirectionButtons)?pressed:Forward;
            quantized.forward=(steering&Forward)?1.f:(steering&Back)?-1.f:0.f;
            quantized.left=(steering&MoveLeft)?1.f:(steering&MoveRight)?-1.f:0.f;
            QuantizedPath(quantized,steering,out);
            if(out.testStraferHandled){
                out.analog=Vec2{quantized.forward,quantized.left};Directions(out,*out.analog);
                // Rebuilds may retain an earlier AA base angle. Yaw deltas
                // below start from the visible input angle, so anchor it too.
                out.angles=Vec3{in.viewPitch,in.viewYaw,0};
            }
        }
        if(options.airStrafe&&!out.testStraferHandled)AirPath(in,pressed,options.fullyDirectional,out);
    }
    return out;
}
inline Plan Build(const Input& input,const Options& options,DirectionState& directions) {
    return Build(input,options,directions,[](Vec3,Vec3,Vec3,Vec3)->std::optional<Trace>{return {};});
}
} // namespace movement_policy
