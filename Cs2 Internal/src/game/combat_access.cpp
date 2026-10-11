#include "game/combat_access.h"
#include "game/combat_command.h"
#include "features/combat/detail/hvh_antiaim.hpp"
#include <limits>

namespace combat_access
{
    namespace
    {
        struct Bindings
        {
            Address client{}, createMove{}, handleView{}, inputEvent{}, input{}, cmdBase{}, cmd{}, view{}, setView{}, buttonAlloc{};
            Address crc{}, filter{}, setupTrace{}, resetTrace{}, bullet{}, punch{}, shootPosition{}, matchedShootPosition{}, spread{}, accuracy{}, spreadSample{}, spreadSeed{};
            Address cvar{}, gameRules{};
        } api;
        std::atomic<const char *> bindingStatus{"Combat bindings have not initialized."};
        struct ControllerSlot {
            Address pointer{};
            std::uint32_t handle{0xffffffffu};
            bool operator==(const ControllerSlot&) const = default;
        };
        ControllerSlot ReadControllerSlot(Address list, unsigned index)
        {
            if (!list || !index || index >= 0x7fff) return {};
            const auto page = memory::safe_read<Address>(list + 0x10 + 8 * (index >> 9)).value_or(0);
            if (!page || page > UINTPTR_MAX - 0x70 * (index & 511) - 0x14) return {};
            const auto slot = page + 0x70 * (index & 511);
            const auto pointer = memory::safe_read<Address>(slot).value_or(0);
            const auto handle = memory::safe_read<std::uint32_t>(slot + 0x10).value_or(0xffffffffu);
            if (!game_access::Valid(pointer, sizeof(Address)) || handle == 0xffffffffu || (handle & 0x7fff) != index)
                return {};
            return {pointer, handle};
        }
        bool ControllerClass(Address entity)
        {
            return game_access::Valid(entity, 0x18) && memory::read_string(reinterpret_cast<Address>(systems::g_entities.get_schema_name(entity)), 64) == "CCSPlayerController";
        }
        struct DiscoveryKey {
            Address list{}, controller{}, pawn{};
            std::uint32_t handle{};
            bool operator==(const DiscoveryKey&) const = default;
        };
        struct ControllerDiscovery {
            std::mutex mutex;
            DiscoveryKey key{};
            int lastCommand{};
            unsigned cursor{};
            std::array<Address, 64> pages{};
            std::vector<unsigned> indices;
            void clear() {
                key = {}; lastCommand = 0; cursor = 0; pages = {}; indices.clear();
            }
            std::vector<unsigned> read(const Context& context) {
                std::scoped_lock lock(mutex);
                const DiscoveryKey current{context.local.list, context.local.controller, context.local.pawn, context.local.handle};
                if (current != key || context.commandNumber < lastCommand) { clear(); key = current; }
                std::array<Address, 64> livePages{};
                std::optional<unsigned> changed;
                for (unsigned page = 0; page < livePages.size(); ++page) {
                    livePages[page] = memory::safe_read<Address>(current.list + 0x10 + page * 8).value_or(0);
                    if (livePages[page] && livePages[page] != pages[page] && !changed) changed = page;
                }
                // An independent discovery budget: one populated512-slot page
                // per new command, rather than depending on camera/ESP refresh.
                if (context.commandNumber != lastCommand || changed) {
                    std::optional<unsigned> selected = changed;
                    if (!selected)
                        for (unsigned offset = 0; offset < livePages.size(); ++offset) {
                            const auto page = (cursor + offset) % 64u;
                            if (livePages[page]) { selected = page; break; }
                        }
                    if (selected) {
                        const auto page = *selected;
                        std::array<std::byte, 512 * 0x70> snapshot;
                        if (memory::copy(snapshot.data(), livePages[page], snapshot.size()) &&
                            memory::safe_read<Address>(current.list + 0x10 + page * 8).value_or(0) == livePages[page]) {
                            for (unsigned offset = 0; offset < 512; ++offset) {
                                const unsigned index = page * 512 + offset;
                                Address entity{}; std::uint32_t handle{};
                                std::memcpy(&entity, snapshot.data() + offset * 0x70, sizeof(entity));
                                std::memcpy(&handle, snapshot.data() + offset * 0x70 + 0x10, sizeof(handle));
                                if (index > 64 && index < 0x7fff && handle != 0xffffffffu && (handle & 0x7fff) == index &&
                                    ControllerClass(entity) && std::find(indices.begin(), indices.end(), index) == indices.end())
                                    indices.push_back(index);
                            }
                        }
                        cursor = (page + 1) % 64u;
                    }
                    lastCommand = context.commandNumber;
                }
                pages = livePages;
                // Reused list/pages and slot allocations are checked fresh;
                // cached pointers or serials never authorize the returned pawn.
                std::erase_if(indices, [&](unsigned index) { return !ControllerClass(ReadControllerSlot(current.list, index).pointer); });
                std::vector<unsigned> result;
                result.reserve(64 + indices.size());
                for (unsigned index = 1; index <= 64; ++index) result.push_back(index);
                result.insert(result.end(), indices.begin(), indices.end());
                return result;
            }
        } targetDiscovery, historyDiscovery;
        __declspec(noinline) Address CurrentCommand(Address controller)
        {
            __try
            {
                if (!controller || !api.cmdBase || !api.cmd) return 0;
                const auto base = memory::call<Address>(api.cmdBase, controller);
                if (!base) return 0;
                const auto sequence = memory::read<int>(base + 0x5910);
                const auto command = sequence > 0 ? memory::call<Address>(api.cmd, controller, sequence) : 0;
                return command && memory::safe_read<int>(command + combat_bindings::kCommandSequence).value_or(0) == sequence ? command : 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        }
        __declspec(noinline) bool ViewAndPunch(Address input, Address services, Vec3 &angles, Vec3 &punch)
        {
            __try
            {
                const auto view = memory::call<Address>(api.view, input, 0);
                if (!memory::copy(&angles, view, sizeof(angles))) return false;
                if (services) memory::call<void>(api.punch, services, &punch, 0u);
                return Finite(angles) && Finite(punch);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        __declspec(noinline) bool Accuracy(Weapon &weapon, unsigned begin, unsigned size)
        {
            std::uint8_t backup[256]{};
            if (!size || size > sizeof(backup) || !memory::copy(backup, weapon.address + begin, size)) return false;
            bool result = false;
            __try
            {
                weapon.spread = memory::call<float>(api.spread, weapon.address);
                weapon.inaccuracy = memory::call<float>(api.accuracy, weapon.address,
                    static_cast<float *>(nullptr), static_cast<float *>(nullptr));
                result = std::isfinite(weapon.spread) && std::isfinite(weapon.inaccuracy) &&
                    weapon.spread >= 0 && weapon.inaccuracy >= 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            return memory::store(weapon.address + begin, backup, size) && result;
        }
        __declspec(noinline) bool NativeCrc(Address rawBase, Address crcSlot, const std::uint8_t *bytes, int size)
        {
            if (!rawBase || !crcSlot || !api.crc || size < 0 || size > 64 || (size && !bytes)) return false;
            struct alignas(8) NativeStringView
            {
                std::array<std::byte, 16> storage{};
                std::size_t length{}, capacity{15};
            };
            static_assert(sizeof(NativeStringView) == 0x20);
            static_assert(offsetof(NativeStringView, length) == 0x10 && offsetof(NativeStringView, capacity) == 0x18);
            NativeStringView message;
            message.length = static_cast<std::size_t>(size);
            if (size < 16)
            {
                if (size) std::memcpy(message.storage.data(), bytes, static_cast<std::size_t>(size));
            }
            else
            {
                std::memcpy(message.storage.data(), &bytes, sizeof(bytes));
                message.capacity = static_cast<std::size_t>(size);
            }
            __try
            {
                const auto arenaBits = memory::read<Address>(rawBase + 8);
                const auto arena = arenaBits & 1 ? memory::read<Address>(arenaBits & ~Address{3}) : arenaBits & ~Address{3};
                // The native routine copies this 32-byte string view into its
                // own storage; the borrowed source needs no allocation or free.
                memory::call<void>(api.crc, reinterpret_cast<void *>(crcSlot), reinterpret_cast<Address>(&message), arena);
                // Native allocation failure leaves the ownership tag (2) with
                // no string object. Reusing an existing tagged object is valid.
                const auto tagged = memory::safe_read<Address>(crcSlot);
                const auto string = tagged ? *tagged & ~Address{3} : 0;
                std::array<std::byte, 0x20> readable{};
                return string && memory::copy(readable.data(), string, readable.size());
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        std::optional<combat_bindings::HistoryShotTiming> ReadHistoryTiming(Address command)
        {
            using namespace combat_bindings;
            const auto proto = memory::safe_read<CommandProto>(command + kCommandProto);
            if (!command || !proto || proto->history.size <= 0 || proto->history.size > 64 ||
            proto->history.capacity < proto->history.size || !proto->history.entries ||
            memory::safe_read<int>(proto->history.entries).value_or(0) < proto->history.size) return {};
            const auto raw = memory::safe_read<Address>(proto->history.entries + 8 + (proto->history.size - 1) * sizeof(Address)).value_or(0);
            if (!raw) return {};
            const auto bits = memory::safe_read<std::uint32_t>(raw + kMessagePayload).value_or(0);
            if (!(bits & kHistoryPlayerTickBit)) return {};
            auto timing = memory::safe_read<HistoryShotTiming>(raw + kMessagePayload + kHistoryPlayerTick);
            if (!timing) return {};
            if (!(bits & kHistoryPlayerFractionBit)) timing->fraction = 0;
            if (timing->tick <= 0 || !std::isfinite(timing->fraction) || timing->fraction < 0 || timing->fraction >= 1) return {};
            return timing;
        }
        bool ReadyContext(const Context &context)
        {
            if (!context.valid || !context.canShoot || !context.command || !context.base ||
                context.input != api.input) return false;
            const auto local = game_access::ReadLocalPlayer(api.client);
            if (local.state != game_access::PlayerState::Alive || local.list != context.local.list ||
                local.controller != context.local.controller || local.pawn != context.local.pawn ||
                local.handle != context.local.handle || local.team != context.local.team) return false;
            const auto services = Field<Address>(local.pawn, "C_BasePlayerPawn", "m_pWeaponServices").value_or(0);
            const auto handle = Field<std::uint32_t>(services, "CPlayer_WeaponServices", "m_hActiveWeapon").value_or(0xffffffffu);
            if (handle != context.weapon.handle || game_access::Resolve(local.list, handle) != context.weapon.address)
                return false;
            const auto subclass = systems::schemas::lookup("C_BaseEntity", "m_nSubclassID"_hash);
            if (!subclass || memory::safe_read<Address>(context.weapon.address + subclass + 8).value_or(0) != context.weapon.data)
                return false;
            if (CurrentCommand(local.controller) != context.command) return false;
            const auto base = memory::safe_read<combat_bindings::BaseCommand>(context.base);
            const auto tick = Field<int>(local.controller, "CBasePlayerController", "m_nTickBase");
            const auto next = Field<int>(context.weapon.address, "C_BasePlayerWeapon", "m_nNextPrimaryAttackTick");
            const auto clip = Field<int>(context.weapon.address, "C_BasePlayerWeapon", "m_iClip1");
            const auto reload = Field<bool>(context.weapon.address, "C_CSWeaponBase", "m_bInReload");
            if (!base || !tick || *tick <= 0 || !next || !clip || *clip <= 0 || !reload || *reload) return false;
            const auto clientTick = base->bits & 0x20 ? base->clientTick : *tick;
            if (clientTick <= 0 || clientTick < *next) return false;
            // The angle was calculated from this eye. A fresh penetration query
            // cannot authorize a command built from a different position.
            const auto timing = ReadHistoryTiming(context.command);
            if (timing.has_value() != context.shotTiming.has_value() || (timing &&
                (timing->tick != context.shotTiming->tick || timing->fraction != context.shotTiming->fraction))) return false;
            Vec3 eye{};
            if (!Finite(context.eye) || !(timing ? ReadShotPosition(local.pawn, *timing, eye) :
                ReadShootPosition(local.pawn, eye))) return false;
            const auto x = eye.x - context.eye.x, y = eye.y - context.eye.y, z = eye.z - context.eye.z;
            return x*x + y*y + z*z <= .0001f;
        }
        __declspec(noinline) Address AllocateButtons(Address rawBase)
        {
            __try
            {
                const auto bits = memory::read<Address>(rawBase + 8);
                const auto arena = bits & 1 ? memory::read<Address>(bits & ~Address{3}) : bits & ~Address{3};
                return memory::call<Address>(api.buttonAlloc, arena);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        }
    }
    bool InitializeInput()
    {
        game_access::verifiedGameplayClient = 0;
        if (!game::module_matches("client.dll", skin_bindings::kClientSha256))
        {
            bindingStatus = "The installed client does not match the combat bindings.";
            return false;
        }
        api.client = reinterpret_cast<Address>(GetModuleHandleW(L"client.dll"));
        using namespace combat_bindings::patterns;
        struct RequiredBinding { Address *slot; std::string_view expression; const char *failure; };
        const RequiredBinding bindings[] = {
            {&api.createMove, create_move, "CreateMove binding unavailable."},
            {&api.handleView, handle_view_angles, "View angle processing binding unavailable."},
            {&api.setView, set_view_angles, "View angle restoration binding unavailable."},
            {&api.input, csgo_input, "Game input object unavailable."},
            {&api.cmdBase, get_usercmd_base, "Command buffer binding unavailable."},
            {&api.cmd, get_usercmd, "User command binding unavailable."},
            {&api.view, get_view_angles, "View angle binding unavailable."},
            {&api.buttonAlloc, button_state_alloc, "Button allocator binding unavailable."},
            {&api.crc, serialize_move_crc, "Command checksum binding unavailable."}};
        bool ready = true;
        for (const auto &[slot, expression, failure] : bindings)
        {
            *slot = memory::resolve_cached(expression);
            if (!*slot && ready) bindingStatus = failure;
            ready = *slot != 0 && ready;
        }
        if (!ready) return false;
        if (!addresses::globals::schema_system)
        {
            const auto module = GetModuleHandleW(L"schemasystem.dll");
            const auto factory = module ? reinterpret_cast<void *(__cdecl *)(const char *, int *)>(
                GetProcAddress(module, "CreateInterface")) : nullptr;
            addresses::globals::schema_system = factory ?
                reinterpret_cast<Address>(factory("SchemaSystem_001", nullptr)) : 0;
        }
        if (!addresses::globals::schema_system) { bindingStatus = "The game schema system is unavailable."; return false; }
        game_access::verifiedGameplayClient.store(api.client, std::memory_order_release);
        bindingStatus = "Input bindings connected.";
        return true;
    }
    bool Initialize()
    {
        if (!InitializeInput()) return false;
        using namespace combat_bindings::patterns;
        struct RequiredBinding { Address *slot; std::string_view expression; const char *failure; };
        const RequiredBinding bindings[] = {
            {&api.filter, trace_filter_init, "Bullet trace filter binding unavailable."},
            {&api.setupTrace, trace_bullet_data_init, "Bullet trace initialization unavailable."},
            {&api.resetTrace, trace_bullet_data_reset, "Bullet trace cleanup unavailable."},
            {&api.bullet, trace_bullet, "Bullet penetration binding unavailable."},
            {&api.punch, get_aim_punch, "Recoil binding unavailable."},
            {&api.shootPosition, get_shoot_position, "Shot eye position binding unavailable."},
            {&api.matchedShootPosition, get_matched_shoot_position, "Time-matched shot eye binding unavailable."},
            {&api.spread, get_spread, "Weapon spread binding unavailable."},
            {&api.accuracy, get_inaccuracy, "Weapon accuracy binding unavailable."},
            {&api.spreadSample, weapon_calculate_spread, "Spread sampling binding unavailable."},
            {&api.spreadSeed, get_tick_view_angles, "Shot spread seed binding unavailable."}};
        bool ready = true;
        for (const auto &[slot, expression, failure] : bindings)
        {
            *slot = memory::resolve_cached(expression);
            if (!*slot && ready) bindingStatus = failure;
            ready = *slot != 0 && ready;
        }
        if (!ready) return false;
        // Native bullet setup resolves its own trace manager. Combat does not
        // depend on the visual subsystem's cached manager pointer.
        bindingStatus = "Combat bindings connected.";
        return true;
    }
    const char *BindingStatus() { return bindingStatus.load(); }
    Address CreateMoveTarget() { return api.createMove; }
    Address HandleViewAnglesTarget() { return api.handleView; }
    std::optional<CommandIdentity> ReadInputCommand(Address input)
    {
        if (!input || input != api.input || !api.client) return {};
        const auto local = game_access::ReadLocalPlayer(api.client);
        if (local.state != game_access::PlayerState::Alive || !local.controller ||
            !local.pawn || local.handle == 0xffffffffu) return {};
        const auto command = CurrentCommand(local.controller);
        if (!command) return {};
        const auto sequence = memory::safe_read<int>(command + combat_bindings::kCommandSequence);
        const auto type = memory::safe_read<int>(command + combat_bindings::kCommandType);
        const auto selection = memory::safe_read<std::uint32_t>(input + combat_bindings::kInputWeaponSelection);
        if (!sequence || *sequence <= 0 || !type || !selection) return {};
        // Native B6C7AA clears this field to -1; B664CF consumes frame+46C.
        return CommandIdentity{command, *sequence, local.handle, *type == 2, *selection != 0xffffffffu};
    }
    __declspec(noinline) bool ReadViewAngles(Address input, Vec3 &angles)
    {
        if (!input || input != api.input || !api.view) return false;
        __try
        {
            const auto pointer = memory::call<Address>(api.view, input, 0);
            Vec3 value{};
            if (!memory::copy(&value, pointer, sizeof(value)) || !Finite(value)) return false;
            angles = value;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    __declspec(noinline) bool WriteViewAngles(Address input, const Vec3 &angles)
    {
        if (!input || input != api.input || !api.setView || !Finite(angles)) return false;
        __try
        {
            memory::call<void>(api.setView, input, 0, &angles);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool WriteViewAngles(const Vec3 &angles) { return WriteViewAngles(api.input, angles); }
    __declspec(noinline) bool ReadShootPosition(Address pawn, Vec3 &position)
    {
        if (!pawn || !api.shootPosition) return false;
        const auto services = Field<Address>(pawn, "C_BasePlayerPawn", "m_pWeaponServices").value_or(0);
        if (!services || memory::safe_read<Address>(services + 0x38).value_or(0) != pawn) return false;
        __try
        {
            Vec3 value{NAN, NAN, NAN};
            // Native903E74 invokes current slot32=A7FB20; this function reads
            // the owner's actual shoot eye, including its native adjustments.
            const auto returned = memory::call<Address>(api.shootPosition, services, &value);
            if (returned != reinterpret_cast<Address>(&value) || !Finite(value)) return false;
            position = value;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    __declspec(noinline) bool ReadShotPosition(Address pawn, combat_bindings::HistoryShotTiming timing, Vec3 &position)
    {
        if (!pawn || !api.matchedShootPosition || !api.client || timing.tick <= 0 ||
            !std::isfinite(timing.fraction) || timing.fraction < 0 || timing.fraction >= 1) return false;
        const auto local = game_access::ReadLocalPlayer(api.client);
        if (local.state != game_access::PlayerState::Alive || local.pawn != pawn) return false;
        const auto controllerHandle = Field<std::uint32_t>(pawn, "C_BasePlayerPawn", "m_hController").value_or(0xffffffffu);
        if (game_access::Resolve(local.list, controllerHandle) != local.controller) return false;
        const auto services = Field<Address>(pawn, "C_BasePlayerPawn", "m_pWeaponServices").value_or(0);
        if (!services || memory::safe_read<Address>(services + 0x38).value_or(0) != pawn) return false;
        const auto head = memory::safe_read<int>(services + 0x368);
        const auto count = memory::safe_read<int>(services + 0x36c);
        if (!head || !count || *head < 0 || *head >= 32 || *count < 0 || *count > 32) return false;
        __try
        {
            Vec3 value{NAN, NAN, NAN};
            // Native80DA20 subtracts controller interpolation from this shot
            // timestamp, then interpolates the corresponding weapon-service
            // history eyes. Its own fallback uses the current native slot32.
            // Native80E51E also passes both optional diagnostic arguments null.
            const auto returned = memory::call<Address>(api.matchedShootPosition, &value, pawn, &timing,
                static_cast<const Vec3 *>(nullptr), static_cast<const combat_bindings::HistoryShotTiming *>(nullptr));
            if (returned != reinterpret_cast<Address>(&value) || !Finite(value)) return false;
            position = value;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    Address InputEventTarget() { return api.inputEvent; }
    combat_command::IO CommandIO() { return {memory::copy, memory::store, AllocateButtons, NativeCrc}; }
    Context ReadContext(Address input, bool readAncillarySettings)
    {
        Context context;
        const auto fail = [&](const char *reason) { context.failure = reason; return context; };
        context.input = input;
        context.local = game_access::ReadLocalPlayer(api.client);
        if (context.local.state != game_access::PlayerState::Alive) return fail("Waiting for an alive local player.");
        if (input != api.input) return fail("The input callback uses a different input object.");
        context.command = CurrentCommand(context.local.controller);
        const auto protoBits = memory::safe_read<std::uint32_t>(context.command + combat_bindings::kCommandProto);
        if (!context.command) return fail("No current user command was returned.");
        if (!protoBits || !(*protoBits & 1)) return fail("The current command has no base message.");
        const auto rawBase = memory::read<Address>(context.command + combat_bindings::kCommandBase);
        context.base = rawBase ? rawBase + combat_bindings::kMessagePayload : 0;
        if (!context.command || !context.base ||
            !memory::copy(&context.baseSnapshot, context.base, sizeof(context.baseSnapshot))) return fail("The command base could not be read.");
        // The protobuf legacy number is optional and can stay zero. The native
        // command-ring sequence is the identifier validated by get_usercmd.
        context.commandNumber = memory::safe_read<int>(context.command + combat_bindings::kCommandSequence).value_or(0);
        const auto tick = Field<int>(context.local.controller, "CBasePlayerController", "m_nTickBase");
        if (!tick || *tick <= 0 || context.commandNumber <= 0) return fail("Player tick or command number is unavailable.");
        context.tick = *tick;
        context.shotTiming = ReadHistoryTiming(context.command);
        if (!(context.shotTiming ? ReadShotPosition(context.local.pawn, *context.shotTiming, context.eye) :
            ReadShootPosition(context.local.pawn, context.eye))) return fail("Player shot eye position is unavailable.");
        const auto punchServices = Field<Address>(context.local.pawn, "C_CSPlayerPawn", "m_pAimPunchServices").value_or(0);
        if (!ViewAndPunch(input, punchServices, context.angles, context.punch)) return fail("View angles or recoil could not be read.");
        const auto services = Field<Address>(context.local.pawn, "C_BasePlayerPawn", "m_pWeaponServices").value_or(0);
        context.weapon.handle = Field<std::uint32_t>(services, "CPlayer_WeaponServices", "m_hActiveWeapon").value_or(0xffffffffu);
        auto &weapon = context.weapon;
        weapon.address = game_access::Resolve(context.local.list, weapon.handle);
        const auto subclass = systems::schemas::lookup("C_BaseEntity", "m_nSubclassID"_hash);
        weapon.data = subclass && weapon.address ? memory::read<Address>(weapon.address + subclass + 8) : 0;
        if (!weapon.address) return fail("The active weapon handle could not be resolved.");
        if (!weapon.data) return fail("Active weapon data is unavailable.");
        weapon.type = Field<int>(weapon.data, "CCSWeaponBaseVData", "m_WeaponType").value_or(0);
        if (weapon.type < 1 || weapon.type > 6) return fail("Equip a firearm to use combat features.");
        weapon.damage = static_cast<float>(Field<int>(weapon.data, "CCSWeaponBaseVData", "m_nDamage").value_or(0));
        weapon.penetration = Field<float>(weapon.data, "CCSWeaponBaseVData", "m_flPenetration").value_or(0);
        weapon.range = Field<float>(weapon.data, "CCSWeaponBaseVData", "m_flRange").value_or(0);
        weapon.rangeModifier = Field<float>(weapon.data, "CCSWeaponBaseVData", "m_flRangeModifier").value_or(0);
        weapon.armorRatio = Field<float>(weapon.data, "CCSWeaponBaseVData", "m_flArmorRatio").value_or(0);
        weapon.headMultiplier = Field<float>(weapon.data, "CCSWeaponBaseVData", "m_flHeadshotMultiplier").value_or(0);
        weapon.bullets = Field<int>(weapon.data, "CCSWeaponBaseVData", "m_nNumBullets").value_or(0);
        weapon.clip = Field<int>(weapon.address, "C_BasePlayerWeapon", "m_iClip1").value_or(0);
        weapon.recoil = Field<float>(weapon.address, "C_CSWeaponBase", "m_flRecoilIndex").value_or(0);
        const auto manager = systems::schemas::lookup("C_EconEntity", "m_AttributeManager"_hash);
        const auto item = systems::schemas::lookup("C_AttributeContainer", "m_Item"_hash);
        const auto definition = systems::schemas::lookup("C_EconItemView", "m_iItemDefinitionIndex"_hash);
        if (!manager || !item || !definition) return fail("Weapon item schema fields are unavailable.");
        weapon.definition = memory::read<std::uint16_t>(weapon.address + manager + item + definition);
        for (const float value : {weapon.damage, weapon.penetration, weapon.range, weapon.rangeModifier,
                                  weapon.armorRatio, weapon.headMultiplier, weapon.recoil})
            if (!std::isfinite(value) || value < 0) return fail("Weapon ballistic values are invalid.");
        if (weapon.damage <= 0 || weapon.range <= 0 || weapon.range > 32768 || weapon.bullets <= 0) return fail("Weapon damage, range or bullet count is unavailable.");
        const auto accuracyBegin = systems::schemas::lookup("C_CSWeaponBase", "m_flTurningInaccuracyDelta"_hash);
        const auto accuracyEnd = systems::schemas::lookup("C_CSWeaponBase", "m_flRecoilIndex"_hash);
        context.accuracyValid = accuracyBegin && accuracyEnd >= accuracyBegin &&
            Accuracy(weapon, accuracyBegin, accuracyEnd - accuracyBegin + 4);
        const auto nextAttack = Field<int>(weapon.address, "C_BasePlayerWeapon", "m_nNextPrimaryAttackTick");
        const bool reloading = Field<bool>(weapon.address, "C_CSWeaponBase", "m_bInReload").value_or(true);
        const auto clientTick = context.baseSnapshot.bits & 0x20 ? context.baseSnapshot.clientTick : context.tick;
        context.canShoot = nextAttack && !reloading && weapon.clip > 0 && clientTick >= *nextAttack;
        if (readAncillarySettings) {
            context.sensitivity = Convar("sensitivity", 1);
            context.fovAdjust = Field<float>(context.local.pawn, "C_BasePlayerPawn", "m_flFOVSensitivityAdjust").value_or(1);
            context.scales = {Convar("mp_damage_scale_ct_head", 1), Convar("mp_damage_scale_t_head", 1),
                              Convar("mp_damage_scale_ct_body", 1), Convar("mp_damage_scale_t_body", 1)};
        }
        context.valid = true;
        context.failure = nullptr;
        return context;
    }
    namespace
    {
        bool MatchesCommittedShot(const OrientationContext &context, const CommittedShot &shot)
        {
            using namespace combat_bindings;
            if (shot.command != context.command || shot.sequence != context.commandNumber ||
                shot.localHandle != context.local.handle || shot.weaponHandle != context.weaponHandle ||
                !Finite(shot.angles) || std::abs(shot.angles.x) > 89.f || shot.timing.tick <= 0 ||
                !std::isfinite(shot.timing.fraction) || shot.timing.fraction < 0 || shot.timing.fraction >= 1) return false;
            const auto proto = memory::safe_read<CommandProto>(context.command + kCommandProto);
            const auto raw = memory::safe_read<ButtonState>(context.command + kCommandButtons);
            if (!proto || !raw || !(raw->value & kAttack) || !(proto->bits & 0x20) ||
                proto->history.size <= 0 || proto->history.size > 64 || proto->history.capacity < proto->history.size ||
                !proto->history.entries || proto->attack1History != proto->history.size - 1 ||
                memory::safe_read<int>(proto->history.entries).value_or(0) < proto->history.size) return false;
            constexpr auto required = 1u | kHistoryPlayerTickBit | kHistoryPlayerFractionBit;
            for (int i = 0; i < proto->history.size; ++i) {
                const auto entry = memory::safe_read<Address>(proto->history.entries + 8 + i * sizeof(Address)).value_or(0);
                const auto header = entry ? memory::safe_read<HistoryHeader>(entry + kMessagePayload) : std::nullopt;
                if (!header || (header->bits & required) != required || !header->angles) return false;
                const auto angles = memory::safe_read<AnglePayload>(header->angles + kMessagePayload);
                const auto timing = memory::safe_read<HistoryShotTiming>(entry + kMessagePayload + kHistoryPlayerTick);
                if (!angles || (angles->bits & 7) != 7 || !timing || timing->tick != shot.timing.tick ||
                    timing->fraction != shot.timing.fraction || angles->pitch != shot.angles.x ||
                    angles->yaw != std::remainder(shot.angles.y, 360.f) ||
                    angles->roll != std::remainder(shot.angles.z, 360.f)) return false;
                if (shot.backtrack) {
                    const auto render = memory::safe_read<HistoryShotTiming>(entry + kMessagePayload + kHistoryRenderTick);
                    const auto target = memory::safe_read<int>(entry + kMessagePayload + kHistoryTargetIndex);
                    if ((header->bits & (kHistoryRenderBits | 0x4000)) != (kHistoryRenderBits | 0x4000) ||
                        !render || !target || render->tick != shot.backtrack->render.tick ||
                        render->fraction != shot.backtrack->render.fraction || *target != shot.backtrack->targetIndex) return false;
                }
            }
            return true;
        }
    }
    OrientationContext ReadOrientationContext(Address input, const CommittedShot *shot)
    {
        using namespace combat_bindings;
        OrientationContext context;
        context.input = input;
        const auto fail = [&](const char *reason) { context.failure = reason; return context; };
        if (!api.client || !input || input != api.input) return fail("The input object is unavailable.");
        context.local = game_access::ReadLocalPlayer(api.client);
        if (context.local.state != game_access::PlayerState::Alive) return fail("Waiting for an alive local player.");
        context.command = CurrentCommand(context.local.controller);
        if (!context.command) return fail("No current user command was returned.");
        const auto proto = memory::safe_read<CommandProto>(context.command + kCommandProto);
        if (!proto || !(proto->bits & 1) || !proto->base) return fail("The current command has no base message.");
        context.base = proto->base + kMessagePayload;
        const auto base = memory::safe_read<BaseCommand>(context.base);
        if (!base) return fail("The command base could not be read.");
        context.commandNumber = memory::safe_read<int>(context.command + kCommandSequence).value_or(0);
        context.tick = Field<int>(context.local.controller, "CBasePlayerController", "m_nTickBase").value_or(0);
        if (context.commandNumber <= 0 || context.tick <= 0) return fail("Player tick or command number is unavailable.");
        if (!ReadViewAngles(input, context.angles)) return fail("The visible view angles are unavailable.");
        const auto selection = memory::safe_read<std::uint32_t>(input + kInputWeaponSelection);
        if (!selection || *selection != 0xffffffffu) return fail("A weapon selection is pending.");
        constexpr std::uint64_t manual = kAttack | (1ull << 11) | (1ull << 5);
        const auto rawButtons = memory::safe_read<ButtonState>(context.command + kCommandButtons);
        if (!rawButtons) return fail("The current button state is unavailable.");
        std::uint64_t actions = (rawButtons->value | rawButtons->changed | rawButtons->scroll) & manual;
        if (base->buttons)
        {
            const auto buttons = memory::safe_read<ButtonPayload>(base->buttons + kMessagePayload);
            if (!buttons) return fail("The command button message is unavailable.");
            actions |= ((buttons->bits & 1) ? buttons->value : 0) & manual;
            actions |= ((buttons->bits & 2) ? buttons->changed : 0) & manual;
            actions |= ((buttons->bits & 4) ? buttons->scroll : 0) & manual;
        }
        const auto &subticks = base->subticks;
        if (subticks.size < 0 || subticks.size > 256 || subticks.capacity < subticks.size)
            return fail("The command subtick storage is invalid.");
        if (subticks.size)
        {
            if (!subticks.entries || memory::safe_read<int>(subticks.entries).value_or(0) < subticks.size)
                return fail("The command subtick storage is invalid.");
            for (int index = 0; index < subticks.size; ++index)
            {
                const auto raw = memory::safe_read<Address>(subticks.entries + 8 + index * sizeof(Address)).value_or(0);
                const auto move = raw ? memory::safe_read<SubtickPayload>(raw + kMessagePayload) : std::nullopt;
                if (!move) return fail("The command subtick storage is invalid.");
                if ((move->bits & 3) == 3 && move->pressed) actions |= move->button & manual;
            }
        }
        if (actions & (manual & ~kAttack)) return fail("Paused for secondary attack or use input.");
        const auto moveType = Field<std::uint8_t>(context.local.pawn, "C_BaseEntity", "m_nActualMoveType");
        const auto flags = Field<std::uint32_t>(context.local.pawn, "C_BaseEntity", "m_fFlags");
        const auto defusing = Field<bool>(context.local.pawn, "C_CSPlayerPawn", "m_bIsDefusing");
        const auto hostage = Field<bool>(context.local.pawn, "C_CSPlayerPawn", "m_bIsGrabbingHostage");
        if (!moveType || !flags || !defusing || !hostage) return fail("Player movement or action state is unavailable.");
        if (*moveType == 7 || *moveType == 9) return fail("Paused on ladders or in noclip.");
        if ((*flags & (1u << 6)) || *defusing || *hostage) return fail("Paused for frozen or interaction state.");
        if (!api.gameRules) api.gameRules = memory::resolve_cached(::patterns::game_rules);
        const auto rules = memory::safe_read<Address>(api.gameRules).value_or(0);
        const auto freeze = Field<bool>(rules, "C_CSGameRules", "m_bFreezePeriod");
        const auto intro = Field<bool>(rules, "C_CSGameRules", "m_bTeamIntroPeriod");
        const auto phase = Field<int>(rules, "C_CSGameRules", "m_gamePhase");
        const auto roundStart = Field<float>(rules, "C_CSGameRules", "m_fRoundStartTime");
        const auto globals = memory::safe_read<Address>(addresses::globals::global_vars).value_or(0);
        const auto time = globals ? memory::safe_read<float>(globals + 0x30) : std::nullopt;
        if (!freeze || !intro || !phase || !roundStart || !time || !std::isfinite(*roundStart) || !std::isfinite(*time))
            return fail("The current round state is unavailable.");
        if (*freeze || *intro || *phase >= 4 || *roundStart > *time) return fail("Paused during freeze time or cinematics.");
        const auto services = Field<Address>(context.local.pawn, "C_BasePlayerPawn", "m_pWeaponServices").value_or(0);
        context.weaponHandle = Field<std::uint32_t>(services, "CPlayer_WeaponServices", "m_hActiveWeapon").value_or(0xffffffffu);
        context.weapon = game_access::Resolve(context.local.list, context.weaponHandle);
        if (!context.weapon) return fail("The active weapon is unavailable.");
        const auto subclass = systems::schemas::lookup("C_BaseEntity", "m_nSubclassID"_hash);
        const auto data = subclass ? memory::safe_read<Address>(context.weapon + subclass + 8).value_or(0) : 0;
        const auto type = Field<int>(data, "CCSWeaponBaseVData", "m_WeaponType");
        if (!type) return fail("The active weapon type is unavailable.");
        context.weaponType = *type;
        if (shot) {
            if (*type < 1 || *type > 6 || !MatchesCommittedShot(context, *shot))
                return fail("The committed shot history changed; keeping its shooting angles.");
            context.shot = *shot;
        } else if (actions & kAttack) {
            // A held trigger must not disable orientation for the entire spray.
            // Keep native shooting angles only on commands that could fire.
            if (*type < 1 || *type > 6) return fail("Paused for a manual weapon action.");
            const auto nextAttack = Field<int>(context.weapon, "C_BasePlayerWeapon", "m_nNextPrimaryAttackTick");
            const auto clip = Field<int>(context.weapon, "C_BasePlayerWeapon", "m_iClip1");
            const auto reload = Field<bool>(context.weapon, "C_CSWeaponBase", "m_bInReload");
            const auto clientTick = base->bits & 0x20 ? base->clientTick : context.tick;
            if (!nextAttack || *nextAttack < 0 || !clip || !reload || clientTick <= 0)
                return fail("Manual shot readiness is unavailable.");
            if (!*reload && *clip > 0 && clientTick >= *nextAttack)
                return fail("Preserving the current manual shot; anti aim resumes between shots.");
        }
        if (*type == 9)
        {
            const auto throwTime = Field<float>(context.weapon, "C_BaseCSGrenade", "m_fThrowTime");
            if (!throwTime || !std::isfinite(*throwTime)) return fail("Grenade throw state is unavailable.");
            if (*throwTime > 0) return fail("Paused while throwing a grenade.");
        }
        context.valid = true;
        context.failure = nullptr;
        return context;
    }
    namespace {
        bool CurrentOrientationTarget(const OrientationContext& context,const OrientationTarget& target){
            if(game_access::Resolve(context.local.list,target.handle)!=target.pawn||!target.pawn||target.pawn==context.local.pawn)return false;
            const auto node=Field<Address>(target.pawn,"C_BaseEntity","m_pGameSceneNode").value_or(0);
            const auto localNode=Field<Address>(context.local.pawn,"C_BaseEntity","m_pGameSceneNode").value_or(0);
            const auto team=Field<std::uint8_t>(target.pawn,"C_BaseEntity","m_iTeamNum").value_or(0);
            const auto origin=Field<Vec3>(node,"CGameSceneNode","m_vecAbsOrigin");
            const auto localOrigin=Field<Vec3>(localNode,"CGameSceneNode","m_vecAbsOrigin");
            return node==target.scene&&localNode==target.localScene&&origin&&localOrigin&&
                origin->x==target.origin.x&&origin->y==target.origin.y&&origin->z==target.origin.z&&
                localOrigin->x==target.localOrigin.x&&localOrigin->y==target.localOrigin.y&&localOrigin->z==target.localOrigin.z&&
                (team==2||team==3)&&team!=context.local.team&&
                Field<int>(target.pawn,"C_BaseEntity","m_iHealth").value_or(0)>0&&
                Field<std::uint8_t>(target.pawn,"C_BaseEntity","m_lifeState").value_or(1)==0&&
                !Field<bool>(node,"CGameSceneNode","m_bDormant").value_or(true)&&
                !Field<bool>(target.pawn,"C_CSPlayerPawn","m_bGunGameImmunity").value_or(true);
        }
    }
    std::optional<OrientationTarget> ReadOrientationTarget(const OrientationContext& context,Address preferredPawn,std::uint32_t preferredHandle){
        if(!context.valid)return {};
        const auto localNode=Field<Address>(context.local.pawn,"C_BaseEntity","m_pGameSceneNode").value_or(0);
        const auto origin=Field<Vec3>(localNode,"CGameSceneNode","m_vecAbsOrigin");
        if(!origin||!Finite(*origin))return {};
        Context query;query.local=context.local;query.valid=true;query.commandNumber=context.commandNumber;
        const auto players=ReadTargets(query,nullptr,false);
        std::vector<hvh_math::Candidate> points;std::vector<Address> nodes;
        points.reserve(players.size());nodes.reserve(players.size());
        for(const auto& player:players){
            const auto node=Field<Address>(player.pawn,"C_BaseEntity","m_pGameSceneNode").value_or(0);
            const auto position=Field<Vec3>(node,"CGameSceneNode","m_vecAbsOrigin");
            if(!position||!Finite(*position))continue;
            hvh_math::Candidate candidate;candidate.pawn=player.pawn;candidate.handle=player.handle;
            candidate.team=player.team;candidate.health=player.health;candidate.valid=true;
            candidate.point={position->x,position->y,position->z};points.push_back(candidate);nodes.push_back(node);
        }
        hvh_math::View view;view.localPawn=context.local.pawn;view.localHandle=context.local.handle;view.localTeam=context.local.team;
        view.eye={origin->x,origin->y,origin->z};view.angles={context.angles.x,context.angles.y,context.angles.z};
        const auto selected=hvh_antiaim::SelectThreat(view,points,preferredPawn,preferredHandle);if(!selected)return {};
        const auto& point=points[selected->index];
        OrientationTarget result{point.pawn,nodes[selected->index],localNode,point.handle,
            {point.point.x,point.point.y,point.point.z},*origin,selected->yaw};
        return CurrentOrientationTarget(context,result)?std::optional{result}:std::nullopt;
    }
    bool ApplyOrientation(const OrientationContext &context, const Vec3 &angles,const OrientationTarget* target)
    {
        if (!context.valid || !Finite(angles) || std::abs(angles.x) > 89.f) return false;
        const auto current = ReadOrientationContext(context.input, context.shot ? &*context.shot : nullptr);
        if (!current.valid || current.command != context.command || current.base != context.base ||
            current.commandNumber != context.commandNumber || current.tick != context.tick || current.local.list != context.local.list ||
            current.local.controller != context.local.controller || current.local.pawn != context.local.pawn ||
            current.local.handle != context.local.handle || current.local.team != context.local.team ||
            current.weapon != context.weapon || current.weaponHandle != context.weaponHandle ||
            current.angles.x != context.angles.x || current.angles.y != context.angles.y || current.angles.z != context.angles.z)
            return false;
        if(target&&!CurrentOrientationTarget(current,*target))return false;
        const std::array<float, 3> value{angles.x, angles.y, 0};
        const combat_command::IO io{memory::copy, memory::store, AllocateButtons, NativeCrc};
        return combat_command::Edit(context.command, context.base, context.commandNumber, value, false, io,
            std::nullopt, combat_command::AngleMode::BaseOnly);
    }
    static std::vector<Target> ReadTargetsFrom(const Context &context, TargetScan *scan,bool geometry,ControllerDiscovery& discovery)
    {
        std::vector<Target> result;
        TargetScan progress{};
        if (scan) *scan = progress;
        if (!context.valid) { ResetTargets(); return result; }
        const auto local = game_access::ReadLocalPlayer(api.client);
        const auto sameLocal = [&](const game_access::LocalPlayer& current) {
            return current.state == game_access::PlayerState::Alive && current.list == context.local.list &&
                current.controller == context.local.controller && current.pawn == context.local.pawn &&
                current.handle == context.local.handle && current.team == context.local.team;
        };
        if (!sameLocal(local)) { ResetTargets(); return result; }
        const auto publish = [&] { if (scan) *scan = progress; };
        for (const auto index : discovery.read(context))
        {
            Target target;
            const auto controllerSlot = ReadControllerSlot(context.local.list, index);
            target.controller = controllerSlot.pointer;
            if (!target.controller || target.controller == context.local.controller ||
                memory::read_string(reinterpret_cast<Address>(systems::g_entities.get_schema_name(target.controller)), 64) != "CCSPlayerController")
                continue;
            ++progress.controllers;
            target.handle = Field<std::uint32_t>(target.controller, "CBasePlayerController", "m_hPawn").value_or(0xffffffffu);
            target.pawn = game_access::Resolve(context.local.list, target.handle);
            if (!target.pawn || target.pawn == context.local.pawn ||
                memory::read_string(reinterpret_cast<Address>(systems::g_entities.get_schema_name(target.pawn)), 64) != "C_CSPlayerPawn") continue;
            ++progress.pawns;
            target.health = Field<int>(target.pawn, "C_BaseEntity", "m_iHealth").value_or(0);
            target.team = Field<std::uint8_t>(target.pawn, "C_BaseEntity", "m_iTeamNum").value_or(0);
            if (target.health <= 0 || target.team < 2 || target.team > 3 || target.team == context.local.team ||
                Field<std::uint8_t>(target.pawn, "C_BaseEntity", "m_lifeState").value_or(1) != 0) continue;
            ++progress.enemies;
            if (Field<bool>(target.pawn, "C_CSPlayerPawn", "m_bGunGameImmunity").value_or(true)) continue;
            const auto scene = Field<Address>(target.pawn, "C_BaseEntity", "m_pGameSceneNode").value_or(0);
            if (!scene || Field<bool>(scene, "CGameSceneNode", "m_bDormant").value_or(true)) continue;
            ++progress.eligibleEnemies;
            if(geometry)target.hitboxes = ReadHitboxes(scene);
            if (target.hitboxes.count < 0 || target.hitboxes.count > static_cast<int>(target.hitboxes.entries.size()))
                target.hitboxes = {};
            if (target.hitboxes.count) {
                ++progress.hitboxPlayers;
                progress.hitboxCount += static_cast<unsigned>(target.hitboxes.count);
            }
            // Enemy discovery does not depend on optional native hitbox data.
            // Callers may use actual model bone points, still requiring a native
            // target-arrival trace before authorizing any shot.
            // Native hitbox bone indices are read individually by HVH. The ESP
            // skeleton is anatomically remapped and cannot be indexed by them.
            target.armor = Field<int>(target.pawn, "C_CSPlayerPawn", "m_ArmorValue").value_or(0);
            const auto services = Field<Address>(target.pawn, "C_BasePlayerPawn", "m_pItemServices").value_or(0);
            target.helmet = Field<bool>(services, "CCSPlayer_ItemServices", "m_bHasHelmet").value_or(false);
            if (ReadControllerSlot(context.local.list, index) != controllerSlot ||
                Field<std::uint32_t>(target.controller, "CBasePlayerController", "m_hPawn").value_or(0xffffffffu) != target.handle ||
                game_access::Resolve(context.local.list, target.handle) != target.pawn) continue;
            result.push_back(std::move(target));
        }
        if (!sameLocal(game_access::ReadLocalPlayer(api.client))) { ResetTargets(); return {}; }
        publish();
        return result;
    }
    std::vector<Target> ReadTargets(const Context& context, TargetScan* scan, bool geometry) {
        return ReadTargetsFrom(context, scan, geometry, targetDiscovery);
    }
    std::vector<Target> ReadHistoryTargets(const game_access::LocalPlayer& local, bool geometry) {
        static std::atomic<unsigned> frameSequence{};
        Context context; context.valid = true; context.local = local;
        const auto tick=Field<int>(local.controller,"CBasePlayerController","m_nTickBase");
        // Rendering can repeat one network/player tick many times. Discovery
        // still validates live slots each call, but scans a new page per tick.
        context.commandNumber=tick&&*tick>0?*tick:
            static_cast<int>(frameSequence.fetch_add(1) % (INT_MAX - 1)) + 1;
        return ReadTargetsFrom(context, nullptr, geometry, historyDiscovery);
    }
    void ResetTargets()
    {
        std::scoped_lock lock(targetDiscovery.mutex, historyDiscovery.mutex);
        targetDiscovery.clear();
        historyDiscovery.clear();
    }
    __declspec(noinline) bool PrepareTrace(const Context &context, const Vec3 &delta, TraceData &trace)
    {
        __try
        {
            systems::tracing::filter filter{};
            memory::call<void>(api.filter, &filter, context.local.pawn, std::uint64_t{0x1c300b}, std::uint8_t{3}, 15);
            // Native vectors can move from inline storage to owned buffers.
            // Reuse their pointer/capacity/flags instead of losing those buffers.
            if (!trace.arrayPointer)
            {
                trace.arrayPointer = trace.elements.data();
                trace.capacity = static_cast<int>(trace.elements.size());
                trace.flags = static_cast<int>(0x80000000);
            }
            if (!trace.hitArray)
            {
                trace.hitArray = trace.hitElements;
                trace.hitCapacity = static_cast<int>(sizeof(trace.hitElements) / 0x18);
                trace.hitFlags = static_cast<int>(0x80000000);
            }
            memory::call<void>(api.setupTrace, &trace, context.eye, delta, filter, 4, true);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    __declspec(noinline) bool ApplyBullet(const Context &context, TraceData &trace)
    {
        __try
        {
            const auto &weapon = context.weapon;
            memory::call<void>(api.bullet, &trace, weapon.damage, weapon.penetration, weapon.rangeModifier,
                               4, static_cast<int>(context.local.team), Address{0});
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    __declspec(noinline) bool ReleaseTrace(TraceData &trace)
    {
        if (!api.resetTrace)
        {
            const bool ownedElements = trace.arrayPointer && trace.arrayPointer != trace.elements.data() &&
                !(static_cast<unsigned>(trace.flags) & 0xc0000000u);
            const bool ownedContacts = trace.hitArray && trace.hitArray != trace.hitElements &&
                !(static_cast<unsigned>(trace.hitFlags) & 0xc0000000u);
            if (ownedElements || ownedContacts) return false;
            trace = {};
            return true;
        }
        __try
        {
            // Native cleanup frees owned vectors with the engine allocator and
            // restores each vector's inline storage, capacity and flags.
            memory::call<void>(api.resetTrace, &trace);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    __declspec(noinline) math::vector2 Spread(const Weapon &weapon, unsigned seed)
    {
        const auto invalid = std::numeric_limits<float>::quiet_NaN();
        constexpr int maximumPellets = 64;
        if (!api.spreadSample || weapon.bullets <= 0 || weapon.bullets > maximumPellets ||
            !std::isfinite(weapon.inaccuracy) || weapon.inaccuracy < 0 ||
            !std::isfinite(weapon.spread) || weapon.spread < 0 ||
            !std::isfinite(weapon.recoil)) return {invalid, invalid};
        std::array<float, maximumPellets> spreadX, spreadY;
        spreadX.fill(invalid);
        spreadY.fill(invalid);
        __try
        {
            memory::call<void>(api.spreadSample, static_cast<std::int16_t>(weapon.definition), weapon.bullets,
                0, seed + 1, weapon.inaccuracy, weapon.spread, weapon.recoil, spreadX.data(), spreadY.data());
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return {invalid, invalid};
        }
        for (int pellet = 0; pellet < weapon.bullets; ++pellet)
            if (!std::isfinite(spreadX[pellet]) || !std::isfinite(spreadY[pellet])) return {invalid, invalid};
        return {spreadX[0], spreadY[0]};
    }
    bool AttackDown(const Context &context)
    {
        if (!context.valid || !context.command) return false;
        const auto buttons = memory::safe_read<combat_bindings::ButtonState>(context.command + combat_bindings::kCommandButtons);
        return buttons && (buttons->value & combat_bindings::kAttack);
    }
    bool ApplyCommand(const Context &context, const std::optional<Vec3> &aim, bool pressAttack)
    {
        if ((aim && !Finite(*aim)) || !ReadyContext(context)) return false;
        std::optional<std::array<float, 3>> angles;
        if (aim) angles = std::array<float, 3>{aim->x, aim->y, aim->z};
        const combat_command::IO io{memory::copy, memory::store, AllocateButtons, NativeCrc};
        return combat_command::Edit(context.command, context.base, context.commandNumber, angles, pressAttack, io);
    }
    bool PressAttack(const Context &context)
    {
        return !AttackDown(context) && ApplyCommand(context, std::nullopt, true);
    }
    bool ReadyForShot(const Context &context) { return ReadyContext(context); }
    std::optional<bool> IsScoped(const Context &context)
    {
        if (!context.valid || context.weapon.type < 1 || context.weapon.type > 6) return {};
        switch (context.weapon.definition) {
        case 9: case 40: case 11: case 38: case 8: case 39: break;
        default: return {};
        }
        return Field<bool>(context.local.pawn, "C_CSPlayerPawn", "m_bIsScoped");
    }
    bool Scope(const Context &context)
    {
        const auto scoped = IsScoped(context);
        if (!scoped || *scoped || !ReadyContext(context)) return false;
        const auto type = Field<int>(context.weapon.data, "CCSWeaponBaseVData", "m_WeaponType");
        const auto next = Field<int>(context.weapon.address, "C_BasePlayerWeapon", "m_nNextSecondaryAttackTick");
        const auto manager = systems::schemas::lookup("C_EconEntity", "m_AttributeManager"_hash);
        const auto item = systems::schemas::lookup("C_AttributeContainer", "m_Item"_hash);
        const auto definition = systems::schemas::lookup("C_EconItemView", "m_iItemDefinitionIndex"_hash);
        const auto currentDefinition = manager && item && definition ?
            memory::safe_read<std::uint16_t>(context.weapon.address + manager + item + definition) : std::nullopt;
        const auto base = memory::safe_read<combat_bindings::BaseCommand>(context.base);
        const auto tick = base && (base->bits & 0x20) ? base->clientTick : context.tick;
        if (!type || *type != context.weapon.type || !currentDefinition || *currentDefinition != context.weapon.definition ||
            !next || *next < 0 || tick <= 0 || tick < *next) return false;
        return combat_command::Edit(context.command, context.base, context.commandNumber, std::nullopt, true,
            CommandIO(), std::nullopt, combat_command::AngleMode::ShotHistory, combat_command::FireButton::Secondary);
    }
    __declspec(noinline) std::optional<std::uint32_t> SpreadSeed(const Vec3 &angles, int tick)
    {
        if (!api.spreadSeed || !Finite(angles) || tick <= 0) return {};
        __try { return memory::call<std::uint32_t>(api.spreadSeed, Address{0}, &angles, tick); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return {}; }
    }
    std::optional<combat_bindings::HistoryShotTiming> ShotTiming(const Context &context)
    {
        return context.valid ? ReadHistoryTiming(context.command) : std::nullopt;
    }
    bool ApplyShot(const Context &context, const Vec3 &angles, combat_bindings::HistoryShotTiming timing,
                   std::optional<combat_bindings::BacktrackTiming> backtrack)
    {
        if (!Finite(angles) || std::abs(angles.x) > 89.f || !ReadyContext(context)) return false;
        const auto currentTiming = ShotTiming(context);
        if (!currentTiming || currentTiming->tick != timing.tick || currentTiming->fraction != timing.fraction) return false;
        const std::array<float, 3> value{angles.x, angles.y, angles.z};
        const combat_command::IO io{memory::copy, memory::store, AllocateButtons, NativeCrc};
        return combat_command::Edit(context.command, context.base, context.commandNumber, value, true, io,
                                    combat_command::Shot{value, timing.tick, timing.fraction, backtrack});
    }
}
