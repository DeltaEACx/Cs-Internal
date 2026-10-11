#include "backtrack.h"
#include "detail/backtrack_history.hpp"
#include "detail/hvh_input.hpp"
#include "features/visuals/backtrack_chams.h"
#include <unordered_set>

namespace backtrack {
namespace {
using Address = game_access::Address;
using combat_access::Field;
std::mutex mutex;
Settings settings;
std::unordered_map<Address, detail::History> histories;
std::atomic<const char*> status{"Waiting for enemy snapshots."};
Address sessionList{};
std::uint32_t sessionLocal{0xffffffffu};
std::uint64_t sessionEpoch{};
std::atomic<std::uint64_t> generation{1};
float lastTime{};
std::atomic<float> serverWindow{};
std::atomic<ULONGLONG> nextLimitsCheck{};
struct Clock { float now{}, window{}; };
Clock ReadClock(const Settings& config) {
    const auto globals = memory::safe_read<Address>(addresses::globals::global_vars).value_or(0);
    const auto now = globals ? memory::safe_read<float>(globals + 0x30) : std::nullopt;
    const auto limit = serverWindow.load();
    if (!now || !std::isfinite(*now) || *now <= 0 || !std::isfinite(limit) || limit <= 0) return {};
    float window = std::min(config.windowMs * .001f, limit);
    return {*now, window};
}
void RefreshLimits() {
    const auto now = GetTickCount64();
    if (now < nextLimitsCheck.load()) return;
    auto limit = combat_access::Convar("sv_maxunlag", 0.f);
    const auto playerLimit = combat_access::Convar("sv_maxunlag_player", 0.f);
    if (std::isfinite(playerLimit) && playerLimit > 0) limit = std::min(limit, playerLimit);
    serverWindow = std::isfinite(limit) && limit > 0 ? limit : 0;
    nextLimitsCheck = now + 250;
}
bool Identity(const Record& record) {
    if (record.epoch != hvh_input::worldEpoch.load(std::memory_order_acquire) || record.generation != generation.load()) return false;
    const auto local = game_access::ReadLocalPlayer(game_access::GameplayClient());
    if (local.state != game_access::PlayerState::Alive || local.list != record.list ||
        local.handle != record.localHandle || local.pawn == record.pawn ||
        local.team == record.team || game_access::Resolve(local.list, record.handle) != record.pawn) return false;
    const auto scene = Field<Address>(record.pawn, "C_BaseEntity", "m_pGameSceneNode").value_or(0);
    const auto modelState = systems::schemas::lookup("CSkeletonInstance", "m_modelState"_hash);
    const auto binding = modelState ? Field<Address>(scene + modelState, "CModelState", "m_hModel").value_or(0) : 0;
    const auto sim = Field<float>(record.pawn, "C_BaseEntity", "m_flSimulationTime");
    const auto origin = Field<math::vector3>(scene, "CGameSceneNode", "m_vecAbsOrigin");
    return scene == record.scene && binding == record.binding && binding &&
        memory::safe_read<Address>(binding).value_or(0) == record.model && sim && *sim >= record.simulationTime &&
        origin && combat_access::Finite(*origin) && (*origin - record.origin).length_sqr() <= 64.f * 64.f &&
        Field<int>(record.pawn, "C_BaseEntity", "m_iHealth").value_or(0) > 0 &&
        Field<std::uint8_t>(record.pawn, "C_BaseEntity", "m_lifeState").value_or(1) == 0 &&
        Field<std::uint8_t>(record.pawn, "C_BaseEntity", "m_iTeamNum").value_or(0) == record.team &&
        !Field<bool>(scene, "CGameSceneNode", "m_bDormant").value_or(true) &&
        !Field<bool>(record.pawn, "C_CSPlayerPawn", "m_bGunGameImmunity").value_or(true);
}
struct CaptureSource {
    std::uint64_t epoch{}, generation{};
    Address scene{}, state{}, binding{}, model{}, cache{};
    float simulationTime{};
    math::vector3 origin{};
    int count{};
    std::uint16_t cacheCount{};
};
std::optional<CaptureSource> ReadCaptureSource(const game_access::LocalPlayer& local,
                                              const combat_access::Target& target, Clock clock) {
    CaptureSource source;
    source.epoch = hvh_input::worldEpoch.load(std::memory_order_acquire);
    source.generation = generation.load();
    source.scene = Field<Address>(target.pawn, "C_BaseEntity", "m_pGameSceneNode").value_or(0);
    const auto sim = Field<float>(target.pawn, "C_BaseEntity", "m_flSimulationTime");
    const auto origin = Field<math::vector3>(source.scene, "CGameSceneNode", "m_vecAbsOrigin");
    const auto stateOffset = systems::schemas::lookup("CSkeletonInstance", "m_modelState"_hash);
    if (!sim || !detail::ValidTime(*sim, clock.now, clock.window) || !origin ||
        !combat_access::Finite(*origin) || !source.scene || !stateOffset) return {};
    source.simulationTime = *sim; source.origin = *origin; source.state = source.scene + stateOffset;
    source.binding = Field<Address>(source.state, "CModelState", "m_hModel").value_or(0);
    source.model = memory::safe_read<Address>(source.binding).value_or(0);
    if (!source.model) return {};
    source.count = memory::safe_read<int>(source.model + 0x178).value_or(0);
    source.cacheCount = memory::safe_read<std::uint16_t>(source.state + 0x94).value_or(0);
    source.cache = memory::safe_read<Address>(source.state + 0x80).value_or(0);
    // Do not truncate a model's pose: the renderer needs every model bone.
    if (source.count <= 0 || source.count > detail::maxBones || source.cacheCount < source.count ||
        source.cacheCount > detail::maxBones || !source.cache) return {};
    Record stamp;
    stamp.epoch=source.epoch;stamp.generation=source.generation;stamp.list=local.list;stamp.localHandle=local.handle;
    stamp.pawn=target.pawn;stamp.handle=target.handle;stamp.team=target.team;stamp.scene=source.scene;
    stamp.binding=source.binding;stamp.model=source.model;stamp.simulationTime=source.simulationTime;stamp.origin=source.origin;
    if (!Identity(stamp)) return {};
    return source;
}
bool Duplicate(const Record& record, const game_access::LocalPlayer& local,
               const combat_access::Target& target, const CaptureSource& source) {
    return record.epoch==source.epoch && record.generation==source.generation && record.list==local.list &&
        record.localHandle==local.handle && record.pawn==target.pawn && record.handle==target.handle &&
        record.team==target.team && record.scene==source.scene && record.binding==source.binding &&
        record.model==source.model && record.bones.size()==static_cast<std::size_t>(source.count) &&
        record.simulationTime==source.simulationTime && (record.origin-source.origin).length_sqr()<=64.f*64.f;
}
Snapshot Capture(const game_access::LocalPlayer& local, const combat_access::Target& target,
                 Clock clock, const CaptureSource& source) {
    const auto transformOffset = systems::schemas::lookup("CGameSceneNode", "m_nodeToWorld"_hash);
    if (!transformOffset) return {};
    auto record = std::make_shared<Record>();
    record->epoch=source.epoch;record->generation=source.generation;record->list=local.list;record->localHandle=local.handle;
    record->pawn=target.pawn;record->handle=target.handle;record->team=target.team;record->target=target;
    record->scene=source.scene;record->binding=source.binding;record->model=source.model;
    record->simulationTime=source.simulationTime;record->origin=source.origin;
    record->target.hitboxes=combat_access::ReadHitboxes(source.scene);
    record->bones.resize(source.count);
    if (!memory::copy(record->bones.data(), source.cache, source.count * sizeof(systems::bones::data)) ||
        !memory::copy(record->transform.data(), record->scene + transformOffset, sizeof(record->transform))) return {};
    for (const auto& bone : record->bones) {
        const auto& q = bone.rotation;
        const auto norm = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
        if (!combat_access::Finite(bone.position) || !std::isfinite(bone.scale) || bone.scale <= 0 || bone.scale > 100 ||
            !std::isfinite(norm) || std::abs(norm - 1.f) > .1f) return {};
    }
    if (!std::ranges::all_of(record->transform, [](float x) { return std::isfinite(x); })) return {};
    for (auto& box : record->target.hitboxes.entries) {
        if (box.bone < 0 || static_cast<std::size_t>(box.bone) >= record->bones.size()) continue;
        const auto scale = record->bones[box.bone].scale;
        box.mins = box.mins * scale; box.maxs = box.maxs * scale; box.radius *= scale;
    }
    record->points = hvh_points::Collect(record->target, [&](unsigned index) {
        return index < record->bones.size() ? record->bones[index] : systems::bones::data{};
    }, [&] { return systems::g_bones.get_skeleton(target.pawn, true); });
    if (Field<float>(target.pawn, "C_BaseEntity", "m_flSimulationTime").value_or(0) != source.simulationTime ||
        memory::safe_read<Address>(source.state + 0x80).value_or(0) != source.cache ||
        memory::safe_read<int>(source.model + 0x178).value_or(0) != source.count ||
        memory::safe_read<std::uint16_t>(source.state + 0x94).value_or(0) != source.cacheCount || !Identity(*record)) return {};
    return record;
}
}
Settings Get() { std::lock_guard lock(mutex); return settings; }
void Set(Settings value) {
    value.windowMs = std::isfinite(value.windowMs) ? std::clamp(value.windowMs, 16.f, 200.f) : 100.f;
    std::lock_guard lock(mutex); settings = value;
    if (!value.enabled) { histories.clear(); ++generation; }
}
void Reset() {
    std::lock_guard lock(mutex); histories.clear(); sessionList = 0; sessionLocal = 0xffffffffu;
    sessionEpoch = 0; lastTime = 0; status = "Waiting for enemy snapshots.";
    ++generation;
    serverWindow = 0; nextLimitsCheck = 0;
}
void Frame(bool inWorld) {
    const auto config = Get();
    if (!inWorld || !config.enabled) { Reset(); return; }
    const auto local = game_access::ReadLocalPlayer(game_access::GameplayClient());
    RefreshLimits();
    const auto clock = ReadClock(config);
    if (local.state != game_access::PlayerState::Alive || clock.window <= 0) {
        Reset(); status = "Waiting for a living local player and server history window."; return;
    }
    {
        std::lock_guard lock(mutex);
        const auto epoch = hvh_input::worldEpoch.load(std::memory_order_acquire);
        if (sessionList != local.list || sessionLocal != local.handle || sessionEpoch != epoch || clock.now < lastTime) {
            histories.clear(); ++generation;
        }
        sessionList = local.list; sessionLocal = local.handle; sessionEpoch = epoch; lastTime = clock.now;
    }
    const auto players = combat_access::ReadHistoryTargets(local, false);
    std::vector<Snapshot> snapshots;
    for (const auto& player : players) {
        if (snapshots.size() >= detail::maxPlayers) break;
        const auto source=ReadCaptureSource(local,player,clock);
        if (!source) continue;
        Snapshot previous;
        {
            std::lock_guard lock(mutex);
            const auto found=histories.find(player.pawn);
            if (found!=histories.end()) {
                found->second.Prune(clock.now,clock.window);
                if (!found->second.records.empty()) previous=found->second.records.front();
            }
        }
        // Render frames often repeat a network simulation state. Keep its
        // valid history active without reading hitboxes or copying the pose.
        if (previous && Duplicate(*previous,local,player,*source)) snapshots.push_back(std::move(previous));
        else if (auto record=Capture(local,player,clock,*source)) snapshots.push_back(std::move(record));
    }
    std::lock_guard lock(mutex);
    const auto epoch = hvh_input::worldEpoch.load(std::memory_order_acquire);
    if (!settings.enabled) return;
    if (sessionList != local.list || sessionLocal != local.handle || sessionEpoch != epoch) return;
    std::unordered_set<Address> active;
    for (auto& record : snapshots) {
        if (record->epoch != epoch || record->generation != generation.load()) continue;
        active.insert(record->pawn); histories[record->pawn].Add(std::move(record), clock.now, clock.window);
    }
    std::erase_if(histories, [&](const auto& item) { return !active.contains(item.first); });
    status = histories.empty() ? "No valid enemy poses are available yet." : "Recording enemy poses; HVH can use valid history.";
}
bool Current(const Record& record) {
    const auto config = Get(); const auto clock = ReadClock(config);
    return config.enabled && detail::ValidTime(record.simulationTime, clock.now, clock.window) && Identity(record);
}
std::vector<Snapshot> Records(Address pawn, std::uint32_t handle) {
    std::vector<Snapshot> result;
    { std::lock_guard lock(mutex); const auto found = histories.find(pawn);
      if (settings.enabled && found != histories.end()) result.assign(found->second.records.begin(), found->second.records.end()); }
    std::erase_if(result, [&](const Snapshot& record) { return record->handle != handle || !Current(*record); });
    return result;
}
std::vector<Snapshot> Oldest() {
    std::vector<Snapshot> result;
    { std::lock_guard lock(mutex);
      if (settings.enabled) for (const auto& [pawn, history] : histories)
          if (!history.records.empty()) result.push_back(history.records.back()); }
    std::erase_if(result, [](const Snapshot& record) { return !Current(*record); });
    return result;
}
std::optional<combat_bindings::BacktrackTiming> Timing(const Record& record) {
    if (!Current(record)) return {};
    const auto ticks = std::floor(double(record.simulationTime) * 64.);
    if (!std::isfinite(ticks) || ticks <= 0 || ticks >= INT_MAX - 1) return {};
    return combat_bindings::BacktrackTiming{{static_cast<int>(ticks) + 1, 0.f}, static_cast<int>(record.handle & 0x7fff)};
}
std::optional<math::vector3> Intersection(const Record& record, const math::vector3& eye, const math::vector3& point) {
    if (!Current(record) || !combat_access::Finite(eye) || !combat_access::Finite(point)) return {};
    auto delta = point - eye;
    if (delta.length_sqr() < .001f) return {};
    // Extend slightly beyond the selected center, then intersect the actual
    // recorded hitboxes. Named-bone fallback alone cannot prove a historical hit.
    delta = delta * 1.01f;
    float nearest = 1.f; bool hit{};
    for (const auto& box : record.target.hitboxes) {
        if (box.bone < 0 || box.bone >= record.bones.size()) continue;
        float fraction{};
        if (combat_access::RayHitbox(eye, delta, box, record.bones[box.bone], fraction) &&
            std::isfinite(fraction) && fraction >= 0 && fraction <= nearest) { nearest = fraction; hit = true; }
    }
    return hit ? std::optional{eye + delta * nearest} : std::nullopt;
}
void DrawMenu() {
    auto config = Get();
    bool changed = ImGui::Checkbox("Enable backtrack", &config.enabled);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Adds recent enemy poses to HVH targeting. Uses your HVH activation, hitbox and damage settings.");
    changed |= ImGui::SliderFloat("History window", &config.windowMs, 16.f, 200.f, "%.0f ms");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("History is limited by the server's lag-compensation window.");
    if (changed) Set(config);
    if (ImGui::CollapsingHeader("Backtrack chams", ImGuiTreeNodeFlags_DefaultOpen)) visuals::backtrack_chams::DrawMenu();
}
}
