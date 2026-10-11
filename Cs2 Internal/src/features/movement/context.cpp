#include "features/movement/context.h"
#include "game/sdk/SkinBindings.h"
#include "game/sdk/MovementBindings.h"
#include <limits>
namespace movement::native {
namespace {
using Address=combat_access::Address;
using Vec3=combat_access::Vec3;
using combat_access::Field;
struct Bindings {Address client{},ray{},filter{},managerSlot{},allocate{},append{};} api;
struct Physics {
    Address list{},pawn{};std::uint32_t handle{0xffffffffu};ULONGLONG refreshed{};bool valid{};
    float gravity{},normal{},maxSpeed{},airAccelerate{},airWish{},staminaRecovery{},friction{},stopSpeed{},accelerate{};
    std::optional<bool> autobhop,quantize;
} physics;
bool Bind() {
    if(api.client&&api.allocate&&api.append)return true;
    if(!game::module_matches("client.dll",skin_bindings::kClientSha256))return false;
    api.client=reinterpret_cast<Address>(GetModuleHandleW(L"client.dll"));
    const auto checked=[&](const skin_bindings::Function& binding){
        std::array<std::uint8_t,24> entry{};
        return memory::copy(entry.data(),api.client+binding.rva,entry.size())&&entry==binding.entry?api.client+binding.rva:Address{};
    };
    api.ray=checked(movement_bindings::hull);api.filter=checked(movement_bindings::filter);
    api.managerSlot=api.client+movement_bindings::traceManager;
    api.allocate=checked(movement_bindings::allocate);api.append=checked(movement_bindings::append);
    // Ordinary strafe/slowwalk and grounded jump recovery do not need a hull.
    return api.client&&api.allocate&&api.append;
}
float Value(Address object,const char *owner,const char *field) {return Field<float>(object,owner,field).value_or(NAN);}
float Cvar(const char *name) {return combat_access::Convar(name,NAN);}
movement_policy::Vec3 Point(Vec3 v){return {v.x,v.y,v.z};}
bool VectorValid(Vec3 v){return combat_access::Finite(v)&&std::abs(v.x)<1e7f&&std::abs(v.y)<1e7f&&std::abs(v.z)<1e7f;}
}
Context Read(Address input,bool shotRequested) {
    using namespace combat_bindings;
    Context result;result.input=input;
    const auto fail=[&](const char *reason){result.failure=reason;return result;};
    if(!Bind())return fail("Movement native bindings are unavailable for this client.");
    result.local=game_access::ReadLocalPlayer(api.client);
    if(result.local.state!=game_access::PlayerState::Alive)return fail("Waiting for an alive local player.");
    const auto identity=combat_access::ReadInputCommand(input);
    if(!identity||identity->localHandle!=result.local.handle)return fail("The current movement command is unavailable.");
    if(identity->weaponSelectionPending)return fail("Paused for weapon selection.");
    result.command=identity->command;
    const auto proto=memory::safe_read<CommandProto>(result.command+kCommandProto);
    if(!proto||!(proto->bits&1)||!proto->base)return fail("The movement command has no base message.");
    result.base=proto->base+kMessagePayload;
    const auto base=memory::safe_read<BaseCommand>(result.base);
    const auto buttons=memory::safe_read<ButtonState>(result.command+kCommandButtons);
    if(!base||!buttons)return fail("The movement command could not be read.");
    result.baseSnapshot=*base;result.buttons=*buttons;
    auto &state=result.state;state.commandNumber=identity->sequence;state.buttons=buttons->value;
    constexpr auto jump=movement_policy::Jump;
    state.jumpPressed=((buttons->value&buttons->changed&jump)!=0)||(buttons->scroll&jump)!=0;
    result.jumpScrolled=(buttons->scroll&jump)!=0;
    state.buttons|=buttons->scroll&jump;
    // Spectral reads prediction.pre().flags; this hook reads post-original
    // state. Preserve a new takeoff/scroll edge instead of treating it as a
    // held airborne jump. A fresh raw release wins over a stale PB held value.
    const bool releasedJump=(buttons->changed&jump)&&!(buttons->value&jump);
    Vec3 view{};
    if(!combat_access::ReadViewAngles(input,view))return fail("The visible movement view is unavailable.");
    state.viewPitch=view.x;state.viewYaw=view.y;
    state.forward=base->bits&0x40?base->forward:0;state.left=base->bits&0x80?base->left:0;
    const auto flags=Field<std::uint32_t>(result.local.pawn,"C_BaseEntity","m_fFlags");
    const auto type=Field<std::uint8_t>(result.local.pawn,"C_BaseEntity","m_nActualMoveType");
    if(!flags||!type)return fail("Player movement flags are unavailable.");
    if((*flags&(1u<<6))||*type==7||*type==8)return fail("Paused while frozen, observing or in noclip.");
    state.onGround=(*flags&1)!=0;state.moveType=*type;
    const auto velocity=Field<Vec3>(result.local.pawn,"C_BaseEntity","m_vecVelocity");
    if(!velocity||!VectorValid(*velocity))return fail("Current movement velocity is unavailable.");
    state.velocity=Point(*velocity);
    const auto scene=Field<Address>(result.local.pawn,"C_BaseEntity","m_pGameSceneNode").value_or(0);
    const auto origin=Field<Vec3>(scene,"CGameSceneNode","m_vecAbsOrigin");
    if(!origin||!VectorValid(*origin))return fail("Current movement origin is unavailable.");
    state.origin=Point(*origin);
    result.services=Field<Address>(result.local.pawn,"C_BasePlayerPawn","m_pMovementServices").value_or(0);
    if(!result.services||memory::safe_read<Address>(result.services+0x38).value_or(0)!=result.local.pawn)
        return fail("Player movement services are unavailable.");
    state.lastImpulses={Value(result.services,"CPlayer_MovementServices","m_flCmdForwardMove"),Value(result.services,"CPlayer_MovementServices","m_flCmdLeftMove")};
    state.stamina=Value(result.services,"CCSPlayer_MovementServices","m_flStamina");
    state.surfaceFriction=Value(result.services,"CPlayer_MovementServices_Humanoid","m_flSurfaceFriction");
    state.maxSpeed=Value(result.services,"CPlayer_MovementServices","m_flMaxspeed");
    const auto weaponServices=Field<Address>(result.local.pawn,"C_BasePlayerPawn","m_pWeaponServices").value_or(0);
    const auto weaponHandle=Field<std::uint32_t>(weaponServices,"CPlayer_WeaponServices","m_hActiveWeapon").value_or(0xffffffffu);
    const auto weapon=game_access::Resolve(result.local.list,weaponHandle);
    const auto subclass=systems::schemas::lookup("C_BaseEntity",fnv1a::runtime_hash("m_nSubclassID"));
    const auto weaponData=weapon&&subclass?memory::safe_read<Address>(weapon+subclass+8).value_or(0):0;
    const auto speedOffset=systems::schemas::lookup("CCSWeaponBaseVData",fnv1a::runtime_hash("m_flMaxSpeed"));
    // Source movement uses the first (unscoped) speed entry for countersteering.
    if(weaponData&&speedOffset)state.weaponMaxSpeed=memory::safe_read<float>(weaponData+speedOffset).value_or(NAN);
    state.duckAmount=Value(result.services,"CCSPlayer_MovementServices","m_flDuckAmount");
    state.duckSpeed=Value(result.services,"CCSPlayer_MovementServices","m_flDuckSpeed");
    state.gravityScale=Value(result.local.pawn,"C_BaseEntity","m_flGravityScale");
    const auto now=GetTickCount64();
    if(!physics.valid||physics.list!=result.local.list||physics.pawn!=result.local.pawn||physics.handle!=result.local.handle||now-physics.refreshed>=250){
        Physics update;update.list=result.local.list;update.pawn=result.local.pawn;update.handle=result.local.handle;update.refreshed=now;update.valid=true;
        update.gravity=Cvar("sv_gravity");update.normal=Cvar("sv_standable_normal");update.maxSpeed=Cvar("sv_maxspeed");
        update.airAccelerate=Cvar("sv_airaccelerate");update.airWish=Cvar("sv_air_max_wishspeed");update.staminaRecovery=Cvar("sv_staminarecoveryrate");
        update.friction=Cvar("sv_friction");update.stopSpeed=Cvar("sv_stopspeed");update.accelerate=Cvar("sv_accelerate");
        update.autobhop=combat_access::ConvarBool("sv_autobunnyhopping");update.quantize=combat_access::ConvarBool("sv_quantize_movement_input");physics=update;
    }
    state.gravity=physics.gravity;state.standableNormal=physics.normal;state.maxServerSpeed=physics.maxSpeed;
    state.airAccelerate=physics.airAccelerate;state.airMaxWishspeed=physics.airWish;state.staminaRecovery=physics.staminaRecovery;
    state.friction=physics.friction;state.stopSpeed=physics.stopSpeed;state.accelerate=physics.accelerate;
    // Missing metadata is retried on the next refresh, never permanently cached.
    state.autoBunnyhop=physics.autobhop.value_or(true);state.quantizeMovement=physics.quantize.value_or(false);
    state.airValid=true;state.groundValid=true;
    const auto collision=Field<Address>(result.local.pawn,"C_BaseEntity","m_pCollision").value_or(0);
    const auto mins=Field<Vec3>(collision,"CCollisionProperty","m_vecMins");
    const auto maxs=Field<Vec3>(collision,"CCollisionProperty","m_vecMaxs");
    // Native movement helper8E1050: owner+D50, plus0x20 for current flags bit4.
    // Legacy Spectral offsetsD48/3F8 are not valid for the pinned client.
    const auto mask=memory::safe_read<std::uint64_t>(result.local.pawn+0xd50);
    if(mins&&maxs&&VectorValid(*mins)&&VectorValid(*maxs)&&mask) {
        state.mins=Point(*mins);state.maxs=Point(*maxs);result.mask=*mask|((*flags&0x10)?0x20ull:0);
        state.traceValid=api.ray&&api.filter&&memory::safe_read<Address>(api.managerSlot).value_or(0)!=0;
    }
    constexpr auto manualActions=kAttack|(1ull<<11)|(1ull<<5);
    auto actions=(buttons->value|buttons->scroll)&manualActions;
    if(base->buttons){
        const auto payload=memory::safe_read<ButtonPayload>(base->buttons+kMessagePayload);
        if(!payload)return fail("Movement protobuf buttons could not be read.");
        const auto value=payload->bits&1?payload->value:0;
        const auto changed=payload->bits&2?payload->changed:0;
        const auto scroll=payload->bits&4?payload->scroll:0;
        actions|=(value|scroll)&manualActions;
        if(!releasedJump) {
            state.buttons|=value&jump;
            state.jumpPressed|=(value&changed&jump)!=0;
        }
        state.buttons|=scroll&jump;state.jumpPressed|=(scroll&jump)!=0;
        result.jumpScrolled|=(scroll&jump)!=0;
    }
    result.jumpPressedFromButtons=state.jumpPressed;result.jumpHeldFromButtons=(state.buttons&jump)!=0;
    const auto &steps=base->subticks;
    if(steps.size<0||steps.size>256||steps.capacity<steps.size||steps.capacity>65536)return fail("Movement subtick storage is invalid.");
    if(steps.size) {
        const auto allocated=memory::safe_read<int>(steps.entries).value_or(0);
        if(!steps.entries||allocated<steps.size||allocated>steps.capacity)return fail("Movement subtick storage is invalid.");
        for(int index=0;index<steps.size;++index){
            const auto raw=memory::safe_read<Address>(steps.entries+8+index*sizeof(Address)).value_or(0);
            const auto step=raw?memory::safe_read<SubtickPayload>(raw+kMessagePayload):std::nullopt;
            if(!step||!std::isfinite(step->when)||step->when<0||step->when>1)return fail("Movement subtick data is invalid.");
            // Button/analog events do not constrain the yaw strafe window.
            // A late native Jump event must not disable quantized air strafe.
            if(step->bits&0x60)state.maxSubtickWhen=std::max(state.maxSubtickWhen,step->when);
            result.hasMovementSubticks|=(step->bits&0x19)!=0;
            if((step->bits&3)==3&&step->pressed) {
                actions|=step->button&manualActions;
                if(step->button&jump){state.jumpPressed=true;state.buttons|=jump;}
            }
        }
    }
    state.shooting=shotRequested||(actions&(manualActions&~kAttack))!=0;
    if(actions&kAttack){
        const auto type=Field<int>(weaponData,"CCSWeaponBaseVData","m_WeaponType");
        const auto next=Field<int>(weapon,"C_BasePlayerWeapon","m_nNextPrimaryAttackTick");
        const auto clip=Field<int>(weapon,"C_BasePlayerWeapon","m_iClip1");
        const auto reload=Field<bool>(weapon,"C_CSWeaponBase","m_bInReload");
        const auto tick=base->bits&0x20?std::optional<int>{base->clientTick}:
            Field<int>(result.local.controller,"CBasePlayerController","m_nTickBase");
        const bool known=type&&*type>=1&&*type<=6&&next&&*next>=0&&clip&&reload&&tick&&*tick>0;
        state.shooting|=!known||(!*reload&&*clip>0&&*tick>=*next);
    }
    state.valid=result.valid=true;result.failure=nullptr;return result;
}
void Reset(){physics={};}
bool Fresh(const Context &context) {
    if(!context.valid)return false;
    const auto player=game_access::ReadLocalPlayer(api.client);
    const auto current=combat_access::ReadInputCommand(context.input);
    if(player.state!=game_access::PlayerState::Alive||player.list!=context.local.list||player.controller!=context.local.controller||
       player.pawn!=context.local.pawn||player.handle!=context.local.handle||!current||current->command!=context.command||
       current->sequence!=context.state.commandNumber||current->weaponSelectionPending)return false;
    const auto proto=memory::safe_read<combat_bindings::CommandProto>(context.command+combat_bindings::kCommandProto);
    combat_bindings::BaseCommand base{};combat_bindings::ButtonState buttons{};
    return proto&&(proto->bits&1)&&proto->base+combat_bindings::kMessagePayload==context.base&&
        memory::copy(&base,context.base,sizeof(base))&&memory::copy(&buttons,context.command+combat_bindings::kCommandButtons,sizeof(buttons))&&
        !std::memcmp(&base,&context.baseSnapshot,sizeof(base))&&!std::memcmp(&buttons,&context.buttons,sizeof(buttons));
}
__declspec(noinline) std::optional<movement_policy::Trace> TraceHull(const Context &context,movement_policy::Vec3 start,
        movement_policy::Vec3 end,movement_policy::Vec3 mins,movement_policy::Vec3 maxs) {
    if(!context.valid||!context.state.traceValid||!Fresh(context))return {};
    const auto manager=memory::safe_read<Address>(api.managerSlot).value_or(0);if(!manager)return {};
    Vec3 from{start.x,start.y,start.z},to{end.x,end.y,end.z};
    if(!VectorValid(from)||!VectorValid(to)||!movement_policy::detail::Finite(mins)||!movement_policy::detail::Finite(maxs)||
       mins.x>=maxs.x||mins.y>=maxs.y||mins.z>=maxs.z)return {};
    __try {
        alignas(8) std::array<std::byte,0x48> filter{};
        memory::call<void>(api.filter,filter.data(),context.local.pawn,context.mask,11);
        systems::tracing::ray ray{};ray.mins={mins.x,mins.y,mins.z};ray.maxs={maxs.x,maxs.y,maxs.z};ray.type=2;
        systems::tracing::result hit{};hit.fraction=NAN;
        // Verified A19EF0 initializesC0 output itself; source's legacy22FC40
        // expression has a different ABI and must never be called as a hull.
        memory::call<bool>(api.ray,manager,&ray,&from,&to,filter.data(),&hit);
        if(hit.all_solid||!std::isfinite(hit.fraction)||hit.fraction<0||hit.fraction>1||!VectorValid(hit.end_pos)||!combat_access::Finite(hit.normal))return {};
        movement_policy::Trace result{hit.fraction,Point(hit.normal),Point(hit.end_pos)};
        return movement_policy::detail::TraceValid(result)?std::optional{result}:std::nullopt;
    } __except(EXCEPTION_EXECUTE_HANDLER){return {};}
}
__declspec(noinline) Address AcquireSubtick(Address field) {
    using namespace combat_bindings;
    if(!api.allocate||!api.append||!field)return 0;
    __try {
        auto data=memory::safe_read<RepeatedField>(field);
        if(!data||data->size<0||data->size>=256||data->capacity<data->size||data->capacity>65536)return 0;
        Address raw{};
        const auto allocated=memory::safe_read<int>(data->entries).value_or(-1);
        if((data->entries&&(allocated<data->size||allocated>data->capacity))||(!data->entries&&(data->size||data->capacity)))return 0;
        if(data->entries&&allocated>data->size) {
            raw=memory::safe_read<Address>(data->entries+8+data->size*sizeof(Address)).value_or(0);
            if(!raw)return 0;
            const auto size=data->size+1;
            if(!memory::store(field+offsetof(RepeatedField,size),&size,sizeof(size)))return 0;
        } else {
            if(data->entries&&allocated<data->size)return 0;
            raw=memory::call<Address>(api.allocate,data->arena);
            if(!raw)return 0;
            if(memory::call<Address>(api.append,field,raw)!=raw)return 0;
        }
        // The transaction overwrites the payload after alias checks. Clearing it
        // here would destroy rollback data before commit validation completed.
        return raw;
    } __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
}
