#include "core/user_defaults.hpp"
#include "features/combat/hvh.h"
#include "features/combat/backtrack.h"
#include "features/combat/detail/hvh_math.hpp"
#include "features/combat/detail/hvh_accuracy.hpp"
#include "features/combat/detail/hvh_scan.hpp"
#include <memory>
#include <unordered_map>
#include "features/combat/detail/hvh_points.hpp"
#include "features/combat/detail/hvh_input.hpp"
#include "features/combat/detail/hvh_antiaim.hpp"
#include "features/movement/movement.h"
#include "game/combat_access.h"
#include "game/autowall_probe.h"
#include "minhook/include/MinHook.h"
#include "ui/gui_capture.h"

// The native command, seed search and recoil convention come from Spectral.win.
// Shooting and body orientation share the same visible-camera restoration.
namespace hvh {
namespace {
using Address = game_access::Address;
using Vec3 = combat_access::Vec3;
using MoveFn = void(__fastcall*)(Address, int, bool);
using ViewFn = void(__fastcall*)(Address, int);
struct Settings {
    bool enabled{true};
    int key{VK_XBUTTON2};
    int activationMode{};
    int hitboxes{};
    float fieldOfView{180.f}, minimumDamage{25.f};
    float hitchance{70.f}, pointScale{.65f};
    bool autoStop{true}, autoScope{true}, multipoints{true};
    bool autoFire{true}, wallPenetration{true}, noSpread{true}, recoilCorrection{true};
    bool showSpreadPrediction{true};
    bool antiAim{}, antiAimUseKey{};
    int yawMode{1}, pitchMode{1};
    float yawOffset{}, spinSpeed{360.f};
    bool operator==(const Settings&) const = default;
} settings=user_defaults::Combat(Settings{});
std::mutex settingsMutex, commandMutex;
std::atomic<bool> running{}, menuOpen{true};
std::atomic<bool> combatReady{};
std::atomic<unsigned> callbacks{};
std::atomic<float> viewportWidth{}, viewportHeight{};
std::atomic<unsigned> commandEpoch{};
std::atomic<const char*> status{"HVH has not initialized."};
std::atomic<const char*> spreadStatus{"Spread compensation needs a server with predictable spread."};
std::atomic<const char*> antiAimStatus{"Anti aim is disabled."};
MoveFn originalMove{};
ViewFn originalView{};
void* hookTarget{};
void* viewHookTarget{};
bool hookCreated{};
bool viewHookCreated{};
thread_local bool inMove{};
std::mutex shotMutex;
hvh_input::SilentCommand silentCommand, cameraCommand;
bool capturing{};
std::array<bool, 256> previousKeys{};
struct ScanSnapshot {
    combat_access::TargetScan players{};
    unsigned aimPoints{}, inView{};
    ULONGLONG time{};
};
std::mutex scanMutex;
ScanSnapshot lastScan;
std::mutex predictionMutex;
std::optional<SpreadPrediction> lastPrediction;
void ClearPrediction() { std::lock_guard lock(predictionMutex); lastPrediction.reset(); }

struct LivePoseSource {
    Address scene{}, state{}, binding{}, model{}, cache{};
    int count{}, modelCount{};
    std::uint16_t cacheCount{};
    float simulationTime{};
    Vec3 origin{};
};
bool Same(const Vec3& a, const Vec3& b) { return a.x==b.x && a.y==b.y && a.z==b.z; }
bool Same(const combat_access::Weapon& a,const combat_access::Weapon& b) {
    return a.address==b.address && a.data==b.data && a.handle==b.handle && a.type==b.type &&
        a.definition==b.definition && a.bullets==b.bullets && a.clip==b.clip && a.damage==b.damage &&
        a.penetration==b.penetration && a.range==b.range && a.rangeModifier==b.rangeModifier &&
        a.armorRatio==b.armorRatio && a.headMultiplier==b.headMultiplier && a.recoil==b.recoil &&
        a.inaccuracy==b.inaccuracy && a.spread==b.spread;
}
combat_access::Target AimHitboxes(combat_access::Target target,int selection) {
    const auto boxes=target.hitboxes;
    target.hitboxes.count=0;
    for(const auto& box:boxes) {
        const auto region=hvh_points::detail::NativeRegion(box.index,box.group);
        if(hvh_points::Accept({{},box.index,false,region},static_cast<hvh_points::Selection>(selection)))
            target.hitboxes.entries[target.hitboxes.count++]=box;
    }
    return target;
}
bool Same(const LivePoseSource& a, const LivePoseSource& b) {
    return a.scene==b.scene && a.state==b.state && a.binding==b.binding && a.model==b.model &&
        a.cache==b.cache && a.count==b.count && a.modelCount==b.modelCount && a.cacheCount==b.cacheCount &&
        a.simulationTime==b.simulationTime && Same(a.origin,b.origin);
}
std::optional<LivePoseSource> ReadLivePoseSource(Address pawn) {
    using combat_access::Field;
    LivePoseSource source;
    source.scene=Field<Address>(pawn,"C_BaseEntity","m_pGameSceneNode").value_or(0);
    const auto simulation=Field<float>(pawn,"C_BaseEntity","m_flSimulationTime");
    const auto origin=Field<Vec3>(source.scene,"CGameSceneNode","m_vecAbsOrigin");
    const auto stateOffset=systems::schemas::lookup("CSkeletonInstance","m_modelState"_hash);
    if(!source.scene || !stateOffset || !simulation || !std::isfinite(*simulation) || *simulation<=0 ||
        !origin || !combat_access::Finite(*origin))return {};
    source.simulationTime=*simulation; source.origin=*origin; source.state=source.scene+stateOffset;
    source.binding=Field<Address>(source.state,"CModelState","m_hModel").value_or(0);
    source.model=memory::safe_read<Address>(source.binding).value_or(0);
    if(!source.model)return {};
    source.modelCount=memory::safe_read<int>(source.model+0x178).value_or(0);
    source.cacheCount=memory::safe_read<std::uint16_t>(source.state+0x94).value_or(0);
    source.cache=memory::safe_read<Address>(source.state+0x80).value_or(0);
    if(source.modelCount<=0 || source.modelCount>1024 || !source.cacheCount || source.cacheCount>1024 || !source.cache)return {};
    source.count=std::min(source.modelCount,static_cast<int>(source.cacheCount));
    return source;
}
struct Geometry { combat_access::Hitbox box; systems::bones::data bone, nativeBone; };
struct Pose { std::vector<Geometry> geometry; std::optional<LivePoseSource> source; };
bool CurrentPose(Address pawn, const Pose& pose) {
    if(!pose.source)return true; // Recorded poses have their own identity/time validation.
    const auto current=ReadLivePoseSource(pawn);
    if(!current || !Same(*current,*pose.source))return false;
    for(const auto& geometry:pose.geometry) {
        const auto bone=memory::safe_read<systems::bones::data>(current->cache+
            static_cast<std::size_t>(geometry.box.bone)*sizeof(systems::bones::data));
        const auto& old=geometry.nativeBone;
        if(!bone || !Same(bone->position,old.position) || bone->scale!=old.scale ||
            bone->rotation.x!=old.rotation.x || bone->rotation.y!=old.rotation.y ||
            bone->rotation.z!=old.rotation.z || bone->rotation.w!=old.rotation.w)return false;
    }
    const auto after=ReadLivePoseSource(pawn);
    return after && Same(*after,*current);
}
struct Acquisition {
    hvh_math::Candidate target;
    Address list{}, localPawn{}, weapon{};
    std::uint32_t localHandle{}, weaponHandle{};
    unsigned epoch{};
    ULONGLONG time{};
    std::optional<LivePoseSource> source;
    Settings settings;
} acquisition;
struct ScanAttempt {
    bool valid{};
    combat_access::Context context;
    Settings settings;
    combat_bindings::ButtonState buttons{};
    std::vector<std::pair<hvh_math::Candidate,std::shared_ptr<const Pose>>> poses;
} lastAttempt;
bool Fresh(const combat_access::Context&,const hvh_math::Candidate&);
bool SameAttempt(const combat_access::Context& context,const Settings& config) {
    if(!lastAttempt.valid || !(lastAttempt.settings==config))return false;
    const auto& old=lastAttempt.context;
    if(context.command!=old.command || context.base!=old.base || context.commandNumber!=old.commandNumber ||
        context.tick!=old.tick || context.local.list!=old.local.list || context.local.pawn!=old.local.pawn ||
        context.local.controller!=old.local.controller || context.local.team!=old.local.team ||
        context.local.handle!=old.local.handle || !Same(context.weapon,old.weapon) ||
        context.accuracyValid!=old.accuracyValid ||
        !Same(context.eye,old.eye) || !Same(context.angles,old.angles) || !Same(context.punch,old.punch) ||
        context.shotTiming.has_value()!=old.shotTiming.has_value() || (context.shotTiming &&
        (context.shotTiming->tick!=old.shotTiming->tick || context.shotTiming->fraction!=old.shotTiming->fraction)))return false;
    const auto buttons=memory::safe_read<combat_bindings::ButtonState>(context.command+combat_bindings::kCommandButtons);
    constexpr std::uint64_t actions=combat_bindings::kAttack | (1ull<<1) | (1ull<<5) | (1ull<<11);
    // Directional bits can be rewritten by our movement/AA pass. Eye, view
    // and weapon accuracy already describe their effect on this shot scan.
    if(!buttons || ((buttons->value^lastAttempt.buttons.value)&actions) ||
        ((buttons->changed^lastAttempt.buttons.changed)&actions) ||
        ((buttons->scroll^lastAttempt.buttons.scroll)&actions))return false;
    const auto targets=combat_access::ReadTargets(context,nullptr,false);
    if(targets.size()!=lastAttempt.poses.size())return false;
    for(const auto& target:targets) {
        const auto found=std::find_if(lastAttempt.poses.begin(),lastAttempt.poses.end(),[&](const auto& entry){
            const auto& previous=entry.first;
            return previous.pawn==target.pawn && previous.handle==target.handle &&
                previous.team==target.team && previous.health==target.health;
        });
        if(found==lastAttempt.poses.end())return false;
    }
    // Failed scans can be reused only while the actual remote geometry stays
    // unchanged. A mouse update, weapon change or new target pose scans now.
    for(const auto& [target,pose]:lastAttempt.poses)
        if(!Fresh(context,target) || !CurrentPose(target.pawn,*pose))return false;
    return true;
}
void RememberAttempt(const combat_access::Context& context,const Settings& config,
                     std::vector<std::pair<hvh_math::Candidate,std::shared_ptr<const Pose>>> poses) {
    const auto buttons=memory::safe_read<combat_bindings::ButtonState>(context.command+combat_bindings::kCommandButtons);
    lastAttempt={};
    if(!buttons)return;
    lastAttempt.valid=!poses.empty();lastAttempt.context=context;lastAttempt.settings=config;
    lastAttempt.buttons=*buttons;lastAttempt.poses=std::move(poses);
}

// Called only under commandMutex. Level shutdown merely advances the epoch;
// the input thread owns all command and movement-state cleanup.
void SynchronizeSession() {
    const auto epoch = hvh_input::worldEpoch.load(std::memory_order_acquire);
    if (commandEpoch.load(std::memory_order_relaxed) == epoch) return;
    combat_access::ResetTargets();
    acquisition={}; lastAttempt={};
    movement::Reset();
    { std::lock_guard lock(shotMutex); silentCommand.Clear(); cameraCommand.Clear(); }
    { std::lock_guard lock(scanMutex); lastScan = {}; }
    ClearPrediction();
    commandEpoch.store(epoch, std::memory_order_release);
}
Settings Get() { std::lock_guard lock(settingsMutex); return settings; }
void Save(Settings value) {
    const auto bounded = [](float number, float low, float high, float fallback) {
        return std::isfinite(number) ? std::clamp(number, low, high) : fallback;
    };
    value.hitboxes = std::clamp(value.hitboxes, 0, 3);
    value.activationMode = std::clamp(value.activationMode, 0, 1);
    value.fieldOfView = bounded(value.fieldOfView, 1.f, 180.f, 180.f);
    value.minimumDamage = bounded(value.minimumDamage, 1.f, 100.f, Settings{}.minimumDamage);
    value.hitchance = bounded(value.hitchance, 0.f, 100.f, 70.f);
    value.pointScale = bounded(value.pointScale, .1f, .9f, .65f);
    value.yawMode = std::clamp(value.yawMode, 0, 1);
    value.pitchMode = std::clamp(value.pitchMode, 0, 2);
    value.yawOffset = bounded(value.yawOffset, -180.f, 180.f, 0.f);
    value.spinSpeed = bounded(value.spinSpeed, 30.f, 1440.f, 360.f);
    std::lock_guard lock(settingsMutex); settings = value;
}
bool Focused() {
    DWORD process{};
    const auto window = GetForegroundWindow();
    return window && GetWindowThreadProcessId(window, &process) && process == GetCurrentProcessId();
}
bool GameplayInput() {
    return running.load(std::memory_order_acquire) && !menuOpen.load(std::memory_order_acquire) && Focused();
}
bool KeyHeld(const Settings& value) {
    return value.key > 0 && value.key < 256 && (GetAsyncKeyState(value.key) & 0x8000);
}
bool Activated(const Settings& value) {
    return hvh_input::Activated(value.enabled, GameplayInput(),
        static_cast<hvh_input::ActivationMode>(value.activationMode), KeyHeld(value));
}
bool AntiAimActivated(const Settings& value) {
    return hvh_input::Activated(value.antiAim, GameplayInput(), value.antiAimUseKey ?
        static_cast<hvh_input::ActivationMode>(value.activationMode) : hvh_input::ActivationMode::Always, KeyHeld(value));
}
hvh_math::Vec3 Point(const Vec3& v) { return {v.x, v.y, v.z}; }
Vec3 Point(hvh_math::Vec3 v) { return {v.x, v.y, v.z}; }
void PublishPrediction(const combat_access::Context& context, hvh_math::Vec3 angles,
                       std::optional<hvh_math::PredictedShot> exact, bool compensated) {
    if (!context.valid || !context.accuracyValid || !hvh_math::detail::Finite(angles) ||
        !combat_access::Finite(context.eye) || !std::isfinite(context.weapon.range) ||
        context.weapon.range <= 0.f || context.weapon.range > 32768.f ||
        !std::isfinite(context.weapon.inaccuracy) || context.weapon.inaccuracy < 0.f ||
        !std::isfinite(context.weapon.spread) || context.weapon.spread < 0.f) { ClearPrediction(); return; }
    SpreadPrediction forecast;
    forecast.origin=Point(context.eye); forecast.angles=angles; forecast.exact=exact;
    forecast.range=context.weapon.range; forecast.radius=context.weapon.inaccuracy+context.weapon.spread;
    if (!std::isfinite(forecast.radius)) { ClearPrediction(); return; }
    forecast.localHandle=context.local.handle; forecast.weaponHandle=context.weapon.handle;
    forecast.epoch=hvh_input::worldEpoch.load(std::memory_order_acquire);
    forecast.time=GetTickCount64(); forecast.compensated=compensated && exact.has_value();
    std::lock_guard lock(predictionMutex); lastPrediction=forecast;
}
void PredictCurrentShot(Address input, const combat_access::CommittedShot* committed) {
    const auto context=combat_access::ReadContext(input,false);
    const auto timing=combat_access::ShotTiming(context);
    if (!context.valid || !context.accuracyValid || !timing || context.weapon.definition==64 ||
        (committed && (committed->sequence!=context.commandNumber ||
        committed->localHandle!=context.local.handle || committed->weaponHandle!=context.weapon.handle))) {
        ClearPrediction(); return;
    }
    const auto angles=hvh_math::ShotAim(Point(committed?committed->angles:context.angles),Point(context.punch),false);
    if (!angles) { ClearPrediction(); return; }
    const auto privateSeed=combat_access::ConvarBool("sv_usercmd_custom_random_seed");
    std::optional<hvh_math::PredictedShot> exact;
    if (context.weapon.bullets==1 && privateSeed && !*privateSeed)
        exact=hvh_math::PredictShot(*angles,timing->tick,
            [](hvh_math::Vec3 aim,int tick){return combat_access::SpreadSeed(Point(aim),tick);},
            [&](std::uint32_t seed)->std::optional<hvh_math::Vec2>{
                const auto spread=combat_access::Spread(context.weapon,seed);
                if (!std::isfinite(spread.x) || !std::isfinite(spread.y)) return {};
                return hvh_math::Vec2{spread.x,spread.y};
            });
    PublishPrediction(context,*angles,exact,false);
}
bool Fresh(const combat_access::Context& context, const hvh_math::Candidate& target) {
    if (game_access::Resolve(context.local.list, target.handle) != target.pawn ||
        game_access::Resolve(context.local.list, context.local.handle) != context.local.pawn ||
        game_access::Resolve(context.local.list, context.weapon.handle) != context.weapon.address) return false;
    using combat_access::Field;
    const auto team = Field<std::uint8_t>(target.pawn, "C_BaseEntity", "m_iTeamNum").value_or(0);
    const auto scene = Field<Address>(target.pawn, "C_BaseEntity", "m_pGameSceneNode").value_or(0);
    return (team == 2 || team == 3) && team != context.local.team &&
        scene && !Field<bool>(scene, "CGameSceneNode", "m_bDormant").value_or(true) &&
        Field<int>(target.pawn, "C_BaseEntity", "m_iHealth").value_or(0) > 0 &&
        Field<std::uint8_t>(target.pawn, "C_BaseEntity", "m_lifeState").value_or(1) == 0 &&
        !Field<bool>(target.pawn, "C_CSPlayerPawn", "m_bGunGameImmunity").value_or(true);
}
bool PrepareAcquired(Address input,const combat_access::Context& context,const Settings& config,bool& preparingShot) {
    if(!config.autoStop || !acquisition.time || GetTickCount64()-acquisition.time>64 ||
        !(acquisition.settings==config) || acquisition.epoch!=hvh_input::worldEpoch.load(std::memory_order_acquire) ||
        acquisition.list!=context.local.list || acquisition.localPawn!=context.local.pawn ||
        acquisition.localHandle!=context.local.handle || acquisition.weapon!=context.weapon.address ||
        acquisition.weaponHandle!=context.weapon.handle || !Fresh(context,acquisition.target))return false;
    const auto current=ReadLivePoseSource(acquisition.target.pawn);
    if(!current || !acquisition.source || current->model!=acquisition.source->model ||
        (current->origin-acquisition.source->origin).length_sqr()>64.f*64.f)return false;
    // This short-lived acquisition authorizes braking only. Every shot still
    // gets fresh geometry, reachability, damage and probability checks.
    if(const auto prepared=movement::PrepareShot(input,true,false)) {
        preparingShot=true;
        std::lock_guard lock(shotMutex);
        cameraCommand.Applied({prepared->command,prepared->sequence,prepared->localHandle,false});
        return true;
    }
    return false;
}
struct ScanBudget {
    unsigned traces{48}, accuracy{16}, seedCalls{64}, samples{};
    combat_access::Weapon weapon;
    bool weaponKnown{};
    hvh_accuracy::SpreadCache<128> spread;
    std::unordered_map<std::uint32_t,std::optional<hvh_math::Vec2>> corrections;
};
void ProcessShot(Address input, std::optional<combat_access::CommittedShot>& shotApplied,
                 std::optional<hvh_math::Candidate>& shotTarget, bool& preparingShot,
                 ScanBudget* sharedBudget=nullptr,bool retried=false) {
    ScanBudget ownBudget;
    auto& budget=sharedBudget?*sharedBudget:ownBudget;
    auto setting = Get();
    if (!Activated(setting)) { acquisition={}; lastAttempt={}; return; }
    const auto context = combat_access::ReadContext(input, false);
    if (!context.valid) { acquisition={}; lastAttempt={}; status = context.failure; return; }
    if (!setting.autoFire && !combat_access::AttackDown(context)) {
        status = "Waiting for primary attack."; return;
    }
    if (!context.canShoot) {
        PrepareAcquired(input,context,setting,preparingShot);
        status = preparingShot?"Preparing movement during weapon cooldown.":"Waiting for the weapon to be ready.";
        return;
    }
    if(!retried && SameAttempt(context,setting)) {
        PrepareAcquired(input,context,setting,preparingShot);
        return;
    }
    if(budget.weaponKnown && (budget.weapon.address!=context.weapon.address ||
        budget.weapon.definition!=context.weapon.definition || budget.weapon.bullets!=context.weapon.bullets ||
        budget.weapon.inaccuracy!=context.weapon.inaccuracy || budget.weapon.spread!=context.weapon.spread ||
        budget.weapon.recoil!=context.weapon.recoil)) { budget.spread={}; budget.corrections.clear(); }
    budget.weapon=context.weapon;budget.weaponKnown=true;
    // The R8 needs a separate cock/release sequence. Never synthesize an
    // unverified primary-attack timing for it through this ordinary gun path.
    if (context.weapon.definition == 64) { status = "The revolver's cocking sequence is not supported."; return; }

    const auto randomSeed = combat_access::ConvarBool("sv_usercmd_custom_random_seed");
    const bool compensate = setting.noSpread && context.weapon.bullets == 1 && randomSeed && !*randomSeed;
    spreadStatus = !setting.noSpread ? "No-spread compensation is disabled in HVH settings." :
        compensate ? "Spread compensation is available on this server." :
        context.weapon.bullets > 1 ? "Shotguns retain their multi-pellet spread pattern." :
        !randomSeed ? "Server spread setting is unavailable; no-spread compensation is inactive." :
        "Server adds a private random seed; no-spread compensation is inactive.";
    const auto timing = combat_access::ShotTiming(context);
    if (!timing) { status = "Waiting for a valid shot-history timestamp."; return; }
    if (compensate && !context.accuracyValid) {
        status = "Waiting for valid weapon accuracy."; return;
    }

    hvh_math::View view;
    view.localPawn = context.local.pawn; view.localHandle = context.local.handle; view.localTeam = context.local.team;
    view.eye = Point(context.eye); view.angles = Point(context.angles); view.range = context.weapon.range;
    view.maxFov = setting.fieldOfView;
    // FOV is an angular limit around the visible input angles, including
    // off-screen and rear targets when 180 degrees is selected.
    view.allowBehind = true;
    std::vector<hvh_math::Candidate> points;
    std::vector<backtrack::Snapshot> pointRecords;
    std::vector<std::shared_ptr<const Pose>> pointPoses;
    std::vector<hvh_math::Selection> order;
    std::vector<hvh_scan::Point> scheduling;
    std::vector<std::pair<hvh_math::Candidate,std::shared_ptr<const Pose>>> livePoses;
    ScanSnapshot scan;
    const auto players = combat_access::ReadTargets(context, &scan.players,false);
    bool completePoses=true,changedPose{};
    points.reserve(players.size() * 20); order.reserve(players.size() * 20);
    const auto append = [&](const combat_access::Target& player, const std::vector<hvh_points::Point>& aimPoints,
                            const std::vector<hvh_points::Point>& centers,const std::shared_ptr<const Pose>& pose,
                            unsigned poseOrder,const backtrack::Snapshot& record = {}) {
        for (const auto& point : aimPoints) {
            if (!hvh_points::Accept(point, static_cast<hvh_points::Selection>(setting.hitboxes))) continue;
            ++scan.aimPoints;
            hvh_math::Candidate candidate;
            candidate.pawn = player.pawn; candidate.handle = player.handle; candidate.team = player.team;
            candidate.health = player.health; candidate.valid = true;
            candidate.point = Point(point.position); candidate.hitbox = point.hitbox;
            if (const auto projected = hvh_math::ProjectCandidate(view, candidate, points.size(), false)) {
                const bool center=std::any_of(centers.begin(),centers.end(),[&](const auto& value){
                    return value.hitbox==point.hitbox && value.region==point.region &&
                        (value.position-point.position).length_sqr()<=.0001f;
                });
                scheduling.push_back({points.size(),player.handle,poseOrder,!center,
                    point.region==hvh_points::Region::Head?0u:point.region==hvh_points::Region::Body?1u:2u,
                    projected->angularDistance,projected->distance});
                order.push_back(*projected); points.push_back(candidate); pointRecords.push_back(record); pointPoses.push_back(pose);
            }
        }
    };
    for (const auto& player : players) {
        auto liveTarget = player;
        auto pose = std::make_shared<Pose>();
        pose->source=ReadLivePoseSource(player.pawn);
        if(!pose->source){completePoses=false;continue;}
        liveTarget.hitboxes=combat_access::ReadHitboxes(pose->source->scene);
        if(liveTarget.hitboxes.count<0 || liveTarget.hitboxes.count>static_cast<int>(liveTarget.hitboxes.entries.size())) {
            completePoses=false;continue;
        }
        if(liveTarget.hitboxes.count>0){++scan.players.hitboxPlayers;scan.players.hitboxCount+=liveTarget.hitboxes.count;}
        pose->geometry.reserve(liveTarget.hitboxes.count);
        for (int index = 0; index < liveTarget.hitboxes.count; ++index) {
            auto& box = liveTarget.hitboxes.entries[index];
            if (box.bone < 0 || box.bone >= 1024) {completePoses=false;continue;}
            if(box.bone>=pose->source->count){completePoses=false;box.bone=-1;continue;}
            const auto rawBone=memory::safe_read<systems::bones::data>(pose->source->cache+
                static_cast<std::size_t>(box.bone)*sizeof(systems::bones::data));
            if(!rawBone){completePoses=false;box.bone=-1;continue;}
            auto bone=*rawBone;
            const auto scaled = hvh_points::ScaleGeometry(box, bone.scale);
            if (!scaled) { completePoses=false;box.bone = -1; continue; }
            box = *scaled;
            const auto& q = bone.rotation;
            const auto norm = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
            const bool rotationValid = std::isfinite(norm) && std::abs(norm - 1.f) <= .1f;
            if (!box.translation_only && !rotationValid) { completePoses=false;box.bone = -1; continue; }
            if (box.translation_only && !rotationValid) bone.rotation = math::quaternion{};
            if (hvh_points::detail::ValidPosition(bone.position)) pose->geometry.push_back({box,bone,*rawBone});
            else completePoses=false;
        }
        if(!CurrentPose(player.pawn,*pose)){completePoses=false;changedPose=true;continue;}
        if(pose->geometry.empty())completePoses=false;
        hvh_math::Candidate identity;
        identity.pawn=player.pawn;identity.handle=player.handle;identity.team=player.team;
        identity.health=player.health;identity.valid=true;
        livePoses.emplace_back(identity,pose);
        const auto nativeBone=[&](unsigned bone) {
            for(const auto& geometry:pose->geometry)if(geometry.box.bone==static_cast<int>(bone))return geometry.bone;
            return systems::bones::data{};
        };
        const auto skeleton=[&]{return setting.hitboxes==2?std::array<systems::bones::data,27>{}:
            systems::g_bones.get_skeleton(player.pawn,true);};
        const auto aimTarget=AimHitboxes(liveTarget,setting.hitboxes);
        const auto centers=hvh_points::Collect(aimTarget,nativeBone,skeleton,0.f);
        append(player,setting.multipoints?hvh_points::Collect(aimTarget,nativeBone,skeleton,setting.pointScale):centers,
            centers,pose,0);
        const auto history = backtrack::Records(player.pawn, player.handle);
        // Bound historical expansion: newest, middle and oldest valid poses.
        std::array<std::size_t, 3> indices{0, history.size()/2, history.empty()?0:history.size()-1};
        std::size_t previous = SIZE_MAX;
        for (const auto index : indices) {
            if (index == previous || index >= history.size()) continue;
            previous = index;
            const auto& record = history[index];
            auto historicalPose = std::make_shared<Pose>();
            for (const auto& box : record->target.hitboxes)
                if (box.bone >= 0 && static_cast<std::size_t>(box.bone) < record->bones.size())
                    historicalPose->geometry.push_back({box,record->bones[box.bone],record->bones[box.bone]});
            const auto historicalTarget=AimHitboxes(record->target,setting.hitboxes);
            const auto historicalPoints = hvh_points::Collect(historicalTarget,
                [&](unsigned bone) { return bone < record->bones.size() ? record->bones[bone] : systems::bones::data{}; },
                [] { return std::array<systems::bones::data, 27>{}; }, setting.multipoints ? setting.pointScale : 0.f);
            const auto historicalCenters=hvh_points::Collect(historicalTarget,
                [&](unsigned bone){return bone<record->bones.size()?record->bones[bone]:systems::bones::data{};},
                []{return std::array<systems::bones::data,27>{};},0.f);
            append(player,historicalPoints,historicalCenters,historicalPose,static_cast<unsigned>(index)+1,record);
        }
    }
    scan.inView = static_cast<unsigned>(order.size()); scan.time = GetTickCount64();
    { std::lock_guard lock(scanMutex); lastScan = scan; }
    const auto scanOrder=hvh_scan::Order(scheduling,budget.traces);
    if(completePoses)RememberAttempt(context,setting,std::move(livePoses));else lastAttempt={};
    if (!order.empty()) status = "No scanned enemy point can be reached.";
    else if (!scan.players.controllers) status = "No other players detected.";
    else if (!scan.players.pawns) status = "Players found, but their current pawn handles are unavailable.";
    else if (!scan.players.enemies) status = "No living enemies detected.";
    else if (!scan.players.eligibleEnemies) status = "Enemies are dormant, spawn-protected, or missing readiness data.";
    else if (!scan.aimPoints) status = "Enemies found, but the selected hitbox points are unavailable.";
    else status = "Enemy aim points are outside the configured FOV or weapon range.";
    // Bound native tracing even when a crowded server exposes every hitbox.
    std::optional<std::size_t> needsAccuracy;
    struct Qualified {
        hvh_math::Candidate candidate;
        Vec3 angles;
        std::optional<combat_bindings::BacktrackTiming> targetTiming;
        backtrack::Snapshot record;
        std::shared_ptr<const Pose> pose;
        float damage{}, chance{}, fov{};
        bool penetrated{};
        hvh_accuracy::QualifiedShot rank;
        std::optional<hvh_math::PredictedShot> prediction;
    };
    std::optional<Qualified> best;
    bool transientUnavailable{};
    for (const auto pointIndex : scanOrder) {
        const auto& selected=order[pointIndex];
        if(!budget.traces)break;
        if (!Activated(Get())) return;
        const auto& candidate = points[selected.index];
        const auto& record = pointRecords[selected.index];
        const auto& pose = *pointPoses[selected.index];
        if(!CurrentPose(candidate.pawn,pose)){changedPose=true;continue;}
        const auto intersection = [&](const Vec3& delta) -> std::optional<std::pair<float, int>> {
            float closest = 1.f; int group = -1;
            for (const auto& geometry : pose.geometry) {
                float fraction = 1.f;
                if (combat_access::RayHitbox(context.eye, delta, geometry.box, geometry.bone, fraction) &&
                    std::isfinite(fraction) && fraction >= 0 && fraction < closest) {
                    closest = fraction;
                    group = geometry.box.group >= 0 ? geometry.box.group : combat_access::Hitgroup(geometry.box.index);
                }
            }
            return group >= 0 ? std::optional{std::pair{closest, group}} : std::nullopt;
        };
        auto delta = Point(candidate.point) - context.eye;
        const auto distance = delta.length();
        if (!std::isfinite(distance) || distance <= 0) continue;
        delta = delta * (context.weapon.range / distance);
        const auto direct = intersection(delta);
        if (!direct || direct->second <= 0 || direct->second > 8) continue;
        const auto group = direct->second;
        const auto region = hvh_points::detail::NativeRegion(-1, group);
        if (!hvh_points::Accept({{}, -1, false, region}, static_cast<hvh_points::Selection>(setting.hitboxes))) continue;
        const auto desired = hvh_math::ShotAim(selected.angles, Point(context.punch), setting.recoilCorrection);
        if (!desired) { status = "The requested shot angle is unavailable."; continue; }
        const auto physicalRay = hvh_math::ShotDirection(*desired, {});
        const auto physicalHit = physicalRay ? intersection(Point(*physicalRay) * context.weapon.range) : std::nullopt;
        // Damage and probability must describe the same first hitgroup, even
        // when recoil correction is disabled and punch moves the physical ray.
        if (!physicalHit || physicalHit->second != group) continue;
        std::optional<combat_bindings::BacktrackTiming> targetTiming;
        std::optional<Vec3> historicalIntersection;
        if (record) {
            targetTiming = backtrack::Timing(*record);
            historicalIntersection = backtrack::Intersection(*record, context.eye, Point(candidate.point));
            if (!targetTiming || !historicalIntersection) continue;
        }
        // Reachability uses native arrival damage. A nearer arm may cover the
        // selected center, so do not claim that center's damage multiplier.
        --budget.traces;
        const auto hit = record ? autowall_probe::EvaluateHistoricalTarget(candidate.pawn, candidate.handle,
            *historicalIntersection, *timing, group) :
            autowall_probe::EvaluateTarget(candidate.pawn, candidate.handle, Point(candidate.point), group, timing);
        if (!hit.valid) {
            lastAttempt={};
            status = autowall_probe::ReasonText(hit.reason);
            if (hit.reason == autowall_probe::Reason::InvalidTarget || hit.reason == autowall_probe::Reason::InvalidPoint) continue;
            return;
        }
        if (!hit.reached || !std::isfinite(hit.rawDamage) || hit.weaponHandle != context.weapon.handle) continue;
        if (!std::isfinite(hit.damage) || hit.damage < std::min(setting.minimumDamage, static_cast<float>(candidate.health))) {
            status = "Enemy detected and reachable, but below the configured minimum damage after hitgroup and armor.";
            continue;
        }
        if (!setting.wallPenetration && hit.penetrated) {
            status = "Enemy detected behind a surface; wall penetration is disabled.";
            continue;
        }
        if (hit.localHandle != context.local.handle || (hit.eyeOrigin - context.eye).length_sqr() > .0001f) {
            status = "Player position changed during the target scan."; return;
        }
        if (!Fresh(context, candidate) || (record && !backtrack::Current(*record))) continue;
        if(!CurrentPose(candidate.pawn,pose)){changedPose=true;continue;}
        acquisition={};acquisition.target=candidate;acquisition.list=context.local.list;
        acquisition.localPawn=context.local.pawn;acquisition.localHandle=context.local.handle;
        acquisition.weapon=context.weapon.address;acquisition.weaponHandle=context.weapon.handle;
        acquisition.epoch=hvh_input::worldEpoch.load(std::memory_order_acquire);
        acquisition.time=GetTickCount64();acquisition.source=ReadLivePoseSource(candidate.pawn);acquisition.settings=setting;
        // An unscoped acquired target does not need 128 spread samples first.
        if(setting.autoScope) {
            const auto scoped=combat_access::IsScoped(context);
            if(scoped && !*scoped)lastAttempt={};
            if(scoped && !*scoped && combat_access::ReadyForShot(context)) {
                PrepareAcquired(input,context,setting,preparingShot);
                if(combat_access::Scope(context)) {
                    preparingShot=true;lastAttempt={};
                    status="Scoping for an acquired target; preparing weapon accuracy.";
                    return;
                }
                lastAttempt={};
            }
        }
        auto aim = *desired;
        float estimatedChance = 1.f;
        if (!budget.accuracy) break;
        --budget.accuracy;
        bool correctedSpread{};
        std::optional<hvh_math::PredictedShot> predicted;
        if(compensate && budget.seedCalls) {
            const auto readSeed=[&](hvh_math::Vec3 angle,int tick)->std::optional<std::uint32_t>{
                if(!budget.seedCalls)return {};
                --budget.seedCalls;
                const auto seed=combat_access::SpreadSeed(Point(angle),tick);
                if(!seed)transientUnavailable=true;
                return seed;
            };
            const auto readSpread=[&](std::uint32_t seed)->std::optional<hvh_math::Vec2>{
                if(const auto found=budget.corrections.find(seed);found!=budget.corrections.end())return found->second;
                const auto value=combat_access::Spread(context.weapon,seed);
                const auto sample=std::isfinite(value.x)&&std::isfinite(value.y)?
                    std::optional{hvh_math::Vec2{value.x,value.y}}:std::nullopt;
                if(!sample)transientUnavailable=true;
                budget.corrections.emplace(seed,sample);return sample;
            };
            const auto corrected=hvh_math::FindSpreadPredictionIterative(aim,timing->tick,readSeed,readSpread);
            if (corrected) {
                // Reconstruct the physical angle from the command's float recoil
                // subtraction. Rounding can change an angle-dependent seed.
                const auto command=hvh_math::RemovePunch(corrected->angles,Point(context.punch));
                const auto physical=command?hvh_math::ShotAim(*command,Point(context.punch),false):std::nullopt;
                predicted=physical?hvh_math::PredictShot(*physical,timing->tick,readSeed,readSpread):std::nullopt;
                const auto predictedHit=predicted?intersection(Point(predicted->direction)*context.weapon.range):std::nullopt;
                const auto expected=hvh_math::detail::Forward(*desired);
                auto actual=predicted?hvh_math::detail::Convert(predicted->direction):hvh_math::detail::DVec3{};
                if (predicted && predicted->seed==corrected->seed && predictedHit && predictedHit->second==group &&
                    hvh_math::detail::Normalize(actual) && hvh_math::detail::Dot(actual,expected)>=1.-1e-10) {
                    aim=corrected->angles; correctedSpread=true;
                } else predicted.reset();
            }
        }
        // A failed exact seed solve must not stall a shot whose ordinary
        // spread probability already satisfies the user's threshold.
        if (!correctedSpread && setting.hitchance > 0.f) {
            const auto chance = hvh_accuracy::EvaluateGroup(aim, context.weapon.range, setting.hitchance * .01f,
                context.accuracyValid, group,
                [&](unsigned seed) -> std::optional<hvh_math::Vec2> {
                    return budget.spread.Get(seed,[&](unsigned number)->std::optional<hvh_math::Vec2>{
                        if(budget.samples>=128)return {};
                        ++budget.samples;
                        const auto spread=combat_access::Spread(context.weapon,number);
                        if(!std::isfinite(spread.x)||!std::isfinite(spread.y)){transientUnavailable=true;return {};}
                        return hvh_math::Vec2{spread.x,spread.y};
                    });
                }, [&](hvh_math::Vec3 ray) -> std::optional<int> {
                    const auto hitGeometry = intersection(Point(ray));
                    return hitGeometry ? hitGeometry->second : 0;
                });
            if (!chance || !chance->passed) {
                if(!chance){transientUnavailable=true;lastAttempt={};}
                status = chance ? "Waiting for the configured hit chance." : "Weapon accuracy or spread samples are unavailable.";
                if (chance) needsAccuracy = selected.index;
                // Weapon accuracy improves on subsequent movement commands.
                // Begin braking this clear live acquisition now instead of
                // simulating every other point before applying the same stop.
                if(chance && !best && !record && !hit.penetrated && setting.autoStop &&
                   Activated(Get()) && combat_access::ReadyForShot(context) &&
                   Fresh(context,candidate) && CurrentPose(candidate.pawn,pose)) {
                    if(const auto prepared=movement::PrepareShot(input,true,false)) {
                        preparingShot=true;
                        if(transientUnavailable)lastAttempt={};
                        std::lock_guard lock(shotMutex);
                        cameraCommand.Applied({prepared->command,prepared->sequence,prepared->localHandle,false});
                        status="Countersteering for an acquired target; waiting for weapon accuracy.";
                        return;
                    }
                }
                continue;
            }
            estimatedChance = chance->chance;
        }
        const auto unpunched = hvh_math::RemovePunch(aim, Point(context.punch));
        if (!unpunched) {
            status = "The corrected shot angle is outside the usable range."; continue;
        }
        const auto commandAngle = Point(*unpunched);
        if (!Activated(Get()) || !Fresh(context, candidate)) return;
        if (record && !backtrack::Current(*record)) continue;
        const hvh_accuracy::QualifiedShot rank{hit.damage, estimatedChance, selected.angularDistance,
            selected.distance, candidate.health, hit.penetrated, selected.index};
        if (!best || hvh_accuracy::Better(rank, best->rank))
            best = Qualified{candidate, commandAngle, targetTiming, record,pointPoses[selected.index],hit.damage,estimatedChance,
                selected.angularDistance, hit.penetrated, rank,predicted};
        // Live centers already lead the fair scan order. A clear live shot
        // that passes the user's damage and confidence settings is ready now;
        // searching multipoints/history afterward stalls its command.
        if(!record && !hit.penetrated)break;
    }
    const auto stillCurrent = [&](std::size_t index) {
        return combat_access::ReadyForShot(context) && Fresh(context, points[index]) &&
            CurrentPose(points[index].pawn,*pointPoses[index]) &&
            (!pointRecords[index] || backtrack::Current(*pointRecords[index]));
    };
    if(transientUnavailable)lastAttempt={};
    if (best) {
        const auto& candidate = best->candidate;
        if (!Activated(Get()) || !Fresh(context, candidate) || (best->record && !backtrack::Current(*best->record))) return;
        if(!CurrentPose(candidate.pawn,*best->pose)) {
            lastAttempt={};
            if(!retried && budget.traces && budget.accuracy)
                ProcessShot(input,shotApplied,shotTarget,preparingShot,&budget,true);
            return;
        }
        const bool applied = combat_access::ApplyShot(context, best->angles, *timing, best->targetTiming);
        if(!applied)lastAttempt={};
        if (applied) {
            if (setting.showSpreadPrediction && best->prediction)
                PublishPrediction(context,best->prediction->angles,best->prediction,true);
            shotTarget=candidate;
            shotApplied = combat_access::CommittedShot{context.command, context.commandNumber,
                context.local.handle, context.weapon.handle, *timing, best->angles, best->targetTiming};
            std::lock_guard shotLock(shotMutex);
            silentCommand.Applied({context.command, context.commandNumber, context.local.handle, false});
            cameraCommand.Applied({context.command, context.commandNumber, context.local.handle, false});
        }
        status = applied ? (best->record ? "Qualified silent shot applied to a recorded enemy pose." :
            best->penetrated ? "Qualified silent shot applied through a penetrable surface." : "Qualified silent shot applied to the best scanned target.") :
                           "The shot command changed before it could be applied.";
        return;
    }
    if(changedPose && !retried && budget.traces && budget.accuracy) {
        lastAttempt={};
        ProcessShot(input,shotApplied,shotTarget,preparingShot,&budget,true);
        return;
    }
    if (needsAccuracy && setting.autoStop && Activated(Get()) && stillCurrent(*needsAccuracy)) {
        if (const auto prepared = movement::PrepareShot(input,true,false)) {
            preparingShot = true;
            std::lock_guard lock(shotMutex);
            cameraCommand.Applied({prepared->command, prepared->sequence, prepared->localHandle, false});
            status = "Countersteering for an acquired target; waiting for weapon accuracy.";
        }
    }
}
bool ProtectedShot(Address input, std::optional<combat_access::CommittedShot>& shotApplied,
                   std::optional<hvh_math::Candidate>& shotTarget, bool& preparingShot) {
    __try { ProcessShot(input, shotApplied, shotTarget, preparingShot); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        combatReady = false;
        status = "HVH stopped after a shot-data fault. Movement input remains connected; reload the build.";
        return false;
    }
}
void ProcessOrientation(Address input, const Settings& setting, const combat_access::CommittedShot* shot = nullptr,
                        const hvh_math::Candidate* shotTarget=nullptr) {
    const auto context = combat_access::ReadOrientationContext(input, shot);
    if (!context.valid) { antiAimStatus = context.failure; return; }
    const hvh_antiaim::Options options{
        static_cast<hvh_antiaim::YawMode>(setting.yawMode),
        static_cast<hvh_antiaim::PitchMode>(setting.pitchMode),
        setting.yawOffset, setting.spinSpeed
    };
    std::optional<combat_access::OrientationTarget> target;
    if(options.yaw==hvh_antiaim::YawMode::Backward){
        target=combat_access::ReadOrientationTarget(context,shotTarget?shotTarget->pawn:0,
            shotTarget?shotTarget->handle:0xffffffffu);
        // No enemy: still turn around camera-relative so the head stays
        // hidden from anyone walking in front, instead of disabling AA.
    }
    const auto angles = hvh_antiaim::Angles(Point(context.angles), context.tick, options,1.f/64.f,
        target?std::optional{target->yaw}:std::nullopt);
    if (!angles) { antiAimStatus = "Anti-aim angles are unavailable."; return; }
    if (!AntiAimActivated(Get())) return;
    if (!combat_access::ApplyOrientation(context, Point(*angles),target?&*target:nullptr)) {
        antiAimStatus = "Player input changed before anti aim could be applied."; return;
    }
    // Unlike a committed shot, an orientation edit must not suppress native
    // input rebuilding: fresh mouse buttons and interactions keep their priority.
    { std::lock_guard lock(shotMutex);
      cameraCommand.Applied({context.command, context.commandNumber, context.local.handle, false}); }
    antiAimStatus = shot ? "Anti aim active during the HVH shot; shot history and camera preserved." :
        setting.yawMode == 1 ? "Spinbot active; visible camera preserved." :
        target ? "Facing away from the selected enemy; visible camera preserved." :
                 "Turned around camera-relative; head hidden from the front.";
}
void Process(Address input) {
    std::lock_guard lock(commandMutex);
    SynchronizeSession();
    const auto setting = Get();
    if (!GameplayInput()) { acquisition={};lastAttempt={};movement::Reset();ClearPrediction();return; }
    ClearPrediction();
    if(!Activated(setting)){acquisition={};lastAttempt={};}
    std::optional<combat_access::CommittedShot> shotApplied;
    std::optional<hvh_math::Candidate> shotTarget;
    bool preparingShot{};
    if (combatReady.load(std::memory_order_acquire) && Activated(setting) &&
        !ProtectedShot(input, shotApplied, shotTarget, preparingShot)) { movement::Reset(); return; }
    if (const auto changed = movement::Process(input, shotApplied.has_value() || preparingShot)) {
        std::lock_guard lock(shotMutex);
        cameraCommand.Applied({changed->command, changed->sequence, changed->localHandle, false});
    }
    if (setting.showSpreadPrediction && combatReady.load(std::memory_order_acquire)) {
        bool published{};
        { std::lock_guard lock(predictionMutex); published=lastPrediction.has_value(); }
        if (!published) PredictCurrentShot(input,shotApplied?&*shotApplied:nullptr);
    }
    if (AntiAimActivated(setting)) ProcessOrientation(input, setting, shotApplied ? &*shotApplied : nullptr,
        shotTarget?&*shotTarget:nullptr);
    else antiAimStatus = setting.antiAim ? "Waiting for the HVH activation key." : "Anti aim is disabled.";
    movement::FinishCommand(input);
}
__declspec(noinline) void ProtectedProcess(Address input) {
    __try { Process(input); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        running = false;
        status = "Combat stopped after a game data fault. Reload the build.";
        antiAimStatus = status.load();
        movement::SetPipelineStatus(status.load());
    }
}
void __fastcall CreateMove(Address input, int slot, bool active) {
    struct Callback { Callback() { ++callbacks; } ~Callback() { --callbacks; } } callback;
    if (inMove) { originalMove(input, slot, active); return; }
    struct Depth { Depth() { inMove = true; } ~Depth() { inMove = false; } } depth;
    if (slot == 0 && running.load(std::memory_order_acquire)) {
        { std::lock_guard lock(commandMutex); SynchronizeSession(); }
        if (const auto command = combat_access::ReadInputCommand(input)) {
            std::lock_guard lock(shotMutex);
            if (!command->weaponSelectionPending &&
                silentCommand.Preserve({command->command, command->sequence, command->localHandle, command->subtickOverwrite}))
                return;
            silentCommand.Clear();
        }
    }
    originalMove(input, slot, active);
    // The current native path can emit a valid command even with active=false.
    // ReadContext validates the actual sequence and protobuf after the original.
    if (running.load(std::memory_order_acquire) && slot == 0) ProtectedProcess(input);
}
void __fastcall HandleViewAngles(Address input, int slot) {
    struct Callback { Callback() { ++callbacks; } ~Callback() { --callbacks; } } callback;
    Vec3 camera{};
    bool preserve{};
    if (running.load(std::memory_order_acquire) && slot == 0 &&
        commandEpoch.load(std::memory_order_acquire) == hvh_input::worldEpoch.load(std::memory_order_acquire)) {
        // Restore only our committed command. This also covers key release
        // between committing the shot and the game's camera-consumption pass.
        if (const auto command = combat_access::ReadInputCommand(input)) {
            std::lock_guard lock(shotMutex);
            preserve = cameraCommand.Matches({command->command, command->sequence, command->localHandle, command->subtickOverwrite});
        }
        if (preserve && !combat_access::ReadViewAngles(input, camera)) {
            preserve = false;
            running = false;
            status = "HVH stopped because the visible camera could not be read.";
            movement::SetPipelineStatus(status.load());
        }
    }
    originalView(input, slot);
    if (preserve && !combat_access::WriteViewAngles(input, camera)) {
        running = false;
        status = "HVH stopped because the visible camera could not be restored.";
        movement::SetPipelineStatus(status.load());
    }
}
std::string KeyName(int key) {
    switch (key) {
    case 0: return "Unbound";
    case VK_LBUTTON: return "Mouse 1";
    case VK_RBUTTON: return "Mouse 2";
    case VK_MBUTTON: return "Mouse 3";
    case VK_XBUTTON1: return "Mouse 4";
    case VK_XBUTTON2: return "Mouse 5";
    }
    char name[64]{};
    const auto scan = MapVirtualKeyA(key, MAPVK_VK_TO_VSC_EX);
    const LONG parameter = (scan & 0xff) << 16 | ((scan & 0xff00) ? 1 << 24 : 0);
    return GetKeyNameTextA(parameter, name, sizeof(name)) ? name : "Key " + std::to_string(key);
}
} // namespace

bool Install() {
    running = false;
    combatReady = false;
    const auto fail = [](const char* reason) {
        status = reason; movement::SetPipelineStatus(reason); return false;
    };
    { std::lock_guard lock(commandMutex); commandEpoch = 0; SynchronizeSession(); }
    if (!combat_access::InitializeInput()) return fail(combat_access::BindingStatus());
    const bool shooting = combat_access::Initialize() && autowall_probe::Initialize();
    const auto shootingFailure = combat_access::BindingStatus();
    // Movement and base-angle edits need the command/camera bindings only.
    // Optional shot/penetration bindings must not prevent the input hook.
    status = shooting ? "Ready. Close the menu and use your HVH activation mode." :
        std::strcmp(shootingFailure, "Combat bindings connected.") == 0 ?
            "The penetration bindings are unavailable; movement input is connected." : shootingFailure;
    combatReady = shooting;
    const auto target = reinterpret_cast<void*>(combat_access::CreateMoveTarget());
    const auto viewTarget = reinterpret_cast<void*>(combat_access::HandleViewAnglesTarget());
    if (hookCreated && hookTarget != target) return fail("The input hook changed; reload the build.");
    if (viewHookCreated && viewHookTarget != viewTarget) return fail("The camera hook changed; reload the build.");
    if (!hookCreated) {
        const auto result = MH_CreateHook(target, reinterpret_cast<void*>(CreateMove), reinterpret_cast<void**>(&originalMove));
        if (result != MH_OK) return fail("The HVH input hook could not be created.");
        hookCreated = true; hookTarget = target;
    }
    if (!viewHookCreated) {
        const auto result = MH_CreateHook(viewTarget, reinterpret_cast<void*>(HandleViewAngles), reinterpret_cast<void**>(&originalView));
        if (result != MH_OK) return fail("The silent camera hook could not be created.");
        viewHookCreated = true; viewHookTarget = viewTarget;
    }
    { std::lock_guard lock(shotMutex); silentCommand.Clear(); cameraCommand.Clear(); }
    const auto viewResult = MH_EnableHook(viewHookTarget);
    if (viewResult != MH_OK && viewResult != MH_ERROR_ENABLED) {
        return fail("The silent camera hook could not be enabled.");
    }
    running = true;
    const auto result = MH_EnableHook(hookTarget);
    if (result != MH_OK && result != MH_ERROR_ENABLED) {
        running = false; MH_DisableHook(viewHookTarget);
        return fail("The HVH input hook could not be enabled.");
    }
    movement::SetPipelineStatus(nullptr);
    return true;
}
const char* Status() { return status.load(); }
std::optional<SpreadPrediction> Prediction() {
    if (!running.load(std::memory_order_acquire) || !GameplayInput() ||
        !Get().showSpreadPrediction) return {};
    std::lock_guard lock(predictionMutex);
    if (!lastPrediction || GetTickCount64()-lastPrediction->time>150 ||
        lastPrediction->epoch!=hvh_input::worldEpoch.load(std::memory_order_acquire)) return {};
    return lastPrediction;
}
bool Stop() {
    running = false;
    combatReady = false;
    ClearPrediction();
    movement::SetPipelineStatus("Input hooks are stopped.");
    bool disabled = true;
    if (hookCreated) {
        const auto result = MH_DisableHook(hookTarget);
        disabled = result == MH_OK || result == MH_ERROR_DISABLED;
    }
    if (viewHookCreated) {
        const auto result = MH_DisableHook(viewHookTarget);
        disabled = (result == MH_OK || result == MH_ERROR_DISABLED) && disabled;
    }
    if (!disabled) return false;
    const auto deadline = GetTickCount64() + 2000;
    while (callbacks.load(std::memory_order_acquire) && GetTickCount64() < deadline) Sleep(1);
    if (callbacks.load(std::memory_order_acquire)) return false;
    // Keep the disabled trampoline alive for callbacks already entering it.
    std::lock_guard lock(commandMutex);
    combat_access::ResetTargets();
    acquisition={};lastAttempt={};
    movement::Reset();
    { std::lock_guard lock(shotMutex); silentCommand.Clear(); cameraCommand.Clear(); }
    const bool reset = autowall_probe::Reset();
    if (reset) game_access::verifiedGameplayClient = 0;
    return reset;
}
void SetInputState(bool open, float width, float height) {
    menuOpen.store(open, std::memory_order_release);
    viewportWidth = width; viewportHeight = height;
    movement::SetInputState(open);
    if (!open) capturing = false;
}
void DrawMenu() {
    auto value = Get();
    bool changed = ImGui::Checkbox("Enable ragebot", &value.enabled);
    constexpr const char* activationModes[]{"Hold key", "Always"};
    changed |= ImGui::Combo("Activation", &value.activationMode, activationModes, 2);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Ranks qualified shots by lethal damage and estimated hit probability. Paused while the menu is open or CS2 is unfocused.");
    if (value.activationMode == 1) capturing = false;
    if (value.activationMode == 0) {
    if (gui_capture::active) {
        capturing = false;
        changed |= gui_capture::active->Keybind("Activation key", &value.key);
    } else {
    const auto label = capturing ? std::string("Press a key...##hvh_key") : KeyName(value.key) + " (hold)##hvh_key";
    if (ImGui::Button(label.c_str(), {180, 0})) {
        capturing = true;
        for (int key = 0; key < 256; ++key) previousKeys[key] = (GetAsyncKeyState(key) & 0x8000) != 0;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Choose a keyboard or mouse button. Escape clears the bind.");
    ImGui::SameLine(); ImGui::TextUnformatted("Keybind");
    }
    }
    if (capturing && !gui_capture::active) {
        ImGui::TextDisabled("Press a key or mouse button. Esc to clear.");
        for (int key = 1; key < 256; ++key) {
            const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
            if (down && !previousKeys[key] && key != VK_INSERT && key != VK_END) {
                value.key = key == VK_ESCAPE ? 0 : key; changed = true; capturing = false; break;
            }
            previousKeys[key] = down;
        }
    }
    if (ImGui::CollapsingHeader("Targeting", ImGuiTreeNodeFlags_DefaultOpen)) {
        constexpr const char* hitboxes[]{"Head", "Head and body", "Body", "All hitboxes"};
        changed |= ImGui::Combo("Hitboxes", &value.hitboxes, hitboxes, 4);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Head selects head points only, with no body fallback.");
        changed |= ImGui::SliderFloat("Field of view", &value.fieldOfView, 1.f, 180.f, "%.0f deg");
        changed |= ImGui::SliderFloat("Minimum damage", &value.minimumDamage, 1.f, 100.f, "%.0f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Damage after the first intersected hitgroup and armor are applied.");
        changed |= ImGui::Checkbox("Multipoints", &value.multipoints);
        if (value.multipoints) changed |= ImGui::SliderFloat("Point scale", &value.pointScale, .1f, .9f, "%.2f");
        changed |= ImGui::Checkbox("Wall penetration", &value.wallPenetration);
    }
    if (ImGui::CollapsingHeader("Firing", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= ImGui::Checkbox("Automatic fire", &value.autoFire);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("When disabled, hold primary attack along with your activation key, if set.");
        changed |= ImGui::Checkbox("Recoil correction", &value.recoilCorrection);
        changed |= ImGui::SliderFloat("Estimated hit chance", &value.hitchance, 0.f, 100.f, "%.0f%%");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Native spread samples against target hitboxes. Private server randomness remains unpredictable; surfaces along spread rays are not sampled.");
        changed |= ImGui::Checkbox("Auto stop", &value.autoStop);
        changed |= ImGui::Checkbox("Auto scope", &value.autoScope);
        changed |= ImGui::Checkbox("No spread", &value.noSpread);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Requires predictable server spread. Servers using a private random seed retain spread.");
        changed |= ImGui::Checkbox("Spread prediction marker", &value.showSpreadPrediction);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Known seed: predicted first contact. Private or unavailable seed: amber spread envelope. Also works with ragebot disabled; firing and correction settings remain separate.");
    }
    if (changed) Save(value);
}
void DrawAntiAimMenu() {
    auto value = Get();
    bool changed = ImGui::Checkbox("Anti Aim", &value.antiAim);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Changes body angles while preserving your camera and movement direction. HVH shots retain their aim; manual fire and interactions take priority.");
    if (ImGui::CollapsingHeader("Activation", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= ImGui::Checkbox("Use HVH activation", &value.antiAimUseKey);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Follows the mode and keybind set in HVH. When disabled, anti aim is always active during play, with the menu closed.");
    }
    if (ImGui::CollapsingHeader("Angles", ImGuiTreeNodeFlags_DefaultOpen)) {
        constexpr const char* yawModes[]{"Away from target", "Spin"};
        constexpr const char* pitchModes[]{"Follow camera", "Down", "Up"};
        changed |= ImGui::Combo("Yaw mode", &value.yawMode, yawModes, 2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Away from target tracks the HVH shot target, or the enemy closest to your crosshair. Zero yaw offset faces directly away.");
        changed |= ImGui::Combo("Pitch", &value.pitchMode, pitchModes, 3);
        changed |= ImGui::SliderFloat("Yaw offset", &value.yawOffset, -180.f, 180.f, "%.0f deg");
        if (value.yawMode == 1)
            changed |= ImGui::SliderFloat("Spin speed", &value.spinSpeed, 30.f, 1440.f, "%.0f deg/s");
    }
    if (changed) Save(value);
}
} // namespace hvh
