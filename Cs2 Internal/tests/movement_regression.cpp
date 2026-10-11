#include "features/movement/detail/movement_policy.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace movement_policy;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool Near(double a,double b,double epsilon=1e-5){return std::abs(a-b)<=epsilon;}
Input State(){
    Input in;in.valid=true;in.commandNumber=77;in.velocity={250,0,-100};in.origin={100,20,100};
    in.mins={-16,-16,0};in.maxs={16,16,72};in.traceValid=in.airValid=in.groundValid=true;
    in.gravity=800;in.gravityScale=1;in.standableNormal=.7f;in.maxSpeed=250;in.maxServerSpeed=320;
    in.airAccelerate=12;in.airMaxWishspeed=30;in.staminaRecovery=60;in.surfaceFriction=1;
    in.friction=4;in.stopSpeed=80;in.accelerate=5.5f;return in;
}
struct Request{Vec3 start,end,mins,maxs;};
struct Script {
    std::vector<Trace> replies;
    std::vector<Request> calls;
    std::optional<Trace> operator()(Vec3 start,Vec3 end,Vec3 mins,Vec3 maxs){
        calls.push_back({start,end,mins,maxs});
        if(calls.size()>replies.size())return {};
        return replies[calls.size()-1];
    }
};
Trace Floor(float fraction=.5f,float z=100){return {fraction,{0,0,1},{103,20,z}};}
Trace Clear(){return {1,{},{}};}
void SafePlan(const Plan& plan){
    Check(plan.count<=64,"Unbounded subtick plan");
    if(plan.analog)Check(std::isfinite(plan.analog->x)&&std::isfinite(plan.analog->y)&&std::abs(plan.analog->x)<=1.000001f&&std::abs(plan.analog->y)<=1.000001f,"Unbounded analog movement");
    Check(((plan.clearButtons|plan.setButtons)&((1ull<<0)|(1ull<<11)|Use))==0,"Movement changed attack/use buttons");
    for(std::size_t i=0;i<plan.count;++i){const auto& step=plan.steps[i];Check(step.when>=0&&step.when<1&&std::isfinite(step.when)&&std::isfinite(step.forwardDelta)&&std::isfinite(step.leftDelta)&&std::isfinite(step.yawDelta),"Malformed subtick");}
}
void BhopAndLanding(){
    auto in=State();Options opt;opt.bhop=true;in.buttons=Jump|(1ull<<0)|(1ull<<11);DirectionState direction;
    const auto missing=Build(in,opt,direction);Check(missing.clearButtons==Jump&&!missing.count,"Airborne jump suppression needs native tracing");SafePlan(missing);
    in.onGround=true;in.jumpPressed=true;Check(!Build(in,opt,direction).Changed(),"Bhop suppressed a fresh grounded jump");
    in.onGround=false;in.jumpPressed=false;in.autoBunnyhop=true;Script unused;Check(!Build(in,opt,direction,unused).Changed()&&unused.calls.empty(),"Server auto-bhop was overridden");
    in.autoBunnyhop=false;Script trace{{Floor(.51f)}};const auto plan=Build(in,opt,direction,trace);
    Check(plan.count==2&&plan.steps[0].button==Jump&&!plan.steps[0].pressed&&plan.steps[1].pressed,"Landing jump lacks release/press edge");
    Check(Near(plan.steps[1].when,33.f/64)&&Near(plan.steps[0].when,32.f/64),"Landing fraction lost native 1/64 quantization");
    Check(trace.calls.size()==1&&Near(trace.calls[0].end.x,103.90625)&&Near(trace.calls[0].end.z,96.33984375),"Landing sweep did not apply half gravity and ground margin");
    in.buttons|=Duck;in.duckAmount=.5f;in.maxs.z=54;Script crouch{{Floor(.51f)}};Build(in,opt,direction,crouch);
    Check(Near(crouch.calls[0].start.z,91)&&crouch.calls[0].maxs.z==72,"Crouched landing sweep did not expand the standing hull");
    in.velocity.z=100;Script rising;const auto up=Build(in,opt,direction,rising);Check(!up.Changed()&&rising.calls.empty(),"Predicted held rising hop was suppressed or caused a landing sweep");
}
void ManualJumpOwnership(){
    auto in=State();in.buttons=Jump|Forward;in.forward=1;in.jumpPressed=true;
    Options options;options.bhop=options.jumpbug=options.edgebug=options.edgeJump=options.edgeStop=true;
    options.edgebugMode=0;options.edgebugJumpSteps=true;DirectionState direction;
    const auto ownsJump=[](const Plan& plan){
        if((plan.clearButtons|plan.setButtons)&Jump)return true;
        for(std::size_t i=0;i<plan.count;++i)if(plan.steps[i].button&Jump)return true;
        return false;
    };
    // A native command may already have predicted the manual takeoff. Current
    // flags/velocity alone therefore cannot distinguish its fresh edge from a
    // held Space in midair (Spectral had a prediction.pre() flags snapshot).
    for(const bool grounded:{true,false})for(const float vz:{-100.f,0.f,280.f}){
        in.onGround=grounded;in.velocity.z=vz;Script native{{Floor(.4f),Clear(),Clear()}};
        const auto plan=Build(in,options,direction,native);
        Check(!ownsJump(plan)&&!plan.jumpbugActive&&!plan.edgebugActive&&native.calls.empty(),
            "Fresh manual takeoff was replaced by bhop/landing/ground-edge automation");
        Check(!plan.Changed(),"Jump-only assists edited an explicit fresh manual press");
        const auto replay=Build(in,options,direction,native);
        Check(!replay.Changed()&&native.calls.empty(),"Same-command fresh jump replay became a held suppression");
        SafePlan(plan);
    }
    // Held-space landing hops can already be predicted rising without a new
    // hardware edge. Retain them until apex/descent; then release and schedule
    // the next landing pulse without needing native pre-prediction hooks.
    options.jumpbug=options.edgebug=options.edgeJump=options.edgeStop=false;
    in.onGround=false;in.jumpPressed=false;in.velocity.z=280;
    Script rising;const auto held=Build(in,options,direction,rising);
    Check(!held.Changed()&&rising.calls.empty(),"Already-predicted held-space rising hop was suppressed");
    const auto risingReplay=Build(in,options,direction,rising);
    Check(!risingReplay.Changed()&&rising.calls.empty(),"Held rising takeoff replay cleared jump");
    in.velocity.z=0;Script apex{{Clear()}};const auto apexRelease=Build(in,options,direction,apex);
    Check(apexRelease.clearButtons==Jump&&!apexRelease.count&&apex.calls.size()==1,"Held jump was not released at the apex");
    in.velocity.z=-100;Script descent{{Clear()}};const auto descendingRelease=Build(in,options,direction,descent);
    Check(descendingRelease.clearButtons==Jump&&!descendingRelease.count&&descent.calls.size()==1,"Held descending jump did not release before landing");
    Script landing{{Floor(.51f)}};const auto hop=Build(in,options,direction,landing);
    Check(hop.clearButtons==Jump&&hop.count==2&&!hop.steps[0].pressed&&hop.steps[1].pressed,
        "Fresh-press guard disabled held-jump landing bunnyhop");
    Script replayLanding{{Floor(.51f)}};const auto replay=Build(in,options,direction,replayLanding);
    Check(replay.clearButtons==hop.clearButtons&&replay.count==hop.count&&
        replay.steps[0].when==hop.steps[0].when&&replay.steps[1].when==hop.steps[1].when,
        "Held landing plan advanced on same-command replay");
    in.buttons=Forward;Script released;
    Check(!Build(in,options,direction,released).Changed()&&released.calls.empty(),"Released manual jump synthesized another hop");
    options.edgebug=true;options.edgebugMode=0;options.edgebugJumpSteps=true;
    Script releasedEdge{{Floor(.4f)}};const auto duckOnly=Build(in,options,direction,releasedEdge);
    Check(duckOnly.edgebugActive&&!ownsJump(duckOnly),"Edgebug jump-step option synthesized jump after release");

    options={};options.bhop=true;in.buttons=Jump;in.autoBunnyhop=true;
    for(const bool fresh:{false,true}){
        in.jumpPressed=fresh;Script server;
        Check(!Build(in,options,direction,server).Changed()&&server.calls.empty(),"Server autobunnyhop jump was taken over locally");
    }

    // Analog movement remains useful on a manual jump but owns only directional
    // buttons and analog samples. It must preserve both the raw/PB jump and its
    // native press/release events when the runtime commits this plan.
    options={};options.airStrafe=true;in.autoBunnyhop=false;in.jumpPressed=true;in.buttons=Jump|Forward;
    in.velocity={250,0,280};in.onGround=false;in.forward=1;
    const auto air=Build(in,options,direction);
    Check(air.analog&&air.count==32&&!ownsJump(air),"Fresh jump disabled air strafe or lost manual jump ownership");
    for(std::size_t i=0;i<air.count;++i)Check(air.steps[i].button==0,"Air analog window emitted an unrelated button event");
    SafePlan(air);
    options.airStrafe=false;options.slowWalk=true;in.onGround=true;in.velocity.z=0;
    const auto slow=Build(in,options,direction);
    Check(slow.analog&&Near(slow.analog->x,.33)&&!ownsJump(slow),"Slowwalk suppressed a fresh grounded jump");
    SafePlan(slow);
}
void LandingExploitPriority(){
    auto in=State();in.buttons=Jump;Options opt;opt.bhop=opt.airStrafe=opt.jumpbug=true;DirectionState direction;Script jump{{Floor(.6f)}};
    const auto plan=Build(in,opt,direction,jump);Check(plan.jumpbugActive&&plan.count==4&&!plan.clearButtons&&!plan.analog,"Jumpbug fought bhop/air-strafe");
    Check(plan.steps[0].button==Duck&&plan.steps[0].pressed&&plan.steps[0].when==0&&plan.steps[1].button==Duck&&!plan.steps[1].pressed&&plan.steps[2].button==Jump&&!plan.steps[2].pressed&&plan.steps[3].pressed,"Jumpbug subtick order");
    SafePlan(plan);
    opt.edgebug=true;opt.edgebugMode=1;opt.edgebugPasses=3;opt.edgebugJumpSteps=true;Script edge{{Floor(.6f),Floor(.4f),Clear()}};
    const auto bug=Build(in,opt,direction,edge);Check(bug.edgebugActive&&!bug.jumpbugActive&&bug.count==8&&!bug.clearButtons,"Edgebug did not own landing window");
    for(unsigned i=0;i<3;++i)Check(bug.steps[2*i].pressed&&!bug.steps[2*i+1].pressed&&Near(bug.steps[2*i].when,.2*i)&&Near(bug.steps[2*i+1].when,.2*(i+1)),"Edgebug cycles overlap instead of chaining");
    Check(bug.steps[6].button==Jump&&!bug.steps[6].pressed&&bug.steps[7].pressed,"Optional edgebug jump edge");SafePlan(bug);
    opt.bhop=opt.airStrafe=opt.jumpbug=false;Script both{{Floor(.6f),Floor(),Floor()}};Check(!Build(in,opt,direction,both).Changed(),"Edgebug treated a broad floor as an edge");
    opt.edgebugMode=2;Script held;Check(!Build(in,opt,direction,held).Changed(),"No-jump edgebug mode accepted held jump");
    opt.edgebugMode=4;in.buttons=0;in.velocity.z=-100;Script threshold{{Floor()}};Check(!Build(in,opt,direction,threshold).Changed(),"Fast-fall mode accepted exact vertical threshold");
    in.velocity.z=-101;opt.edgebugMode=0;Script noGeometry{{Floor(.4f)}};Check(Build(in,opt,direction,noGeometry).edgebugActive&&noGeometry.calls.size()==1,"Mode zero unexpectedly requires lateral support");
    opt.edgebugMode=1;Script unavailable{{Floor(.4f)}};Check(!Build(in,opt,direction,unavailable).Changed(),"Missing edge-side traces were treated as empty ground");
}
void CombinedJumpActivation(){
    // Exact reported settings: all three jump assists, edge geometry, one
    // duck window, no optional edgebug jump events, and no analog assists.
    Options options;options.bhop=options.jumpbug=options.edgebug=true;
    options.edgebugMode=1;options.edgebugPasses=1;options.edgebugJumpSteps=false;
    auto in=State();in.buttons=Jump;DirectionState direction;
    const auto waiting=[&](bool ground,bool fresh,float vz){
        in.onGround=ground;in.jumpPressed=fresh;in.velocity.z=vz;
        Script trace{{Floor(.5f),Floor(),Clear()}};
        Check(!Build(in,options,direction,trace).Changed()&&trace.calls.empty(),
            "Combined jump assists changed grounded/rising/fresh manual input");
    };
    waiting(true,true,0);waiting(false,false,280);
    waiting(false,true,0);waiting(false,true,-100);
    in.onGround=false;in.jumpPressed=false;in.velocity.z=-100;
    const auto release=[&](const Plan& plan){
        Check(plan.Changed()&&plan.clearButtons==Jump&&!plan.setButtons&&!plan.count&&
            !plan.analog&&!plan.angles&&!plan.jumpbugActive&&!plan.edgebugActive,
            "Combined assists failed to release held descending jump without a landing");
        SafePlan(plan);
    };
    in.traceValid=false;Script noHull;release(Build(in,options,direction,noHull));
    Check(noHull.calls.empty(),"Unavailable collision prerequisites reached native tracing");
    in.traceValid=true;Script failed;release(Build(in,options,direction,failed));
    Check(failed.calls.size()==1,"Held descending jump required a successful hull to release");
    Script clear{{Clear()}};release(Build(in,options,direction,clear));
    Script malformed{{{NAN,{0,0,1},{}}}};release(Build(in,options,direction,malformed));
    in.velocity.z=0;Script apex{{Clear()}};release(Build(in,options,direction,apex));
    in.velocity.z=-100;
    const auto flat=[&](Script& trace){
        const auto plan=Build(in,options,direction,trace);
        Check(plan.Changed()&&plan.jumpbugActive&&!plan.edgebugActive&&plan.count==4&&
            !plan.clearButtons&&!plan.setButtons&&!plan.analog&&!plan.angles,
            "Edge geometry setting suppressed the flat-landing jumpbug fallback");
        Check(plan.steps[0].button==Duck&&plan.steps[0].pressed&&plan.steps[0].when==0&&
            plan.steps[1].button==Duck&&!plan.steps[1].pressed&&Near(plan.steps[1].when,.5)&&
            plan.steps[2].button==Jump&&!plan.steps[2].pressed&&Near(plan.steps[2].when,.5)&&
            plan.steps[3].button==Jump&&plan.steps[3].pressed&&Near(plan.steps[3].when,.5),
            "Combined flat landing lost its ordered duck and jump window");
        SafePlan(plan);return plan;
    };
    Script broad{{Floor(.5f),Floor(),Floor()}};const auto first=flat(broad);
    Script broadReplay{{Floor(.5f),Floor(),Floor()}};const auto again=flat(broadReplay);
    Check(first.count==again.count&&first.steps[3].when==again.steps[3].when,
        "Combined landing replay changed the jump timing");
    Script sideUnavailable{{Floor(.5f)}};flat(sideUnavailable);
    Script edge{{Floor(.5f),Floor(),Clear()}};const auto duck=Build(in,options,direction,edge);
    Check(duck.Changed()&&duck.edgebugActive&&!duck.jumpbugActive&&duck.count==2&&
        !duck.clearButtons&&!duck.setButtons&&!duck.analog&&!duck.angles,
        "Combined edge landing fought its higher-priority edgebug window");
    for(std::size_t i=0;i<duck.count;++i)Check(duck.steps[i].button==Duck,
        "Edgebug emitted jump while its optional jump steps were disabled");
    Check(duck.steps[0].pressed&&duck.steps[0].when==0&&!duck.steps[1].pressed&&Near(duck.steps[1].when,.5),
        "One-window edge geometry setting produced the wrong duck timing");
    SafePlan(duck);
    in.buttons=0;Script releasedBroad{{Floor(.5f),Floor(),Floor()}};
    Check(!Build(in,options,direction,releasedBroad).Changed(),
        "Released jump on a broad floor synthesized another jump");
    in.buttons=Jump;in.autoBunnyhop=true;Script serverLanding{{Floor(.5f),Floor(),Floor()}};flat(serverLanding);
    Script serverClear{{Clear()}};Check(!Build(in,options,direction,serverClear).Changed(),
        "Server autobunnyhop did not retain ownership away from a landing exploit");
}
void HeldJumpCycles(){
    Options options;options.bhop=options.jumpbug=options.edgebug=true;
    options.edgebugMode=1;options.edgebugPasses=1;options.edgebugJumpSteps=false;
    auto in=State();in.buttons=Jump;in.onGround=true;in.velocity.z=0;in.jumpPressed=true;
    DirectionState direction;
    const auto groundHop=[&]{
        Script unused;const auto plan=Build(in,options,direction,unused);
        Check(plan.Changed()&&plan.count==2&&!plan.clearButtons&&!plan.setButtons&&
            !plan.analog&&!plan.angles&&!plan.jumpbugActive&&!plan.edgebugActive&&unused.calls.empty(),
            "Grounded held jump failed to rearm after a missed landing window");
        Check(plan.steps[0].button==Jump&&!plan.steps[0].pressed&&plan.steps[0].when==0&&
            plan.steps[1].button==Jump&&plan.steps[1].pressed&&Near(plan.steps[1].when,1.f/64),
            "Grounded bunnyhop did not emit ordered release then press");
        SafePlan(plan);return plan;
    };
    // Initial hardware takeoff remains native; holding Space then supplies the
    // following commands. The native integration fixture verifies that capture.
    Script firstPress;Check(!Build(in,options,direction,firstPress).Changed()&&firstPress.calls.empty(),
        "Initial fresh grounded takeoff was replaced by automated input");
    ++in.commandNumber;in.onGround=false;in.jumpPressed=false;in.velocity.z=280;
    Script rising;Check(!Build(in,options,direction,rising).Changed()&&rising.calls.empty(),
        "Held first-hop rise was suppressed");
    ++in.commandNumber;in.velocity.z=0;Script apex{{Clear()}};
    Check(Build(in,options,direction,apex).clearButtons==Jump,"First-hop apex did not release jump");
    ++in.commandNumber;in.velocity.z=-100;Script missed;
    Check(Build(in,options,direction,missed).clearButtons==Jump&&missed.calls.size()==1,
        "Missed descending collision window lost the held jump release");
    ++in.commandNumber;in.onGround=true;in.velocity.z=0;
    const auto repeat=groundHop(),replay=groundHop();
    Check(repeat.count==replay.count&&repeat.steps[0].when==replay.steps[0].when&&
        repeat.steps[1].when==replay.steps[1].when,"Replayed grounded command advanced hop timing");
    ++in.commandNumber;in.onGround=false;in.velocity.z=280;
    Script secondRise;Check(!Build(in,options,direction,secondRise).Changed()&&secondRise.calls.empty(),
        "Grounded rearm suppressed the next predicted rising hop");
    ++in.commandNumber;in.velocity.z=-100;Script nativeAlreadyLanded{{Floor(0)}};
    const auto zeroFraction=Build(in,options,direction,nativeAlreadyLanded);
    Check(zeroFraction.clearButtons==Jump&&!zeroFraction.count,
        "A zero-fraction native landing was mistaken for a usable airborne pulse window");
    ++in.commandNumber;in.onGround=true;in.velocity.z=0;groundHop();
    in.jumpPressed=true;Script fresh;
    Check(!Build(in,options,direction,fresh).Changed()&&fresh.calls.empty(),
        "Ground rearm intercepted a fresh manual press");
    in.jumpPressed=false;in.velocity.z=280;Script predicted;
    Check(!Build(in,options,direction,predicted).Changed()&&predicted.calls.empty(),
        "Grounded positive takeoff velocity generated a second jump pulse");
    in.velocity.z=0;in.buttons=0;Script released;
    Check(!Build(in,options,direction,released).Changed()&&released.calls.empty(),
        "Ground rearm continued after the user released jump");
    in.buttons=Jump;in.autoBunnyhop=true;Script server;
    Check(!Build(in,options,direction,server).Changed()&&server.calls.empty(),
        "Ground rearm took over the server's native autobunnyhop");
}
void GroundEdges(){
    auto in=State();in.onGround=true;in.velocity.z=0;in.forward=1;Options opt;opt.edgeJump=true;DirectionState direction;
    Script cliff;cliff.replies={Floor(),Clear()};for(unsigned i=0;i<18;++i)cliff.replies.push_back(Clear());
    const auto jump=Build(in,opt,direction,cliff);Check(jump.setButtons==Jump&&cliff.calls.size()==20,"Edge jump did not check the two-tick cliff and stairs");
    Check(Near(cliff.calls[0].start.x,103.90625)&&Near(cliff.calls[1].start.x,107.8125),"Edge lookahead is not keyed to native tick interval");
    Script stairs{{Floor(),Clear(),Floor(.5f,80)}};Check(!Build(in,opt,direction,stairs).Changed(),"Edge jump fires on a stair step");
    Script alreadyOff{{Clear(),Clear()}};Check(!Build(in,opt,direction,alreadyOff).Changed(),"Edge jump fired after the current hull already left ground");
    Script missing{{Floor(),Clear()}};Check(!Build(in,opt,direction,missing).Changed(),"Incomplete stair scan was interpreted as a cliff");
    opt.edgeStop=true;Script precedence{{Floor(),Floor()}};Check(!Build(in,opt,direction,precedence).analog,"Edge stop overrode enabled edge jump");
    opt.edgeJump=false;Script stop{{Floor(),Floor(),Clear()}};const auto brake=Build(in,opt,direction,stop);Check(brake.analog&&brake.analog->x<0&&Near(brake.analog->y,0)&&!(brake.setButtons&Jump),"Edge stop failed to oppose forward momentum");
    // Directions include Back, rather than replacing attack/use bits.
    Check((brake.setButtons&Back)!=0,"Brake buttons do not match analog movement");SafePlan(brake);
    in.velocity={3,0,0};Script creep{{Clear(),Clear()}};const auto push=Build(in,opt,direction,creep);Check(push.analog&&Near(push.analog->x,-.3)&&creep.calls.size()==2,"Slow input can creep off a cliff");
    in.friction=0;Script zeroFriction;Check(!Build(in,opt,direction,zeroFriction).Changed()&&zeroFriction.calls.size()<=1,"Zero friction caused unbounded lookahead");
}
void SlowwalkAndLadder(){
    auto in=State();in.onGround=true;in.forward=in.left=1;Options opt;opt.slowWalk=true;opt.slowwalkPercent=33;DirectionState direction;
    const auto slow=Build(in,opt,direction);Check(slow.analog&&Near(std::hypot(slow.analog->x,slow.analog->y),.33)&&Near(slow.analog->x,slow.analog->y),"Diagonal slowwalk exceeds requested speed");
    in.forward=.1f;in.left=0;Check(Near(Build(in,opt,direction).analog->x,.1),"Slowwalk accelerated existing fine input");
    in.onGround=false;Check(!Build(in,opt,direction).Changed(),"Slowwalk changed air movement");
    in.moveType=9;in.forward=1;in.buttons=Forward|(1ull<<0)|(1ull<<11);in.viewYaw=179;opt.fastLadder=true;
    const auto up=Build(in,opt,direction);Check(up.angles&&up.angles->x==89&&up.angles->y==89&&up.cameraPreservation&&up.analog->x==-1&&up.analog->y==1,"Ascending ladder transform");SafePlan(up);
    in.forward=-1;const auto down=Build(in,opt,direction);Check(down.angles&&down.angles->y==-91&&down.analog->y==-1,"Descending ladder yaw wrap");
    in.forward=0;in.left=1;in.viewPitch=-20;Check(Build(in,opt,direction).analog->y==1,"Lateral ladder climb ignores view pitch");
    in.shooting=true;Check(!Build(in,opt,direction).Changed(),"Fastladder rewrote a committed shot angle");
    in.shooting=false;in.buttons|=Use;Check(!Build(in,opt,direction).Changed(),"Fastladder rewrote use interaction");
    in.moveType=7;Check(!Build(in,opt,direction).Changed(),"Movement rewrote noclip");
}
Vec2 IntegrateAir(const Input& in,const Plan& plan,bool yawSteps){
    double vx=in.velocity.x,vy=in.velocity.y,yaw=in.viewYaw,forward=in.lastImpulses.x,left=in.lastImpulses.y;
    const auto move=plan.analog.value_or(Vec2{in.forward,in.left});
    for(std::size_t i=0;i<plan.count;++i){const auto& step=plan.steps[i];forward+=step.forwardDelta;left+=step.leftDelta;yaw+=step.yawDelta;
        const double dt=in.interval*((i+1<plan.count?plan.steps[i+1].when:1)-step.when);
        const double offset=yawSteps?std::atan2(move.y,move.x):0;
        const double rad=yaw*std::numbers::pi/180.+offset;
        double wx=yawSteps?std::cos(rad):std::cos(rad)*forward-std::sin(rad)*left;
        double wy=yawSteps?std::sin(rad):std::sin(rad)*forward+std::cos(rad)*left;
        const double length=std::hypot(wx,wy);if(length<=1e-8)continue;wx/=length;wy/=length;
        const double wish=in.maxSpeed;
        const double add=std::min(wish,double(in.airMaxWishspeed))-(vx*wx+vy*wy);
        if(add>0){const double gain=std::min(wish*in.airAccelerate*in.surfaceFriction*dt,add);vx+=wx*gain;vy+=wy*gain;}
    }
    return {static_cast<float>(vx),static_cast<float>(vy)};
}
void Strafing(){
    auto in=State();in.velocity.z=0;in.buttons=Forward;in.forward=1;Options opt;opt.airStrafe=true;DirectionState direction;
    const auto air=Build(in,opt,direction);Check(air.count==32&&air.analog&&!air.angles&&!air.cameraPreservation,"Ordinary strafe changed view or omitted subticks");SafePlan(air);
    double forward=in.lastImpulses.x,left=in.lastImpulses.y;
    for(std::size_t i=0;i<air.count;++i){forward+=air.steps[i].forwardDelta;left+=air.steps[i].leftDelta;Check(std::abs(forward)<=1.000001&&std::abs(left)<=1.000001,"Analog impulses are not bounded throughout the tick");Check(Near(air.steps[i].when,double(i)/32),"Air strafe lost 32-step timing");}
    Check(Near(forward,air.analog->x)&&Near(left,air.analog->y),"Subtick deltas disagree with final base movement");
    const auto velocity=IntegrateAir(in,air,false);Check(std::hypot(velocity.x,velocity.y)>250,"Air-strafe plan did not gain speed in an independent acceleration model");
    const auto again=Build(in,opt,direction);Check(again.count==air.count&&Near(again.analog->x,air.analog->x)&&Near(again.analog->y,air.analog->y),"Repeated command advanced strafe phase");
    in.buttons=0;const auto coast=Build(in,opt,direction);const auto coastVelocity=IntegrateAir(in,coast,false);
    Check(coast.analog&&std::hypot(coastVelocity.x,coastVelocity.y)>250,"Releasing direction input braked ordinary air strafe");
    in.buttons=Forward|Sprint;const auto shift=Build(in,opt,direction);Check(shift.count==1&&shift.analog->x<0,"Shift-in-air braking");
    in.buttons=Forward;in.quantizeMovement=true;in.maxSubtickWhen=.4f;opt.testStrafer=true;
    const auto quant=Build(in,opt,direction);Check(quant.testStraferHandled&&quant.cameraPreservation&&quant.angles&&quant.angles->y==in.viewYaw&&!quant.analog&&quant.count==16,"Quantized strafe did not anchor its view or take priority over analog strafe");SafePlan(quant);
    float previous=in.maxSubtickWhen;for(std::size_t i=0;i<quant.count;++i){Check(quant.steps[i].when>previous&&quant.steps[i].when<1,"Quantized yaw conflicts with an existing subtick");previous=quant.steps[i].when;}
    const auto qVelocity=IntegrateAir(in,quant,true);Check(std::hypot(qVelocity.x,qVelocity.y)>250,"Quantized strafe plan did not gain speed");
    in.shooting=true;Check(!Build(in,opt,direction).Changed(),"Strafing rewrote a committed shot");
    in.shooting=false;in.quantizeMovement=false;Check(Build(in,opt,direction).count==32,"Unsupported quantization disabled ordinary strafe");
    in.airValid=false;Check(!Build(in,opt,direction).Changed(),"Missing physics produced a strafe guess");
}
void InvalidAndState(){
    DirectionState direction;Check(direction.Update(Forward)==Forward,"Forward preference");
    Check(direction.Update(Forward|Back)==Back,"New opposite button did not win");direction.Reset();Check(!direction.previous&&!direction.pressed,"Session reset retained held direction");
    auto in=State();Options all;all.bhop=all.airStrafe=all.jumpbug=all.fastLadder=all.edgeJump=all.edgeStop=all.edgebug=all.slowWalk=all.testStrafer=true;
    in.buttons=Jump|Forward;in.viewYaw=std::numeric_limits<float>::quiet_NaN();Script unused;Check(!Build(in,all,direction,unused).Changed()&&unused.calls.empty(),"NaN view reached native tracing");
    in=State();in.buttons=Jump;in.interval=0;Check(!Build(in,all,direction).Changed(),"Zero tick interval accepted");
    in=State();in.buttons=Jump;all.airStrafe=all.testStrafer=false;Script malformed{{{std::numeric_limits<float>::quiet_NaN(),{0,0,1},{}}}};const auto safe=Build(in,all,direction,malformed);Check(!safe.jumpbugActive&&!safe.edgebugActive&&!safe.count,"Malformed native trace created landing pulses");
    in.traceValid=false;Script noTrace;const auto basic=Build(in,all,direction,noTrace);Check((basic.clearButtons&Jump)&&noTrace.calls.empty(),"Unsupported hull bindings disabled basic bhop");
}
void OffAxisAirStrafe(){
    for(const float yaw:{0.f,20.f,-20.f,90.f,-90.f,179.f,-179.f})
    for(const std::uint64_t buttons:{Forward,Back,MoveLeft,MoveRight,Forward|MoveLeft,std::uint64_t{0}})
    for(const float acceleration:{1.f,12.f,400.f}){
        auto in=State();in.viewYaw=yaw;in.buttons=buttons;in.airAccelerate=acceleration;
        Options options;options.airStrafe=true;DirectionState direction;
        for(unsigned tick=0;tick<32;++tick){
            const auto before=std::hypot(in.velocity.x,in.velocity.y);
            const auto plan=Build(in,options,direction);SafePlan(plan);
            const auto next=IntegrateAir(in,plan,false);
            Check(plan.analog&&plan.count==32&&std::hypot(next.x,next.y)>before,
                "Off-axis analog strafe braked or stalled across yaw, direction or acceleration changes");
            in.velocity={next.x,next.y,0};in.lastImpulses=*plan.analog;++in.commandNumber;
            in.viewYaw=detail::Yaw(in.viewYaw+.25f);
        }
    }
}
void WeaponSpeedLimits(){
    auto in=State();in.velocity={5,0,0};in.weaponMaxSpeed=100;Options options;options.airStrafe=true;DirectionState direction;
    const auto air=Build(in,options,direction);Check(air.analog&&air.analog->x>0,"Low-speed air strafe failed to accelerate without direction input");
    in.buttons=Sprint;Check(Near(Build(in,options,direction).analog->x,0),"Shift braking synthesized motion below the source stop threshold");
    in.buttons=0;in.onGround=true;options.airStrafe=false;options.edgeStop=true;Script cliff{{Clear()}};
    const auto ground=Build(in,options,direction,cliff);Check(ground.analog&&Near(ground.analog->x,-.05),"Ground brake used the movement-service limit instead of weapon speed");
    in.weaponMaxSpeed=std::numeric_limits<float>::quiet_NaN();Script absent{{Clear()}};
    Check(Near(Build(in,options,direction,absent).analog->x,-.02),"Missing weapon data disabled validated movement-speed fallback");
    in.weaponMaxSpeed=std::numeric_limits<float>::infinity();Script invalid{{Clear()}};
    Check(Near(Build(in,options,direction,invalid).analog->x,-.02),"Invalid weapon speed contaminated braking");
    options.edgeStop=false;options.slowWalk=true;in.forward=1;in.weaponMaxSpeed=100;
    const auto slow=Build(in,options,direction);Check(slow.analog&&Near(slow.analog->x,.33),"Weapon speed changed slowwalk's input percentage");
    options.slowWalk=false;options.airStrafe=true;in.onGround=false;in.velocity={250,0,0};in.buttons=Forward;
    const auto first=Build(in,options,direction);in.weaponMaxSpeed=1000;const auto second=Build(in,options,direction);
    Check(first.count==second.count&&Near(first.analog->x,second.analog->x)&&Near(first.analog->y,second.analog->y),"Weapon speed replaced the effective air-acceleration limit");
}
void AutomaticQuantizedStrafe(){
    Options options;options.airStrafe=true;DirectionState direction;
    auto in=State();in.quantizeMovement=true;in.velocity.z=0;in.buttons=0;
    const auto first=Build(in,options,direction);
    Check(first.testStraferHandled&&first.cameraPreservation&&first.count==16&&first.analog&&
        first.analog->x==1&&first.analog->y==0&&(first.setButtons&Forward)&&first.angles&&first.angles->y==in.viewYaw,
        "Ordinary air strafe did not automatically select quantized input without WASD");
    SafePlan(first);
    const auto repeated=Build(in,options,direction);
    for(std::size_t i=0;i<first.count;++i)Check(Near(first.steps[i].yawDelta,repeated.steps[i].yawDelta),
        "Replayed quantized air strafe advanced its phase");
    for(unsigned tick=0;tick<64;++tick){
        in.buttons=tick%3==0?Forward:0;in.forward=in.buttons?1.f:0.f;
        const auto plan=Build(in,options,direction);const auto next=IntegrateAir(in,plan,true);
        Check(std::hypot(next.x,next.y)>std::hypot(in.velocity.x,in.velocity.y),
            "Quantized air strafe stalled or lost speed between direction presses");
        in.velocity={next.x,next.y,0};++in.commandNumber;
    }
    Check(std::hypot(in.velocity.x,in.velocity.y)>400,"Quantized strafe did not gain sustained speed");
    for(const auto side:{MoveLeft,MoveRight}){
        auto turn=State();turn.quantizeMovement=true;turn.buttons=side;turn.left=side==MoveLeft?1.f:-1.f;
        direction.Reset();const auto plan=Build(turn,options,direction),replay=Build(turn,options,direction);
        const auto velocity=IntegrateAir(turn,plan,true);
        Check(plan.analog&&plan.analog->x==0&&plan.analog->y==turn.left&&
            velocity.y*turn.left>0&&std::hypot(velocity.x,velocity.y)>250,
            "Quantized directional steering lost speed or turned against its held key");
        Check(Near(plan.steps[0].yawDelta,replay.steps[0].yawDelta),"Directional replay changed strafe timing");
    }
    in=State();in.quantizeMovement=true;in.velocity={0,0,0};direction.Reset();
    const auto launch=Build(in,options,direction);Check(launch.analog&&launch.analog->x==1,
        "Air strafe could not launch from zero speed without WASD");
    in.velocity={250,0,0};in.buttons=Sprint;
    Check(!Build(in,options,direction).testStraferHandled,"Quantized strafe overrode explicit Shift braking");
    in.buttons=0;in.shooting=true;Check(!Build(in,options,direction).Changed(),"Automatic quantized strafe rewrote a shot");
}
}
int main(){try{BhopAndLanding();ManualJumpOwnership();LandingExploitPriority();CombinedJumpActivation();HeldJumpCycles();GroundEdges();SlowwalkAndLadder();Strafing();InvalidAndState();OffAxisAirStrafe();WeaponSpeedLimits();AutomaticQuantizedStrafe();std::cout<<"PASS: nine movement policies, held-jump recovery, automatic quantized strafe and sustained speed gain, directional steering, ordered subticks, replay, bounds, and shot coexistence\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
