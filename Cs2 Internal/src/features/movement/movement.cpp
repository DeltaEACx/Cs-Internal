#include "core/user_defaults.hpp"
#include "features/movement/movement.h"
#include "features/movement/context.h"
#include "imgui/imgui.h"
#include "ui/gui_capture.h"
#include <mutex>
#include <atomic>
namespace movement {
namespace {
namespace policy=movement_policy;
using Address=std::uintptr_t;
struct Settings {bool enabled{true};policy::Options options;std::array<int,9>keys{};};
Settings settings=user_defaults::Movement(Settings{});
std::mutex settingsMutex,stateMutex;
std::atomic<bool> menuOpen{true};
std::atomic<const char*> status{"Movement ready."};
std::atomic<const char*> pipelineStatus{"The movement input hook has not initialized."};
std::atomic<const char*> gameplayStatus{"Waiting for a gameplay command."};
policy::DirectionState direction;
bool spaceJumpArmed{};
Address lastList{},lastPawn{};std::uint32_t lastHandle{0xffffffffu};int lastSequence{};
int captureKey=-1;std::array<bool,256>previousKeys{};bool finishPending{};
struct Committed {
    bool valid{},camera{},grounded{},shotPreparation{};Address command{},base{};int sequence{};std::uint32_t handle{};
    combat_bindings::BaseCommand message{};combat_bindings::ButtonState buttons{};
    combat_bindings::ButtonPayload pb{};combat_bindings::AnglePayload angles{};
    std::array<Address,256> stepPointers{};std::array<combat_bindings::SubtickPayload,256> steps{};
    policy::Options options{};float visiblePitch{},visibleYaw{};
    std::array<Address,64> ownedSteps{};std::size_t ownedCount{};
} committed;
struct Observation {
    bool valid{},grounded{},heldJump{},freshJump{},serverBhop{},hull{};
    bool airPhysics{},quantized{},shooting{};
    int command{},nativeSteps{};float verticalSpeed{};std::size_t plannedSteps{};
} observation;
const char *WaitingStatus(const policy::Input &state,const policy::Options &options) {
    if(!policy::detail::Valid(state))return "Current movement inputs are invalid; command preserved.";
    if(state.moveType==9){
        if(!options.fastLadder)return "Enable fast ladder to assist ladder movement.";
        if(state.shooting||(state.buttons&policy::Use))return "Fast ladder paused for this shot or interaction.";
        return "Fast ladder needs forward or sideways input.";
    }
    if(state.moveType!=2)return "Movement assists wait for normal walking or ladder movement.";
    if(!state.onGround&&(options.airStrafe||options.testStrafer)){
        if(state.shooting)return "Air strafe paused for this shot or interaction.";
        if(!policy::detail::AirValid(state))return "Air strafe needs valid movement physics and previous impulses.";
        if(options.testStrafer&&!options.airStrafe&&!state.quantizeMovement)return "Quantized strafe requires sv_quantize_movement_input.";
    }
    const bool jumpingOnly=!options.airStrafe&&!options.testStrafer&&!options.slowWalk&&!options.fastLadder&&
        !options.edgeJump&&!options.edgeStop&&(options.bhop||options.jumpbug||options.edgebug);
    if(!jumpingOnly){
        if(state.onGround&&(options.bhop||options.jumpbug)&&!(state.buttons&policy::Jump))
            return "Hold jump for bunnyhop or jumpbug. Air strafe starts when airborne.";
        if(state.onGround&&(options.airStrafe||options.testStrafer))return "Air strafe waits for an airborne player.";
        if(!policy::detail::HullValid(state)&&(options.edgeJump||options.edgeStop||options.jumpbug||options.edgebug))
            return "Landing and edge assists need a valid player hull and trace.";
        return "Movement ready; waiting for an eligible movement state.";
    }
    if(state.jumpPressed)return "Manual jump takeoff preserved; landing assists wait for descent.";
    if(!state.onGround&&state.velocity.z>0)return "Jump rising; landing assists resume on descent.";
    if(!(state.buttons&policy::Jump)&&(options.bhop||options.jumpbug))
        return "Hold jump for bunnyhop and jumpbug. Edgebug waits for a landing.";
    if(state.autoBunnyhop&&options.bhop)return "Server autobhop has priority; landing assists are ready.";
    if(!policy::detail::HullValid(state)&&(options.jumpbug||options.edgebug))
        return "Landing hull is unavailable; manual jumping is preserved.";
    return "Jumping assists ready; waiting for a landing surface.";
}
bool SameOptions(const policy::Options &a,const policy::Options &b){
    return a.bhop==b.bhop&&a.airStrafe==b.airStrafe&&a.fullyDirectional==b.fullyDirectional&&a.jumpbug==b.jumpbug&&
        a.fastLadder==b.fastLadder&&a.edgeJump==b.edgeJump&&a.edgeStop==b.edgeStop&&a.edgebug==b.edgebug&&a.slowWalk==b.slowWalk&&
        a.testStrafer==b.testStrafer&&a.slowwalkPercent==b.slowwalkPercent&&a.edgebugMode==b.edgebugMode&&
        a.edgebugPasses==b.edgebugPasses&&a.edgebugJumpSteps==b.edgebugJumpSteps;
}
bool Capture(const native::Context &context,const policy::Options &options,bool camera,Committed &out){
    using namespace combat_bindings;out={};out.command=context.command;out.base=context.base;out.sequence=context.state.commandNumber;out.handle=context.local.handle;
    out.camera=camera;out.grounded=context.state.onGround;out.options=options;out.visiblePitch=context.state.viewPitch;out.visibleYaw=context.state.viewYaw;
    if(!memory::copy(&out.message,context.base,sizeof(out.message))||out.message.subticks.size<0||out.message.subticks.size>256||
        !memory::copy(&out.buttons,context.command+kCommandButtons,sizeof(out.buttons)))return false;
    if(out.message.buttons&&!memory::copy(&out.pb,out.message.buttons+kMessagePayload,sizeof(out.pb)))return false;
    if(out.message.angles&&!memory::copy(&out.angles,out.message.angles+kMessagePayload,sizeof(out.angles)))return false;
    const auto &field=out.message.subticks;
    if(field.size&&(!field.entries||!memory::copy(out.stepPointers.data(),field.entries+8,field.size*sizeof(Address))))return false;
    for(int index=0;index<field.size;++index)
        if(!out.stepPointers[index]||!memory::copy(&out.steps[index],out.stepPointers[index]+kMessagePayload,sizeof(out.steps[index])))return false;
    out.valid=true;return true;
}
bool Intact(const native::Context &context,const policy::Options &options){
    if(!committed.valid||committed.command!=context.command||committed.base!=context.base||committed.sequence!=context.state.commandNumber||
        committed.handle!=context.local.handle||committed.grounded!=context.state.onGround||!SameOptions(committed.options,options)||committed.visiblePitch!=context.state.viewPitch||committed.visibleYaw!=context.state.viewYaw)return false;
    Committed current;if(!Capture(context,options,committed.camera,current))return false;
    return !std::memcmp(&current.message,&committed.message,sizeof(current.message))&&!std::memcmp(&current.buttons,&committed.buttons,sizeof(current.buttons))&&
        !std::memcmp(&current.pb,&committed.pb,sizeof(current.pb))&&!std::memcmp(&current.angles,&committed.angles,sizeof(current.angles))&&
        current.stepPointers==committed.stepPointers&&!std::memcmp(current.steps.data(),committed.steps.data(),current.message.subticks.size*sizeof(current.steps[0]));
}
bool Reusable(const native::Context &context){
    if(!committed.valid||committed.command!=context.command||committed.sequence!=context.state.commandNumber||
        committed.handle!=context.local.handle||!committed.ownedCount)return false;
    const auto &field=context.baseSnapshot.subticks;if(field.size<static_cast<int>(committed.ownedCount)||field.size>256)return false;
    std::array<Address,256> active{};
    if(!memory::copy(active.data(),field.entries+8,field.size*sizeof(Address)))return false;
    for(std::size_t index=0;index<committed.ownedCount;++index){
        if(std::find(active.begin(),active.begin()+field.size,committed.ownedSteps[index])==active.begin()+field.size)return false;
        const auto old=std::find(committed.stepPointers.begin(),committed.stepPointers.begin()+committed.message.subticks.size,committed.ownedSteps[index]);
        combat_bindings::SubtickPayload payload{};
        if(old==committed.stepPointers.begin()+committed.message.subticks.size||
            !memory::copy(&payload,committed.ownedSteps[index]+combat_bindings::kMessagePayload,sizeof(payload))||
            std::memcmp(&payload,&committed.steps[old-committed.stepPointers.begin()],sizeof(payload)))return false;
    }
    return true;
}
policy::Input PlanningInput(const native::Context &context,bool continuingSpace=false) {
    auto state=context.state;
    const bool reusable=Reusable(context);
    if(!reusable&&!continuingSpace)return state;
    // A native overwrite may retain our exact generated landing messages.
    // They are not fresh user presses: retain raw/PB edges and only unowned
    // native jump events when deciding whether bunnyhop can own this window.
    // Restoring held Jump after our raw release sets the native changed bit
    // again. A confirmed continuous Space hold is not another hardware press;
    // wheel input and unowned native press events retain manual priority.
    state.jumpPressed=continuingSpace?context.jumpScrolled:context.jumpPressedFromButtons;
    state.buttons&=~policy::Jump;
    if(context.jumpHeldFromButtons)state.buttons|=policy::Jump;
    // Generated yaw steps are replaced on rebuild; only native angle events
    // constrain the start of the next quantized window.
    state.maxSubtickWhen=0;
    const auto &field=context.baseSnapshot.subticks;
    for(int index=0;index<field.size;++index){
        const auto raw=memory::safe_read<Address>(field.entries+8+index*sizeof(Address)).value_or(0);
        if(reusable&&std::find(committed.ownedSteps.begin(),committed.ownedSteps.begin()+committed.ownedCount,raw)!=
           committed.ownedSteps.begin()+committed.ownedCount)continue;
        const auto step=raw?memory::safe_read<combat_bindings::SubtickPayload>(raw+combat_bindings::kMessagePayload):std::nullopt;
        if(!step)return context.state; // Preserve uncertain native input.
        if(step->bits&0x60)state.maxSubtickWhen=std::max(state.maxSubtickWhen,step->when);
        if((step->bits&3)==3&&step->pressed&&(step->button&policy::Jump)){
            // Native input can reassert Jump after our release on every
            // command. A confirmed continuous Space hold is still held input;
            // wheel input and a new unconfirmed hold keep manual priority.
            if(!continuingSpace||context.jumpScrolled)state.jumpPressed=true;
            state.buttons|=policy::Jump;
        }
    }
    return state;
}
Settings Get(){std::lock_guard lock(settingsMutex);return settings;}
bool Any(const policy::Options &value){
    return value.bhop||value.airStrafe||value.testStrafer||value.slowWalk||value.fastLadder||
        value.edgeJump||value.edgeStop||value.jumpbug||value.edgebug;
}
void Clear(){direction.Reset();spaceJumpArmed=false;committed={};finishPending=false;lastList=lastPawn=0;lastHandle=0xffffffffu;lastSequence=0;}
bool Focused(){DWORD process{};const auto window=GetForegroundWindow();return window&&GetWindowThreadProcessId(window,&process)&&process==GetCurrentProcessId();}
bool Held(int key){return key==0||(key>0&&key<256&&(GetAsyncKeyState(key)&0x8000));}
policy::Options Active(const Settings &config) {
    if(!config.enabled)return {};
    auto value=config.options;
    bool *toggles[]{&value.bhop,&value.airStrafe,&value.testStrafer,&value.slowWalk,&value.fastLadder,&value.edgeJump,&value.edgeStop,&value.jumpbug,&value.edgebug};
    for(std::size_t index=0;index<config.keys.size();++index)*toggles[index]&=Held(config.keys[index]);
    return value;
}
const char *InactiveStatus(const Settings &config){
    return !config.enabled?"Enable movement assists.":Any(config.options)?
        "Hold the activation key for an enabled movement assist.":"Enable a movement assist.";
}
std::optional<std::uint64_t> PreparationActions(const native::Context &context){
    using namespace combat_bindings;
    constexpr std::uint64_t actions=kAttack|policy::Use|(1ull<<11);
    auto held=(context.buttons.value|context.buttons.scroll)&actions;
    if(context.baseSnapshot.buttons){
        const auto payload=memory::safe_read<ButtonPayload>(context.baseSnapshot.buttons+kMessagePayload);
        if(!payload)return {};
        held|=((payload->bits&1?payload->value:0)|(payload->bits&4?payload->scroll:0))&actions;
    }
    const auto &steps=context.baseSnapshot.subticks;
    for(int index=0;index<steps.size;++index){
        const auto raw=memory::safe_read<Address>(steps.entries+8+index*sizeof(Address)).value_or(0);
        const auto step=raw?memory::safe_read<SubtickPayload>(raw+kMessagePayload):std::nullopt;
        if(!step)return {};
        if((step->bits&3)==3&&step->pressed)held|=step->button&actions;
    }
    return held;
}
std::string KeyName(int key){
    if(!key)return "Always";if(key==VK_LBUTTON)return "Mouse1";if(key==VK_RBUTTON)return "Mouse2";
    if(key==VK_XBUTTON1)return "Mouse4";if(key==VK_XBUTTON2)return "Mouse5";
    char name[64]{};const auto scan=MapVirtualKeyA(key,MAPVK_VK_TO_VSC_EX);
    const auto parameter=(scan&0xff)<<16|((scan&0xff00)?1<<24:0);
    return GetKeyNameTextA(parameter,name,64)?name:std::to_string(key);
}
void Toggle(const char *label,bool &value,Settings &config,int index,const char *help=nullptr) {
    ImGui::PushID(index);ImGui::Checkbox(label,&value);
    if(gui_capture::active){
        captureKey=-1;
        const auto binding=std::string(label)+" key";
        gui_capture::active->Keybind(binding.c_str(),&config.keys[index]);
        ImGui::PopID();
        return;
    }
    if(help&&ImGui::IsItemHovered())ImGui::SetTooltip("%s",help);
    ImGui::SameLine();
    const auto text=captureKey==index?std::string("Press key..."):KeyName(config.keys[index]);
    if(ImGui::Button(text.c_str(),{115,0})){
        captureKey=index;for(int key=0;key<256;++key)previousKeys[key]=(GetAsyncKeyState(key)&0x8000)!=0;
    }
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Set a hold key. Escape clears the bind to Always.");
    ImGui::PopID();
}
}
void Reset(){std::lock_guard lock(stateMutex);Clear();native::Reset();
    status=menuOpen.load(std::memory_order_acquire)?"Paused while the menu is open.":"Waiting for a gameplay command.";}
void SetInputState(bool open){
    const auto before=menuOpen.exchange(open,std::memory_order_acq_rel);
    if(before!=open){Reset();captureKey=-1;if(open)status="Paused while the menu is open.";}
}
void SetPipelineStatus(const char *failure){pipelineStatus.store(failure,std::memory_order_release);}
std::optional<CameraStamp> PrepareShot(Address input,bool allowHeldPrimary,bool shotCommitted){
    std::lock_guard lock(stateMutex);
    if(shotCommitted)return {};
    finishPending=false;
    if(menuOpen.load(std::memory_order_acquire)||!Focused())return {};
    const auto context=native::Read(input,false);
    const auto report=[&](const char *message){status=message;gameplayStatus=message;};
    if(!context.valid){report(context.failure);return {};}
    const auto &state=context.state;
    if(!state.onGround||state.moveType!=2){report("Auto stop waits for a grounded player.");return {};}
    const bool heldSpace=spaceJumpArmed&&(GetAsyncKeyState(VK_SPACE)&0x8000)!=0;
    const auto actions=PreparationActions(context);
    const bool primaryOnly=actions&&(*actions&combat_bindings::kAttack)&&!(*actions&(policy::Use|(1ull<<11)));
    if(!actions||(*actions&(policy::Use|(1ull<<11)))||state.jumpPressed||(state.buttons&policy::Jump)||heldSpace||
       (state.shooting&&!(allowHeldPrimary&&primaryOnly))){
        report("Auto stop preserves manual jumping, shots and interactions.");return {};
    }
    const auto analog=policy::detail::CounterSteer(state);
    if(!analog||!policy::detail::Finite(state.lastImpulses)||std::abs(state.lastImpulses.x)>1.001f||std::abs(state.lastImpulses.y)>1.001f){
        report("Auto stop needs valid ground physics and previous movement impulses.");return {};
    }
    if(policy::detail::Speed(state.velocity)<=1){report("Player movement is already stopped; waiting for weapon accuracy.");return {};}
    const auto options=Active(Get());
    if(Intact(context,options)&&committed.shotPreparation){finishPending=true;
        report("Auto stop applied; waiting for weapon accuracy.");return CameraStamp{context.command,state.commandNumber,context.local.handle};}
    combat_command::Movement edit;edit.analog=std::array{analog->x,analog->y};edit.replaceAnalog=true;
    // Countersteer is calculated in the visible view's basis. Native command
    // replays may retain a prior anti-aim base angle; anchor this edit before
    // anti-aim rotates the committed movement into its outgoing angle again.
    edit.angles=std::array{state.viewPitch,state.viewYaw,0.f};
    policy::Plan directions;policy::detail::Directions(directions,*analog);
    edit.clearButtons=directions.clearButtons;edit.setButtons=directions.setButtons;
    edit.steps[0].bits=0x1c;edit.steps[0].when=0;
    edit.steps[0].forward=analog->x-state.lastImpulses.x;edit.steps[0].left=analog->y-state.lastImpulses.y;edit.count=1;
    if(Reusable(context)){edit.replaceSteps=committed.ownedSteps;edit.replaceCount=committed.ownedCount;}
    std::array<Address,64> owned{};
    if(!native::Fresh(context)||!combat_command::EditMovement(context.command,context.base,state.commandNumber,
            edit,{combat_access::CommandIO(),native::AcquireSubtick,&owned})){
        report("Auto stop input changed or its command could not be committed.");return {};
    }
    Capture(context,options,true,committed);committed.shotPreparation=true;committed.ownedSteps=owned;committed.ownedCount=1;finishPending=committed.valid;
    observation={true,true,false,false,state.autoBunnyhop,policy::detail::HullValid(state),policy::detail::AirValid(state),
        state.quantizeMovement,state.shooting,state.commandNumber,context.baseSnapshot.subticks.size,state.velocity.z,1};
    report("Auto stop applied; waiting for weapon accuracy.");return CameraStamp{context.command,state.commandNumber,context.local.handle};
}
std::optional<CameraStamp> Process(Address input,bool shotRequested) {
    std::lock_guard lock(stateMutex);finishPending=false;
    const auto config=Get();
    if(menuOpen.load(std::memory_order_acquire)||!Focused()){
        Clear();status="Paused while the menu is open or the game is unfocused.";return {};
    }
    struct ResultStamp {~ResultStamp(){gameplayStatus.store(status.load(std::memory_order_acquire),std::memory_order_release);}} resultStamp;
    const auto options=Active(config);
    if(!config.enabled){direction.Reset();spaceJumpArmed=false;}
    if(!Any(options)&&!(committed.valid&&committed.shotPreparation)){Clear();
        status=InactiveStatus(config);return {};}
    const bool spaceDown=(GetAsyncKeyState(VK_SPACE)&0x8000)!=0;
    const auto context=native::Read(input,shotRequested);
    if(!context.valid){
        // A brief command/weapon-selection gap must not forget a confirmed
        // hold on the same alive player. Never carry it across death/session
        // changes, focus loss, menu transitions or physical key release.
        const bool keepHold=spaceJumpArmed&&spaceDown&&context.local.state==game_access::PlayerState::Alive&&
            lastList==context.local.list&&lastPawn==context.local.pawn&&lastHandle==context.local.handle;
        if(keepHold){direction.Reset();committed={};finishPending=false;}else Clear();
        status=context.failure;return {};
    }
    if(lastList!=context.local.list||lastPawn!=context.local.pawn||lastHandle!=context.local.handle||
       context.state.commandNumber<lastSequence||context.state.commandNumber>lastSequence+512){direction.Reset();spaceJumpArmed=false;}
    lastList=context.local.list;lastPawn=context.local.pawn;lastHandle=context.local.handle;lastSequence=context.state.commandNumber;
    const bool continuingSpace=spaceDown&&spaceJumpArmed;
    // Arm only after native Jump confirms this Space hold is a jump binding.
    // Our own descending release can survive a native command rebuild; it
    // must not erase the user's still-held key. Physical release is immediate.
    if(!spaceDown)spaceJumpArmed=false;
    else if(context.jumpHeldFromButtons)spaceJumpArmed=true;
    if(Intact(context,options)){finishPending=true;status=committed.shotPreparation?
        "Auto stop applied; waiting for weapon accuracy.":"Movement already applied to this native command.";
        return committed.camera?std::optional{CameraStamp{context.command,context.state.commandNumber,context.local.handle}}:std::nullopt;}
    if(!Any(options)){Clear();status=InactiveStatus(config);return {};}
    auto next=direction;auto planning=PlanningInput(context,continuingSpace);
    if(spaceJumpArmed)planning.buttons|=policy::Jump;
    observation={true,planning.onGround,(planning.buttons&policy::Jump)!=0,planning.jumpPressed,planning.autoBunnyhop,
        policy::detail::HullValid(planning),policy::detail::AirValid(planning),planning.quantizeMovement,planning.shooting,
        planning.commandNumber,context.baseSnapshot.subticks.size,planning.velocity.z,0};
    auto plan=policy::Build(planning,options,next,[&](auto start,auto end,auto mins,auto maxs){return native::TraceHull(context,start,end,mins,maxs);});
    observation.plannedSteps=plan.count;
    if(!plan.Changed()){direction=next;status=WaitingStatus(planning,options);return {};}
    combat_command::Movement edit;
    if(plan.analog)edit.analog=std::array{plan.analog->x,plan.analog->y};
    if(plan.angles)edit.angles=std::array{plan.angles->x,plan.angles->y,plan.angles->z};
    // Analog strafe deltas are calculated in the visible view's basis. A
    // rebuilt command may retain anti-aim's old base yaw; anchor this basis
    // before the outgoing orientation pass rotates the movement again.
    const bool visibleBasis=plan.analog&&!plan.angles&&!planning.onGround&&options.airStrafe;
    if(visibleBasis)edit.angles=std::array{planning.viewPitch,planning.viewYaw,0.f};
    const bool camera=plan.cameraPreservation||visibleBasis;
    edit.clearButtons=plan.clearButtons;edit.setButtons=plan.setButtons;
    if(Reusable(context)){edit.replaceSteps=committed.ownedSteps;edit.replaceCount=committed.ownedCount;}
    bool analogWindow=false;
    for(std::size_t index=0;index<plan.count;++index){
        const auto &step=plan.steps[index];auto &out=edit.steps[index];out.bits=4;
        const bool analog=(step.forwardDelta!=0||step.leftDelta!=0)||
            (plan.analog&&step.button==0&&step.yawDelta==0&&step.pitchDelta==0);
        if(step.button)out.bits|=3;if(analog)out.bits|=0x18;
        if(step.pitchDelta!=0)out.bits|=0x20;if(step.yawDelta!=0)out.bits|=0x40;
        analogWindow|=analog;
        out.button=step.button;out.pressed=step.pressed;out.when=step.when;out.forward=step.forwardDelta;
        out.left=step.leftDelta;out.pitch=step.pitchDelta;out.yaw=step.yawDelta;
    }
    edit.count=plan.count;edit.replaceAnalog=plan.analog.has_value();
    // Source desubtick clears preceding impulses; retain manual events instead.
    // Its AG2 bridge expresses a changed base movement relative to services
    // when the assist did not already provide its own analog window.
    if(edit.analog&&!analogWindow){
        if(!std::isfinite(context.state.lastImpulses.x)||!std::isfinite(context.state.lastImpulses.y)||
           std::abs(context.state.lastImpulses.x)>1.001f||std::abs(context.state.lastImpulses.y)>1.001f){
            status="Previous movement impulses are unavailable.";return {};
        }
        if(edit.count==edit.steps.size()){status="The movement subtick window is full.";return {};}
        auto &step=edit.steps[edit.count++];step.bits=0x1c;step.when=0;
        step.forward=(*edit.analog)[0]-context.state.lastImpulses.x;step.left=(*edit.analog)[1]-context.state.lastImpulses.y;
    }
    // Spectral clears base analog before input.apply, which can add a fallback
    // AG2 impulse afterwards. Only the feature-generated window owns the base.
    edit.zeroBaseAnalog=edit.analog.has_value()&&plan.count>0&&!plan.testStraferHandled;
    observation.plannedSteps=edit.count;
    std::array<Address,64> owned{};
    if(!native::Fresh(context)||!combat_command::EditMovement(context.command,context.base,context.state.commandNumber,
            edit,{combat_access::CommandIO(),native::AcquireSubtick,&owned})){
        status="Movement input changed or its native command could not be committed.";return {};
    }
    direction=next;Capture(context,options,camera,committed);committed.ownedSteps=owned;committed.ownedCount=edit.count;finishPending=committed.valid;
    status=plan.edgebugActive?"Edgebug subtick window applied.":plan.jumpbugActive?"Jumpbug subtick window applied.":
        plan.testStraferHandled?"Quantized strafe subticks applied.":plan.cameraPreservation?"Fast ladder applied; visible camera preserved.":"Movement command applied.";
    return camera?std::optional{CameraStamp{context.command,context.state.commandNumber,context.local.handle}}:std::nullopt;
}
void FinishCommand(Address input){
    std::lock_guard lock(stateMutex);if(!committed.valid||!finishPending)return;finishPending=false;
    const auto context=native::Read(input,false);
    if(!context.valid||context.command!=committed.command||context.base!=committed.base||
        context.state.commandNumber!=committed.sequence||context.local.handle!=committed.handle)return;
    const auto owned=committed.ownedSteps;const auto count=committed.ownedCount;const auto options=committed.options;const auto camera=committed.camera;
    const auto preparation=committed.shotPreparation;
    Committed refreshed;
    if(!Capture(context,options,camera,refreshed)){committed.valid=false;return;}
    // Capture already validates and copies the complete native table. Check
    // ownership locally rather than rereading every pointer for each step.
    const auto begin=refreshed.stepPointers.begin(),end=begin+refreshed.message.subticks.size;
    for(std::size_t index=0;index<count;++index)
        if(std::find(begin,end,owned[index])==end){committed.valid=false;return;}
    refreshed.shotPreparation=preparation;refreshed.ownedSteps=owned;refreshed.ownedCount=count;
    committed=std::move(refreshed);
}
void DrawMenu() {
    auto value=Get();ImGui::Checkbox("Enable movement assists",&value.enabled);
    if(ImGui::CollapsingHeader("Jumping",ImGuiTreeNodeFlags_DefaultOpen)){
        Toggle("Bunnyhop",value.options.bhop,value,0,"Hold jump. Server autobunnyhop takes priority.");
        Toggle("Jumpbug",value.options.jumpbug,value,7);
        Toggle("Edgebug",value.options.edgebug,value,8);
        if(value.options.edgebug){
            constexpr const char* modes[]{"Any landing","Edge geometry","No jump held","Moving","Fast descent"};
            ImGui::Combo("Edgebug mode",&value.options.edgebugMode,modes,5);
            ImGui::SliderInt("Duck taps",&value.options.edgebugPasses,1,5);
            ImGui::Checkbox("Jump steps",&value.options.edgebugJumpSteps);
        }
    }
    if(ImGui::CollapsingHeader("Air",ImGuiTreeNodeFlags_DefaultOpen)){
        Toggle("Air strafe",value.options.airStrafe,value,1);
        ImGui::Checkbox("Directional steering",&value.options.fullyDirectional);
        Toggle("Quantized strafe",value.options.testStrafer,value,2,"Requires sv_quantize_movement_input.");
    }
    if(ImGui::CollapsingHeader("Ground & ladders",ImGuiTreeNodeFlags_DefaultOpen)){
        Toggle("Slow walk",value.options.slowWalk,value,3);
        ImGui::SliderFloat("Slow walk speed",&value.options.slowwalkPercent,1,100,"%.0f%%");
        Toggle("Edge jump",value.options.edgeJump,value,5,"Takes priority over edge stop.");
        Toggle("Edge stop",value.options.edgeStop,value,6);
        Toggle("Fast ladder",value.options.fastLadder,value,4,"Preserves your camera. Shooting takes priority.");
    }
    if(captureKey>=0&&!gui_capture::active){
        ImGui::TextDisabled("Press a key. Esc to clear.");
        for(int key=1;key<256;++key){const bool down=(GetAsyncKeyState(key)&0x8000)!=0;
            if(down&&!previousKeys[key]&&key!=VK_INSERT&&key!=VK_END){value.keys[captureKey]=key==VK_ESCAPE?0:key;captureKey=-1;break;}
            previousKeys[key]=down;
        }
    }
    value.options.slowwalkPercent=std::clamp(value.options.slowwalkPercent,1.f,100.f);
    value.options.edgebugMode=std::clamp(value.options.edgebugMode,0,4);value.options.edgebugPasses=std::clamp(value.options.edgebugPasses,1,5);
    {std::lock_guard lock(settingsMutex);settings=value;}
}
}
