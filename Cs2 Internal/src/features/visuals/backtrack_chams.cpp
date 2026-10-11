#include "backtrack_chams.h"
#include "chams_materials.h"
#include "features/combat/backtrack.h"
#include "game/sdk/BacktrackBindings.h"
#include "game/sdk/ChamsBindings.h"
#include <unordered_set>

namespace visuals::backtrack_chams {
namespace {
using Address = std::uintptr_t;
using combat_access::Field;
struct Api { Address client{}, mesh{}, scene{}, create{}, destroy{}, group{}; } api;
struct Ghost { Address object{}; backtrack::Snapshot record; };
std::recursive_mutex mutex;
std::unordered_map<Address, Ghost> ghosts;
std::atomic<bool> ready{};
std::atomic<const char*> status{"Backtrack model renderer has not initialized."};
chams::Configuration config = [] {
    chams::Configuration value; value.enabled = true;
    value.visible = {true, chams::Material::Flat, {180, 130, 255, 105}};
    value.hidden = {false, chams::Material::Flat, {180, 130, 255, 65}};
    return value;
}();
bool Match(Address base, const skin_bindings::Function& function) {
    std::array<std::uint8_t,24> bytes{};
    return base && memory::copy(bytes.data(), base + function.rva, bytes.size()) && bytes == function.entry;
}
__declspec(noinline) Address Interface(const char* module, const char* name) {
    __try {
        const auto factory = GetProcAddress(GetModuleHandleA(module), "CreateInterface");
        return factory ? reinterpret_cast<Address(__cdecl*)(const char*, int*)>(factory)(name, nullptr) : 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
__declspec(noinline) Address Create(const backtrack::Record& record) {
    __try {
        const auto groupId = Field<int>(record.scene, "CGameSceneNode", "m_worldGroupId");
        const auto system = memory::safe_read<Address>(api.client + backtrack_bindings::worldStorage).value_or(0);
        if (!groupId || !system) return 0;
        const auto group = memory::call<Address>(api.group, system, *groupId);
        if (!group) return 0;
        const auto flags = *groupId ? 0x2000000000ull : 0x2000000008ull;
        auto transform = record.transform;
        return memory::call<Address>(api.create, api.mesh, record.binding, transform.data(),
            "AnimatableSceneObjectDesc", flags, 0x4100000001ull, group);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
__declspec(noinline) void Destroy(Address scene) {
    __try {
        if (scene && api.destroy && api.scene) {
            const auto flags = memory::safe_read<std::uint64_t>(scene + 0x80);
            if (flags && !(*flags & 0x4000000000000000ull)) memory::call<void>(api.destroy, api.scene, scene);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}
bool Pose(Ghost& ghost, const backtrack::Record& record) {
    const auto count = memory::safe_read<int>(ghost.object + chams_bindings::kRenderBoneCount).value_or(0);
    const auto bones = memory::safe_read<Address>(ghost.object + chams_bindings::kRenderBones).value_or(0);
    if (!bones || count <= 0 || count > record.bones.size()) return false;
    const auto matrix = [](const systems::bones::data& b) -> std::array<float,12> {
        const auto& q = b.rotation; const float s = b.scale;
        return {
            s*(1-2*(q.y*q.y+q.z*q.z)), s*2*(q.x*q.y-q.w*q.z), s*2*(q.x*q.z+q.w*q.y), b.position.x,
            s*2*(q.x*q.y+q.w*q.z), s*(1-2*(q.x*q.x+q.z*q.z)), s*2*(q.y*q.z-q.w*q.x), b.position.y,
            s*2*(q.x*q.z-q.w*q.y), s*2*(q.y*q.z+q.w*q.x), s*(1-2*(q.x*q.x+q.y*q.y)), b.position.z};
    };
    std::vector<std::array<float,12>> matrices(count);
    for (int i = 0; i < count; ++i) matrices[i] = matrix(record.bones[i]);
    systems::bones::data transform{};
    static_assert(sizeof(transform) == sizeof(record.transform));
    std::memcpy(&transform, record.transform.data(), sizeof(transform));
    const auto world = matrix(transform);
    return memory::store(bones, matrices.data(), matrices.size()*sizeof(matrices[0])) &&
        memory::store(ghost.object + chams_bindings::kSceneTransform, world.data(), sizeof(world)) &&
        memory::safe_write(ghost.object + chams_bindings::kSceneOwner, std::uint32_t{0xffffffffu}) &&
        memory::safe_write(ghost.object + 0x110, record.scene);
}
}
bool Initialize() {
    std::lock_guard lock(mutex);
    ready = false;
    status = "Backtrack chams bindings are unavailable for this game build.";
    if (!game::module_matches("client.dll", skin_bindings::kClientSha256) ||
        !game::module_matches("meshsystem.dll", backtrack_bindings::meshSha256) ||
        !game::module_matches("scenesystem.dll", chams_bindings::kSceneSha256)) return false;
    const auto meshBase = reinterpret_cast<Address>(GetModuleHandleA("meshsystem.dll"));
    const auto sceneBase = reinterpret_cast<Address>(GetModuleHandleA("scenesystem.dll"));
    api.client = reinterpret_cast<Address>(GetModuleHandleA("client.dll"));
    if (!Match(api.client, backtrack_bindings::worldGroup) || !Match(meshBase, backtrack_bindings::createScene) ||
        !Match(sceneBase, backtrack_bindings::deleteScene)) return false;
    api.mesh = Interface("meshsystem.dll", "MeshSystem001");
    api.scene = Interface("scenesystem.dll", "SceneSystem_002");
    api.create = memory::safe_read<Address>(memory::safe_read<Address>(api.mesh).value_or(0) + 20*8).value_or(0);
    api.destroy = memory::safe_read<Address>(memory::safe_read<Address>(api.scene).value_or(0) + 16*8).value_or(0);
    if (!api.mesh || !api.scene || api.create != meshBase + backtrack_bindings::createScene.rva ||
        api.destroy != sceneBase + backtrack_bindings::deleteScene.rva) return false;
    api.group = api.client + backtrack_bindings::worldGroup.rva;
    ready = true; status = "Ready; move an enemy to display its oldest valid pose."; return true;
}
void Reset() {
    std::lock_guard lock(mutex);
    for (const auto& [pawn, ghost] : ghosts) Destroy(ghost.object);
    ghosts.clear();
}
void Shutdown() { std::lock_guard lock(mutex); Reset(); ready = false; }
void Frame(bool inWorld) {
    std::lock_guard lock(mutex);
    if (!ready || !inWorld || !config.enabled || !backtrack::Get().enabled) { Reset(); return; }
    if (config.visible.enabled) chams::materials::Ensure(config.visible.material, false);
    if (config.hidden.enabled) chams::materials::Ensure(config.hidden.material, true);
    std::unordered_set<Address> active;
    const auto records = backtrack::Oldest();
    for (const auto& record : records) {
        const auto currentOrigin = Field<math::vector3>(record->scene, "CGameSceneNode", "m_vecAbsOrigin");
        if (!currentOrigin || (*currentOrigin - record->origin).length_sqr() < .25f*.25f) continue;
        auto& ghost = ghosts[record->pawn];
        if (ghost.record && (ghost.record->model != record->model || ghost.record->binding != record->binding ||
            ghost.record->scene != record->scene || ghost.record->handle != record->handle ||
            ghost.record->epoch != record->epoch || ghost.record->localHandle != record->localHandle)) {
            Destroy(ghost.object); ghost = {};
        }
        if (!ghost.object) ghost.object = Create(*record);
        if (!ghost.object || !backtrack::Current(*record) || !Pose(ghost, *record)) {
            Destroy(ghost.object); ghosts.erase(record->pawn); status = "Waiting for native ghost-model geometry."; continue;
        }
        ghost.record = record; active.insert(record->pawn);
    }
    std::erase_if(ghosts, [&](const auto& item) {
        if (active.contains(item.first)) return false;
        Destroy(item.second.object); return true;
    });
    if (!ghosts.empty()) status = "Displaying the oldest valid enemy poses.";
    else if (records.empty()) status = "Waiting for valid backtrack history.";
    else status = "Ready; moving enemies leave a recorded model behind.";
}
bool Generate(Address self, Address scene, Address view, Address output, chams::GenerateFn original) {
    std::lock_guard lock(mutex);
    for (const auto& [pawn, ghost] : ghosts) {
        if (ghost.object != scene) continue;
        // Always consume our scene even when disabled/expired; a ghost must
        // never fall through as an uncolored duplicate of the real player.
        if (ready && config.enabled && ghost.record && backtrack::Current(*ghost.record))
            chams::Apply(config, self, scene, view, output, original);
        return true;
    }
    return false;
}
void DrawMenu() {
    std::lock_guard lock(mutex);
    ImGui::Checkbox("Enable history chams", &config.enabled);
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Shows the oldest valid recorded pose using the enemy's model.");
    ImGui::Checkbox("Show through walls", &config.hidden.enabled);
    int material = static_cast<int>(config.visible.material);
    const char* names[]{"Liquid", "Metallic", "Matte", "Flat", "Bloom", "Outlines", "Glow", "Electric", "Distortion", "Hologram", "Pearl"};
    if (ImGui::Combo("History material", &material, names, 11))
        config.visible.material = config.hidden.material = static_cast<chams::Material>(material);
    float rgba[]{config.visible.tint.r/255.f, config.visible.tint.g/255.f, config.visible.tint.b/255.f, config.visible.tint.a/255.f};
    if (ImGui::ColorEdit4("History tint", rgba)) {
        const auto byte = [](float value) { return static_cast<std::uint8_t>(std::clamp(value, 0.f, 1.f)*255); };
        config.visible.tint = config.hidden.tint = {byte(rgba[0]),byte(rgba[1]),byte(rgba[2]),byte(rgba[3])};
    }
}
}
