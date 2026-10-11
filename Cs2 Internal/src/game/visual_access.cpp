#include "game/combat_access.h"

// Observational game reads for grenade prediction and the accuracy HUD. This
// adapter owns no input, firing, targeting, or prediction hooks.
namespace combat_access {
namespace {
bool CompatibleClient() {
    const auto client = game_access::GameplayClient();
    return client && client == reinterpret_cast<Address>(GetModuleHandleA("client.dll"));
}

__declspec(noinline) Address CvarInterface() {
    __try {
        const auto module = GetModuleHandleA("tier0.dll");
        const auto factory = module ? reinterpret_cast<void*(__cdecl*)(const char*, int*)>(GetProcAddress(module,"CreateInterface")) : nullptr;
        return factory ? reinterpret_cast<Address>(factory("VEngineCvar007",nullptr)) : 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

Address FindConvar(Address cvar, const char* name) {
    if (!cvar || !name || !*name) return 0;
    const auto count = memory::read<std::uint16_t>(cvar+0x4a) & 0x7fff;
    const auto entries = memory::read<Address>(cvar+0x50);
    auto index = memory::read<std::uint16_t>(cvar+0x58);
    if (!entries || entries > UINTPTR_MAX - static_cast<std::size_t>(count) * 0x10) return 0;
    for (unsigned visited{}; index != 0xffff && visited < static_cast<unsigned>(count); ++visited) {
        if (index >= count) return 0;
        const auto entry = entries + index * 0x10;
        const auto variable = memory::read<Address>(entry);
        if (variable >= 0x10000 && variable <= UINTPTR_MAX - 0x5c
            && memory::read_string(memory::read<Address>(variable),128) == name) return variable;
        index = memory::read<std::uint16_t>(entry+0xa);
    }
    return 0;
}

std::optional<bool> ReadBooleanConvar(Address cvar, const char* name) {
    const auto variable = FindConvar(cvar,name);
    // Native boolean registration uses type0, and its consumer compares only
    // the byte at+58. Float reads include union padding and can flush a true
    // byte's denormal representation to zero under the game's floating mode.
    const auto type = variable ? memory::safe_read<std::int16_t>(variable+0x28) : std::nullopt;
    if (!type || *type != 0) return {};
    const auto value = memory::safe_read<std::uint8_t>(variable+0x58);
    return value ? std::optional<bool>{*value != 0} : std::nullopt;
}

__declspec(noinline) bool View(Address getter, Address input, Vec3& angles) {
    __try {
        const auto value = memory::call<Address>(getter,input,0);
        Vec3 result;
        if (!memory::copy(&result,value,sizeof(result)) || !Finite(result)) return false;
        angles = result;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

__declspec(noinline) bool AccuracyOffsets(std::uint32_t& begin, std::uint32_t& end) {
    __try {
        begin = systems::schemas::lookup("C_CSWeaponBase","m_flTurningInaccuracyDelta"_hash);
        end = systems::schemas::lookup("C_CSWeaponBase","m_flRecoilIndex"_hash);
        return begin && end >= begin;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

__declspec(noinline) bool Accuracy(Address weapon, Address spreadFn, Address inaccuracyFn,
    std::uint32_t begin, std::size_t size, float& spread, float& inaccuracy) {
    std::uint8_t backup[0x100]{};
    bool copied{}, valid{};
    float newSpread{}, newInaccuracy{};
    __try {
        copied = memory::copy(backup,weapon+begin,size);
        if (copied) {
            newSpread = memory::call<float>(spreadFn,weapon);
            newInaccuracy = memory::call<float>(inaccuracyFn,weapon,static_cast<float*>(nullptr),static_cast<float*>(nullptr));
            valid = std::isfinite(newSpread) && newSpread >= 0 && std::isfinite(newInaccuracy) && newInaccuracy >= 0;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { valid = false; }
    // GetInaccuracy mutates weapon state. Restore the same schema-delimited
    // span even if a native read faults, keeping these HUD reads observational.
    if (copied && !memory::store(weapon+begin,backup,size)) valid = false;
    if (valid) { spread = newSpread; inaccuracy = newInaccuracy; }
    return valid;
}
} // namespace

bool Finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

float Convar(const char* name, float fallback) {
    if (!name || !*name || !CompatibleClient()) return fallback;
    const auto variable = FindConvar(CvarInterface(),name);
    const auto value = variable ? memory::safe_read<float>(variable+0x58) : std::nullopt;
    return value && std::isfinite(*value) ? *value : fallback;
}

std::optional<bool> ConvarBool(const char* name) {
    if (!name || !*name || !CompatibleClient()) return {};
    return ReadBooleanConvar(CvarInterface(),name);
}

bool ReadViewAngles(Vec3& angles) {
    if (!CompatibleClient()) return false;
    const auto input = memory::resolve_cached(combat_bindings::patterns::csgo_input);
    const auto getter = memory::resolve_cached(combat_bindings::patterns::get_view_angles);
    return input && getter && View(getter,input,angles);
}

bool ReadAccuracy(Address weapon, float& spread, float& inaccuracy) {
    if (!weapon || !CompatibleClient()) return false;
    std::uint32_t begin{}, end{};
    if (!AccuracyOffsets(begin,end)) return false;
    const auto size = static_cast<std::size_t>(end-begin)+sizeof(float);
    if (size > 0x100 || weapon > UINTPTR_MAX-begin || weapon+begin > UINTPTR_MAX-size) return false;
    const auto spreadFn = memory::resolve_cached(combat_bindings::patterns::get_spread);
    const auto inaccuracyFn = memory::resolve_cached(combat_bindings::patterns::get_inaccuracy);
    return spreadFn && inaccuracyFn && Accuracy(weapon,spreadFn,inaccuracyFn,begin,size,spread,inaccuracy);
}
} // namespace combat_access
