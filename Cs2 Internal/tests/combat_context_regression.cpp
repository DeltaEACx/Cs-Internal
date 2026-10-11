// Include the runtime adapter to inject native bindings without a production test API.
#include "game/combat_access.h"
namespace { HMODULE FixtureModule(LPCWSTR name); }
#define GetModuleHandleW FixtureModule
#include "game/combat_access.cpp"
#undef GetModuleHandleW
#include <iostream>
#include <stdexcept>

namespace
{
    using namespace combat_access;
    void Check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
    struct SchemaField { const char *owner, *name; std::uint32_t offset; };
    constexpr SchemaField fields[] = {
        {"CBasePlayerController", "m_nTickBase", 0x200},
        {"CBasePlayerController", "m_hPawn", 0x300},
        {"C_BasePlayerPawn", "m_hController", 0x680},
        {"C_BaseEntity", "m_nActualMoveType", 0x760},
        {"C_BaseEntity", "m_fFlags", 0x764},
        {"C_CSPlayerPawn", "m_bIsDefusing", 0x770},
        {"C_CSPlayerPawn", "m_bIsGrabbingHostage", 0x771},
        {"C_CSPlayerPawn", "m_bIsScoped", 0x772},
        {"C_BaseCSGrenade", "m_fThrowTime", 0x1a0},
        {"C_CSGameRules", "m_bFreezePeriod", 0x40},
        {"C_CSGameRules", "m_bTeamIntroPeriod", 0x100},
        {"C_CSGameRules", "m_gamePhase", 0x84},
        {"C_CSGameRules", "m_fRoundStartTime", 0x70},
        {"C_BaseEntity", "m_iHealth", static_cast<std::uint32_t>(game_access::field::C_BaseEntity::m_iHealth)},
        {"C_BaseEntity", "m_iTeamNum", static_cast<std::uint32_t>(game_access::field::C_BaseEntity::m_iTeamNum)},
        {"C_BaseEntity", "m_lifeState", static_cast<std::uint32_t>(game_access::field::C_BaseEntity::m_lifeState)},
        {"C_CSPlayerPawn", "m_bGunGameImmunity", 0x700},
        {"C_CSPlayerPawn", "m_ArmorValue", 0x720},
        {"C_BasePlayerPawn", "m_pItemServices", 0x728},
        {"CCSPlayer_ItemServices", "m_bHasHelmet", 0x49},
        {"CGameSceneNode", "m_bDormant", 0x200},
        {"C_BaseEntity", "m_pGameSceneNode", static_cast<std::uint32_t>(game_access::field::C_BaseEntity::m_pGameSceneNode)},
        {"C_BaseEntity", "m_nSubclassID", static_cast<std::uint32_t>(game_access::field::C_BaseEntity::m_nSubclassID)},
        {"CGameSceneNode", "m_vecAbsOrigin", 0x100},
        {"C_BaseModelEntity", "m_vecViewOffset", 0x600},
        {"C_CSPlayerPawn", "m_pAimPunchServices", 0x620},
        {"C_BasePlayerPawn", "m_pWeaponServices", static_cast<std::uint32_t>(game_access::field::C_BasePlayerPawn::m_pWeaponServices)},
        {"C_BasePlayerPawn", "m_flFOVSensitivityAdjust", 0x640},
        {"CPlayer_WeaponServices", "m_hActiveWeapon", static_cast<std::uint32_t>(game_access::field::CPlayer_WeaponServices::m_hActiveWeapon)},
        {"CCSWeaponBaseVData", "m_WeaponType", 0x100},
        {"CCSWeaponBaseVData", "m_nDamage", 0x104},
        {"CCSWeaponBaseVData", "m_flPenetration", 0x108},
        {"CCSWeaponBaseVData", "m_flRange", 0x10c},
        {"CCSWeaponBaseVData", "m_flRangeModifier", 0x110},
        {"CCSWeaponBaseVData", "m_flArmorRatio", 0x114},
        {"CCSWeaponBaseVData", "m_flHeadshotMultiplier", 0x118},
        {"CCSWeaponBaseVData", "m_nNumBullets", 0x11c},
        {"C_BasePlayerWeapon", "m_iClip1", 0x120},
        {"C_BasePlayerWeapon", "m_nNextPrimaryAttackTick", 0x124},
        {"C_BasePlayerWeapon", "m_nNextSecondaryAttackTick", 0x128},
        {"C_CSWeaponBase", "m_bInReload", 0x170},
        {"C_CSWeaponBase", "m_flTurningInaccuracyDelta", 0x180},
        {"C_CSWeaponBase", "m_flRecoilIndex", 0x190},
        {"C_EconEntity", "m_AttributeManager", static_cast<std::uint32_t>(game_access::field::C_EconEntity::m_AttributeManager)},
        {"C_AttributeContainer", "m_Item", static_cast<std::uint32_t>(game_access::field::C_AttributeContainer::m_Item)},
        {"C_EconItemView", "m_iItemDefinitionIndex", static_cast<std::uint32_t>(game_access::field::C_EconItemView::m_iItemDefinitionIndex)}
    };
    bool missingAccuracyField{},missingImmunityField{},missingOrientationField{},missingScopeField{},missingSecondaryField{},missingTickField{};
    template <class T, class Block> void Put(Block &block, std::size_t offset, const T &value)
    {
        Check(offset + sizeof(value) <= block.size(), "Fixture write exceeds its native buffer");
        std::memcpy(block.data() + offset, &value, sizeof(value));
    }
    template <class Block> Address AddressOf(const Block &block) { return reinterpret_cast<Address>(block.data()); }
    struct NativeContact
    {
        float enterFraction{}, exitFraction{}, damage{};
        int team{};
        std::uint16_t enter{}, exit{};
        std::uint8_t penetrate{}, padding[3]{};
    };
    static_assert(sizeof(NativeContact) == 0x18 && offsetof(NativeContact, penetrate) == 0x14);
    static_assert(offsetof(NativeContact, enter) == 0x10 && offsetof(NativeContact, exit) == 0x12);
    static_assert(sizeof(systems::tracing::filter) == 0x48);
    static_assert(offsetof(TraceData, elements) == 0x18 && offsetof(TraceData, start) == 0x1cf8);
    struct Fixture
    {
        std::vector<std::byte> client = std::vector<std::byte>(game_access::offset::dwEntityList + sizeof(Address));
        std::array<std::byte, 0x2200> list{};
        std::array<std::byte, 512 * 0x70> page{};
        std::array<std::byte, 0x1000> controller{};
        std::array<std::byte, 0x4000> pawn{};
        std::array<std::byte, 0x400> scene{},enemyScene{};
        std::array<std::byte, 0x1000> enemyController{};
        std::array<std::byte, 0x4000> enemyPawn{};
        std::array<std::byte, 0x200> enemyItems{};
        Hitboxes enemyHitboxes{};
        std::vector<systems::entities::cached> cachedControllers;
        unsigned discoveryPageCopies{}, visualCacheReads{};
        bool invalidControllerClass{},invalidPawnClass{};
        alignas(8) std::array<std::byte, 0x500> services{}, punchServices{}, data{};
        std::array<std::byte, 0x2000> weapon{};
        std::array<std::byte, 0x1000> input{};
        std::array<std::byte, 0x200> rules{};
        std::array<std::byte, 0x100> globals{};
        std::array<Address,1> rulesSlot{}, globalsSlot{};
        std::array<std::byte,0x40> orientationSubtick{};
        std::array<std::byte,0x10> orientationSubtickEntries{};
        std::array<std::byte, 0x5a00> commandBuffer{};
        std::array<std::byte, 0x100> command{}, rawBase{};
        std::array<std::byte, 0x40> commandAngles{}, commandButtons{}, historyAngles{};
        std::array<std::byte, 0x90> history{};
        alignas(8) std::array<std::byte, 0x10> historyEntries{};
        alignas(8) std::array<std::byte, 0x20> crcString{};
        Vec3 view{10, 25, 0}, shootEyeOffset{}, matchedEyeOffset{};
        combat_bindings::HistoryShotTiming lastMatchedTiming{};
        Vec3 traceDelta{4096, -2048, 512};
        std::array<NativeContact, 192> bulletContacts{};
        std::array<TraceElement, 192> bulletElements{};
        TraceData *nativeTrace{};
        bool nullCommand{}, invalidAccuracy{}, failView{}, failSetView{}, nullView{}, failShootEye{}, invalidShootEye{}, nullShootEye{}, failMatchedEye{};
        bool failFilter{}, failBullet{}, expectRetainedTrace{}, failReset{};
        bool failSample{}, invalidLastPellet{};
        bool failCrc{}, tagOnlyCrc{}, unreadableCrc{}, inspectCrc{true}, failSeed{};
        unsigned seedCalls{};
        unsigned baseCalls{}, commandCalls{}, viewCalls{}, setViewCalls{}, shootEyeCalls{}, matchedEyeCalls{}, punchCalls{}, spreadCalls{}, accuracyCalls{};
        unsigned filterCalls{}, setupCalls{}, bulletCalls{}, resetCalls{};
        unsigned sampleCalls{};
        int expectedPellets{8};
        std::array<std::uint8_t, 64> crcBytes{};
        int expectedCrcSize{};
        Address expectedCrcArena{};
        unsigned crcCalls{};
        Fixture()
        {
            using namespace game_access;
            Put(client, offset::dwEntityList, AddressOf(list));
            Put(client, offset::dwLocalPlayerController, AddressOf(controller));
            Put(list, 0x10, AddressOf(page));
            Put(page, 0x70, AddressOf(pawn));
            Put(page, 0x80, std::uint32_t{0x8001});
            Put(page, 2 * 0x70, AddressOf(weapon));
            Put(page, 2 * 0x70 + 0x10, std::uint32_t{0x8002});
            Put(controller, field::CCSPlayerController::m_hPlayerPawn, std::uint32_t{0x8001});
            Put(controller, 0x200, 1100);
            Put(page, 5 * 0x70, AddressOf(controller));
            Put(page, 5 * 0x70 + 0x10, std::uint32_t{0x8005});
            Put(pawn, 0x680, std::uint32_t{0x8005});
            Put(pawn, 0x760, std::uint8_t{2});
            Put(rules, 0x70, 10.f);
            Put(rules, 0x84, 3);
            Put(globals, 0x30, 20.f);
            rulesSlot[0]=AddressOf(rules);globalsSlot[0]=AddressOf(globals);
            Put(pawn, field::C_BaseEntity::m_iHealth, 100);
            Put(pawn, field::C_BaseEntity::m_iTeamNum, std::uint8_t{2});
            Put(pawn, field::C_BaseEntity::m_lifeState, std::uint8_t{});
            Put(pawn, field::C_BaseEntity::m_pGameSceneNode, AddressOf(scene));
            Put(pawn, 0x600, Vec3{0, 0, 64});
            Put(pawn, 0x620, AddressOf(punchServices));
            Put(pawn, 0x640, .5f);
            Put(pawn, field::C_BasePlayerPawn::m_pWeaponServices, AddressOf(services));
            Put(scene, 0x100, Vec3{100, 200, 300});
            Put(services, field::CPlayer_WeaponServices::m_hActiveWeapon, std::uint32_t{0x8002});
            Put(services, 0x38, AddressOf(pawn));
            Put(input, combat_bindings::kInputWeaponSelection, std::uint32_t{0xffffffffu});
            Put(weapon, field::C_BaseEntity::m_nSubclassID + 8, AddressOf(data));
            Put(data, 0x100, 3); // Rifle.
            Put(data, 0x104, 35);
            Put(data, 0x108, 2.f);
            Put(data, 0x10c, 8192.f);
            Put(data, 0x110, .98f);
            Put(data, 0x114, 1.5f);
            Put(data, 0x118, 4.f);
            Put(data, 0x11c, 1);
            Put(weapon, 0x120, 30);
            Put(weapon, 0x124, 1199);
            Put(weapon, 0x128, 1200);
            Put(weapon, 0x170, false);
            Put(weapon, 0x180, .75f);
            Put(weapon, 0x190, 2.f);
            Put(weapon, field::C_EconEntity::m_AttributeManager + field::C_AttributeContainer::m_Item +
                field::C_EconItemView::m_iItemDefinitionIndex, std::uint16_t{7});
            Put(commandBuffer, 0x5910, 77);
            Put(command, combat_bindings::kCommandSequence, 77);
            combat_bindings::CommandProto proto{};
            proto.bits = 1;
            proto.base = AddressOf(rawBase);
            Put(command, combat_bindings::kCommandProto, proto);
            combat_bindings::BaseCommand base{};
            base.bits = 0x20; // Client tick exists; legacy command number remains absent and zero.
            base.clientTick = 1200;
            Put(rawBase, combat_bindings::kMessagePayload, base);
        }
    };
    Fixture *fixture{};
    bool initializeBindings{}, compatibleClient{};
    std::string_view absentBinding;
    HMODULE FixtureModule(LPCWSTR name)
    {
        return fixture && std::wstring_view(name) == L"client.dll" ?
            reinterpret_cast<HMODULE>(AddressOf(fixture->client)) : nullptr;
    }
    Address __fastcall GetCommandBase(Address controller)
    {
        Check(controller == AddressOf(fixture->controller), "Native command-base ABI controller mismatch");
        ++fixture->baseCalls;
        return AddressOf(fixture->commandBuffer);
    }
    Address __fastcall GetCommand(Address controller, int sequence)
    {
        Check(controller == AddressOf(fixture->controller) && sequence == 77, "Native user-command ABI sequence mismatch");
        ++fixture->commandCalls;
        return fixture->nullCommand ? 0 : AddressOf(fixture->command);
    }
    Address __fastcall GetView(Address input, int slot)
    {
        Check(input == AddressOf(fixture->input) && slot == 0, "Native view-angle ABI mismatch");
        ++fixture->viewCalls;
        if (fixture->failView) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
        return fixture->nullView ? 0 : reinterpret_cast<Address>(&fixture->view);
    }
    void __fastcall SetView(Address input, int slot, const Vec3 *angles)
    {
        Check(input == AddressOf(fixture->input) && slot == 0 && angles, "Native angle setter ABI mismatch");
        ++fixture->setViewCalls;
        if (fixture->failSetView) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
        fixture->view = *angles;
    }
    Address __fastcall GetShootEye(Address services, Vec3 *out)
    {
        Check(services == AddressOf(fixture->services) && out, "Native shoot-eye ABI services/output mismatch");
        ++fixture->shootEyeCalls;
        if(fixture->failShootEye) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
        const auto origin=memory::read<Vec3>(AddressOf(fixture->scene)+0x100);
        const auto offset=memory::read<Vec3>(AddressOf(fixture->pawn)+0x600);
        *out=origin+offset+fixture->shootEyeOffset;
        if(fixture->invalidShootEye) out->x=NAN;
        return fixture->nullShootEye ? 0 : reinterpret_cast<Address>(out);
    }
    Address __fastcall GetMatchedEye(Vec3 *out, Address pawn, const combat_bindings::HistoryShotTiming *timing,
                                    const Vec3 *clientEye, const combat_bindings::HistoryShotTiming *clientTime)
    {
        Check(out && pawn==AddressOf(fixture->pawn) && timing && !clientEye && !clientTime,
              "Time-matched native shoot-eye ABI output/pawn/time/optional arguments mismatch");
        ++fixture->matchedEyeCalls;fixture->lastMatchedTiming=*timing;
        if(fixture->failMatchedEye) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
        const auto origin=memory::read<Vec3>(AddressOf(fixture->scene)+0x100);
        const auto offset=memory::read<Vec3>(AddressOf(fixture->pawn)+0x600);
        *out=origin+offset+fixture->matchedEyeOffset;
        if(fixture->invalidShootEye)out->x=NAN;
        return fixture->nullShootEye?0:reinterpret_cast<Address>(out);
    }
    void __fastcall GetPunch(Address services, Vec3 *out, unsigned slot)
    {
        Check(services == AddressOf(fixture->punchServices) && out && slot == 0, "Native recoil ABI mismatch");
        ++fixture->punchCalls;
        *out = {1, 2, 0};
    }
    float __fastcall GetSpread(Address weapon)
    {
        Check(weapon == AddressOf(fixture->weapon), "Native spread ABI weapon mismatch");
        ++fixture->spreadCalls;
        Put(fixture->weapon, 0x180, -999.f); // Production Accuracy must restore temporary native updates.
        return .02f;
    }
    float __fastcall GetAccuracy(Address weapon, float *pitch, float *yaw)
    {
        Check(weapon == AddressOf(fixture->weapon) && !pitch && !yaw, "Native accuracy ABI mismatch");
        ++fixture->accuracyCalls;
        Put(fixture->weapon, 0x190, 999.f);
        return fixture->invalidAccuracy ? std::numeric_limits<float>::quiet_NaN() : .03f;
    }
    void __fastcall InitFilter(systems::tracing::filter *filter, Address skipPawn, std::uint64_t mask,
                               std::uint8_t layer, int type)
    {
        Check(filter && skipPawn == AddressOf(fixture->pawn) && mask == 0x1c300b && layer == 3 && type == 15,
              "Native trace filter ABI skip/mask/layer/type mismatch");
        ++fixture->filterCalls;
        if (fixture->failFilter) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
        Check(filter->vtable == 0 && filter->mask == 0 && filter->flags == 0, "Native filter was not zero initialized");
        filter->vtable = 0x12345678;
        filter->mask = mask;
        filter->layer = layer;
        filter->flags = 0x5a;
        filter->skip_handles[0] = 0x8001;
    }
    void __fastcall SetupTrace(TraceData *trace, const Vec3 &start, const Vec3 &delta,
                               const systems::tracing::filter &filter, int penetrations, bool traceWorld)
    {
        Check(trace && start.x == 100 && start.y == 200 && start.z == 364 &&
              delta.x == fixture->traceDelta.x && delta.y == fixture->traceDelta.y && delta.z == fixture->traceDelta.z &&
              filter.vtable == 0x12345678 && filter.mask == 0x1c300b && filter.layer == 3 && filter.flags == 0x5a &&
              filter.skip_handles[0] == 0x8001 && penetrations == 4 && traceWorld,
              "Native bullet trace setup ABI vector/filter/argument mismatch");
        if (fixture->expectRetainedTrace)
            Check(trace->arrayPointer == fixture->bulletElements.data() && trace->hitArray == fixture->bulletContacts.data() &&
                  trace->capacity == 192 && trace->hitCapacity == 192 && trace->flags == 0 && trace->hitFlags == 0,
                  "Native setup lost retained external vector pointer/capacity/flags triples");
        else
            Check(trace->arrayPointer == trace->elements.data() && trace->hitArray == trace->hitElements &&
                  trace->capacity == 128 && trace->hitCapacity == 8 &&
                  trace->flags == static_cast<int>(0x80000000) && trace->hitFlags == static_cast<int>(0x80000000),
                  "Native trace buffers or layout defaults were not initialized");
        Check(trace->unknown2 == 52 && trace->fraction == 1, "Native trace scalar defaults were not initialized");
        ++fixture->setupCalls;
        fixture->nativeTrace = trace;
        const NativeContact contacts[]{{0, .1f, 35, 0, 0, 1, 0}, {.1f, .2f, 32, 0, 1, 2, 1}};
        std::memcpy(trace->hitArray, contacts, sizeof(contacts));
        trace->unknown1 = 5;
        trace->numHits = 2;
    }
    void __fastcall TraceBullet(TraceData *trace, float damage, float penetration, float rangeModifier,
                                int penetrations, int team, Address opaque)
    {
        Check(trace == fixture->nativeTrace && trace->numHits == 2 && trace->unknown1 == 5 &&
              trace->hitArray == (fixture->expectRetainedTrace ? static_cast<void *>(fixture->bulletContacts.data()) : trace->hitElements) &&
              damage == 35 && penetration == 2 && rangeModifier == .98f && penetrations == 4 && team == 2 && opaque == 0,
              "Native penetration ABI damage/penetration/range/team mismatch");
        ++fixture->bulletCalls;
        if (fixture->failBullet) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
        fixture->bulletContacts = {{{0, .1f, 35, 0, 0, 1, 0}, {.1f, .2f, 31, 0, 1, 2, 1},
                                   {.3f, .4f, 24, 0, 3, 4, 1}}};
        trace->numHits = 3;
        trace->hitArray = fixture->bulletContacts.data();
        trace->arrayPointer = fixture->bulletElements.data();
        trace->capacity = trace->hitCapacity = 192;
        trace->flags = trace->hitFlags = 0;
    }
    void __fastcall ResetTrace(TraceData *trace)
    {
        Check(trace == fixture->nativeTrace, "Native trace cleanup ABI did not receive the trace pointer");
        ++fixture->resetCalls;
        if (fixture->failReset) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
        *trace = {};
        trace->arrayPointer = trace->elements.data();
        trace->hitArray = trace->hitElements;
    }
    void __fastcall SampleSpread(std::int16_t definition, int pellets, int mode, unsigned seed,
                                  float inaccuracy, float spread, float recoil, float *outX, float *outY)
    {
        Check(definition == 7 && pellets == fixture->expectedPellets && mode == 0 && seed == 9 &&
              inaccuracy == .03f && spread == .02f && recoil == 2 && outX && outY,
              "Native spread sample ABI definition/count/seed/accuracy mismatch");
        ++fixture->sampleCalls;
        const auto x = reinterpret_cast<Address>(outX), y = reinterpret_cast<Address>(outY);
        const auto bytes = static_cast<std::size_t>(pellets) * sizeof(float);
        // The native routine writes one output per pellet; a vector2's adjacent
        // members overlap these ranges for shotguns and cannot be passed safely.
        if (!(x + bytes <= y || y + bytes <= x)) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
        if (fixture->failSample) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
        for (int pellet = 0; pellet < pellets; ++pellet)
        {
            outX[pellet] = .125f + pellet * .001f;
            outY[pellet] = -.25f - pellet * .001f;
        }
        if (fixture->invalidLastPellet) outY[pellets - 1] = std::numeric_limits<float>::quiet_NaN();
    }
    void __fastcall CommandCrc(void *slot, Address message, Address arena)
    {
        const auto expectedSlot = AddressOf(fixture->rawBase) + combat_bindings::kMessagePayload +
            offsetof(combat_bindings::BaseCommand, moveCrc);
        Check(reinterpret_cast<Address>(slot) == expectedSlot && arena == fixture->expectedCrcArena && !(message & 7),
              "Native command CRC ABI slot/arena/string alignment mismatch");
        const auto size = memory::read<std::uint64_t>(message + 0x10);
        const auto capacity = memory::read<std::uint64_t>(message + 0x18);
        Check((!fixture->inspectCrc || size == fixture->expectedCrcSize) && capacity == (size < 16 ? 15 : size),
              "Native CRC string object lacks 32-byte size/capacity fields");
        const auto data = size < 16 ? message : memory::read<Address>(message);
        Check(!fixture->inspectCrc || size < 16 || data == reinterpret_cast<Address>(fixture->crcBytes.data()),
              "Long native CRC source copied bytes instead of borrowing the supplied payload");
        std::array<std::uint8_t, 64> actual{};
        Check(!size || memory::copy(actual.data(), data, static_cast<std::size_t>(size)), "Native CRC source data unreadable");
        Check(!fixture->inspectCrc || std::equal(actual.begin(), actual.begin() + size, fixture->crcBytes.begin()), "Native CRC source payload differs from serialized command");
        ++fixture->crcCalls;
        if (fixture->failCrc) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
        const Address value = fixture->tagOnlyCrc ? Address{2} : fixture->unreadableCrc ? Address{0x10002} :
            AddressOf(fixture->crcString) | Address{2};
        Check(game_access::Store(expectedSlot, &value, sizeof(value)), "Native CRC output slot unreadable");
    }
    std::uint32_t __fastcall Seed(Address unused,const Vec3 *angles,int tick)
    {
        Check(!unused&&angles&&angles->x==10&&angles->y==25&&angles->z==3&&tick==1200,"Native spread seed ABI mismatch");
        ++fixture->seedCalls;
        if(fixture->failSeed)RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);
        return 0; // Zero is a valid seed, distinct from an unavailable callback.
    }
    void Bind(Fixture &value)
    {
        fixture = &value;
        missingAccuracyField = missingImmunityField = missingOrientationField = false;
        missingScopeField = missingSecondaryField = missingTickField = false;
        combat_access::api = {};
        combat_access::api.client = AddressOf(value.client);
        combat_access::api.input = AddressOf(value.input);
        combat_access::api.gameRules=reinterpret_cast<Address>(value.rulesSlot.data());
        addresses::globals::global_vars=reinterpret_cast<Address>(value.globalsSlot.data());
        combat_access::api.cmdBase = reinterpret_cast<Address>(GetCommandBase);
        combat_access::api.cmd = reinterpret_cast<Address>(GetCommand);
        combat_access::api.view = reinterpret_cast<Address>(GetView);
        combat_access::api.setView = reinterpret_cast<Address>(SetView);
        combat_access::api.punch = reinterpret_cast<Address>(GetPunch);
        combat_access::api.shootPosition = reinterpret_cast<Address>(GetShootEye);
        combat_access::api.matchedShootPosition = reinterpret_cast<Address>(GetMatchedEye);
        combat_access::api.spread = reinterpret_cast<Address>(GetSpread);
        combat_access::api.accuracy = reinterpret_cast<Address>(GetAccuracy);
        combat_access::api.filter = reinterpret_cast<Address>(InitFilter);
        combat_access::api.setupTrace = reinterpret_cast<Address>(SetupTrace);
        combat_access::api.bullet = reinterpret_cast<Address>(TraceBullet);
        combat_access::api.resetTrace = reinterpret_cast<Address>(ResetTrace);
        combat_access::api.spreadSample = reinterpret_cast<Address>(SampleSpread);
        combat_access::api.crc = reinterpret_cast<Address>(CommandCrc);
        combat_access::api.spreadSeed = reinterpret_cast<Address>(Seed);
    }
    void Failure(const Context &context, const char *reason)
    {
        Check(!context.valid && context.failure && std::string_view(context.failure) == reason,
              "Runtime context did not report the expected failing stage");
    }
    void InputInitializationIndependence()
    {
        Fixture native; Bind(native);
        const auto oldSchema = addresses::globals::schema_system;
        const auto oldCosmetic = game_access::verifiedClient.load();
        addresses::globals::schema_system = 1; // Fixture's schema lookup is already connected.
        game_access::verifiedClient = 0;
        initializeBindings = compatibleClient = true;
        absentBinding = combat_bindings::patterns::trace_bullet;
        Check(InitializeInput(), "Missing shooting binding disabled input initialization");
        Check(game_access::GameplayClient() == AddressOf(native.client),
              "Gameplay reads still require cosmetic installation");
        Check(!Initialize(), "Missing bullet binding incorrectly enabled shooting");
        const auto command = ReadInputCommand(AddressOf(native.input));
        Vec3 angles;
        Check(command && command->sequence == 77 && ReadViewAngles(AddressOf(native.input), angles),
              "A shooting initialization failure disconnected native movement input");
        Check(game_access::GameplayClient() == AddressOf(native.client) && !game_access::verifiedClient.load(),
              "Input readiness lost observational reads or bypassed the cosmetic guard");
        absentBinding = combat_bindings::patterns::get_usercmd;
        Check(!InitializeInput() && !game_access::verifiedGameplayClient.load(),
              "Missing input binding published a usable gameplay client");
        compatibleClient = false; absentBinding = {};
        Check(!InitializeInput() && !game_access::verifiedGameplayClient.load(),
              "Unsupported module bypassed the gameplay hash guard");
        initializeBindings = false;
        addresses::globals::schema_system = oldSchema;
        game_access::verifiedClient = oldCosmetic;
    }
    void ReadNativeContext()
    {
        Fixture memory;
        Bind(memory);
        auto context = ReadContext(AddressOf(memory.input));
        Check(context.valid && context.canShoot && context.accuracyValid && !context.failure,
              "Real ReadContext rejected a valid native command with absent legacy number");
        Check(context.commandNumber == 77 && context.baseSnapshot.commandNumber == 0 &&
              !(context.baseSnapshot.bits & 0x10), "ReadContext used optional legacy command numbering");
        Check(context.local.pawn == AddressOf(memory.pawn) && context.local.handle == 0x8001 &&
              context.weapon.address == AddressOf(memory.weapon) && context.weapon.handle == 0x8002,
              "Native serial entity handles did not resolve");
        Check(context.tick == 1100 && context.eye.x == 100 && context.eye.y == 200 && context.eye.z == 364 &&
              context.angles.x == 10 && context.angles.y == 25 && context.punch.x == 1 && context.punch.y == 2,
              "ReadContext lost tick, eye, view or recoil data");
        Check(context.weapon.type == 3 && context.weapon.definition == 7 && context.weapon.damage == 35 &&
              context.weapon.range == 8192 && context.weapon.bullets == 1 && context.weapon.clip == 30 &&
              context.weapon.spread == .02f && context.weapon.inaccuracy == .03f && context.fovAdjust == .5f,
              "ReadContext lost firearm schema values");
        Check(memory.baseCalls == 1 && memory.commandCalls == 1 && memory.viewCalls == 1 && memory.punchCalls == 1 &&
              memory.spreadCalls == 1 && memory.accuracyCalls == 1, "ReadContext did not use native function bindings");
        Check(memory::read<float>(AddressOf(memory.weapon) + 0x180) == .75f &&
              memory::read<float>(AddressOf(memory.weapon) + 0x190) == 2.f, "Accuracy sampling changed native weapon state");
        memory.invalidAccuracy = true;
        context = ReadContext(AddressOf(memory.input));
        Check(context.valid && context.canShoot && !context.accuracyValid && !context.failure,
              "Failed native accuracy sampling disabled valid combat geometry");
        Check(memory::read<float>(AddressOf(memory.weapon) + 0x180) == .75f &&
              memory::read<float>(AddressOf(memory.weapon) + 0x190) == 2.f, "Failed accuracy sampling did not restore native state");
        memory.invalidAccuracy = false;
        missingAccuracyField = true;
        context = ReadContext(AddressOf(memory.input));
        Check(context.valid && context.canShoot && !context.accuracyValid, "Missing optional accuracy schema disabled geometry");
        missingAccuracyField = missingImmunityField = missingOrientationField = false;
        Put(memory.weapon, 0x170, true);
        context = ReadContext(AddressOf(memory.input));
        Check(context.valid && !context.canShoot, "Reload should preserve context but block shots");
        Put(memory.weapon, 0x170, false);
        Put(memory.weapon, 0x120, 0);
        context = ReadContext(AddressOf(memory.input));
        Check(context.valid && !context.canShoot, "Empty clip should preserve context but block shots");
        Put(memory.weapon, 0x120, 30);
        auto base = memory::read<combat_bindings::BaseCommand>(AddressOf(memory.rawBase) + combat_bindings::kMessagePayload);
        base.bits &= ~0x20u;
        Put(memory.rawBase, combat_bindings::kMessagePayload, base);
        context = ReadContext(AddressOf(memory.input));
        Check(context.valid && !context.canShoot, "Absent client tick did not use local tick for readiness");
        Put(memory.weapon, 0x124, 1100);
        context = ReadContext(AddressOf(memory.input));
        Check(context.valid && context.canShoot, "Local-tick readiness boundary");
        Failure(ReadContext(AddressOf(memory.input) + 8), "The input callback uses a different input object.");
        memory.nullCommand = true;
        Failure(ReadContext(AddressOf(memory.input)), "No current user command was returned.");
        memory.nullCommand = false;
        Put(memory.command, combat_bindings::kCommandProto, std::uint32_t{});
        Failure(ReadContext(AddressOf(memory.input)), "The current command has no base message.");
        Put(memory.command, combat_bindings::kCommandProto, std::uint32_t{1});
        Put(memory.command, combat_bindings::kCommandSequence, 0);
        Failure(ReadContext(AddressOf(memory.input)), "No current user command was returned.");
        Put(memory.command, combat_bindings::kCommandSequence, 77);
        Put(memory.page, 2 * 0x70 + 0x10, std::uint32_t{0x10002});
        Failure(ReadContext(AddressOf(memory.input)), "The active weapon handle could not be resolved.");
    }
    void ReadNativeCameraAndIdentity()
    {
        Fixture native;
        Bind(native);
        const auto input = AddressOf(native.input);
        Put(native.command, combat_bindings::kCommandType, 2);
        auto identity = ReadInputCommand(input);
        Check(identity && identity->command == AddressOf(native.command) && identity->sequence == 77 &&
              identity->localHandle == 0x8001 && identity->subtickOverwrite,
              "Command identity lost the full handle, sequence or native overwrite status");
        Put(native.command, combat_bindings::kCommandType, 0);
        identity = ReadInputCommand(input);
        Check(identity && !identity->subtickOverwrite && !identity->weaponSelectionPending,
              "Fresh command or idle weapon selection was classified incorrectly");
        Put(native.input, combat_bindings::kInputWeaponSelection, std::uint32_t{0x8002});
        identity=ReadInputCommand(input);
        Check(identity && identity->weaponSelectionPending, "Pending native weapon selection was lost");
        Put(native.input, combat_bindings::kInputWeaponSelection, std::uint32_t{0xffffffffu});
        Check(!ReadInputCommand(input + 8), "Command identity accepted a different input object");
        Put(native.command, combat_bindings::kCommandSequence, 78);
        Check(!ReadInputCommand(input), "Command identity accepted a stale ring sequence");
        Put(native.command, combat_bindings::kCommandSequence, 77);
        Put(native.pawn, game_access::field::C_BaseEntity::m_iHealth, 0);
        Check(!ReadInputCommand(input), "Command identity accepted a dead local player");
        Put(native.pawn, game_access::field::C_BaseEntity::m_iHealth, 100);
        native.nullCommand = true;
        Check(!ReadInputCommand(input), "Command identity accepted a missing native command");
        native.nullCommand = false;
        Vec3 saved{-8, 119, 0};
        Check(ReadViewAngles(input, saved) && saved.x == 10 && saved.y == 25,
              "Camera getter did not return the native angle sample");
        const Vec3 restore{-8, 119, 0};
        Check(WriteViewAngles(input, restore) && native.setViewCalls == 1 && native.view.x == -8 && native.view.y == 119,
              "Camera setter did not use the native input/slot/vector ABI");
        Check(WriteViewAngles(Vec3{3, 5, 0}) && native.setViewCalls == 2 && native.view.x == 3 && native.view.y == 5,
              "Implicit camera setter did not use the initialized input object");
        Check(!WriteViewAngles(input + 8, restore) && !WriteViewAngles(input, Vec3{NAN, 1, 0}) &&
              !ReadViewAngles(input + 8, saved) && native.setViewCalls == 2,
              "Invalid input or nonfinite angle reached the native setter");
        native.failView = true;
        saved = restore;
        Check(!ReadViewAngles(input, saved) && saved.x == restore.x && saved.y == restore.y,
              "Camera getter exception changed the caller's saved view");
        native.failView = false; native.nullView = true;
        Check(!ReadViewAngles(input, saved), "Camera getter accepted a null native sample");
        native.nullView = false; native.view.x = NAN;
        Check(!ReadViewAngles(input, saved) && saved.x == restore.x,
              "Camera getter accepted nonfinite native angles");
        native.view = restore; native.failSetView = true;
        Check(!WriteViewAngles(input, Vec3{3, 5, 0}) && native.view.x == restore.x,
              "Camera setter exception escaped its runtime boundary");
        combat_access::api.view = combat_access::api.setView = 0;
        Check(!ReadViewAngles(input, saved) && !WriteViewAngles(input, restore),
              "Unresolved camera helper called a missing native function");
    }
    void ReadNativeShootEye()
    {
        Fixture native; Bind(native);
        native.shootEyeOffset={2,-3,-5};
        auto context=ReadContext(AddressOf(native.input));
        Check(context.valid && !context.shotTiming && context.eye.x==102 && context.eye.y==197 && context.eye.z==359,
              "ReadContext ignored the adjusted native shoot eye in favor of scene origin/view offset");
        Vec3 saved{1,2,3};
        Check(ReadShootPosition(AddressOf(native.pawn),saved) && saved.z==359,
              "Direct native shoot-eye helper did not return its output vector");
        const auto calls=native.shootEyeCalls;
        Put(native.services,0x38,AddressOf(native.enemyPawn));
        Check(!ReadShootPosition(AddressOf(native.pawn),saved) && native.shootEyeCalls==calls,
              "Shoot-eye helper invoked services belonging to a different pawn");
        Put(native.services,0x38,AddressOf(native.pawn));
        saved={1,2,3};native.failShootEye=true;
        Check(!ReadShootPosition(AddressOf(native.pawn),saved) && saved.x==1 && saved.z==3,
              "Native shoot-eye exception escaped or changed the caller's output");
        native.failShootEye=false;native.invalidShootEye=true;
        Check(!ReadShootPosition(AddressOf(native.pawn),saved),"Nonfinite native shoot eye accepted");
        native.invalidShootEye=false;native.nullShootEye=true;
        Check(!ReadShootPosition(AddressOf(native.pawn),saved),"Native shoot-eye output return mismatch accepted");
        native.nullShootEye=false;combat_access::api.shootPosition=0;
        Failure(ReadContext(AddressOf(native.input)),"Player shot eye position is unavailable.");
    }
    void ReadNativeBulletTrace()
    {
        Fixture native;
        Bind(native);
        const auto context = ReadContext(AddressOf(native.input));
        Check(context.valid, "Trace fixture has no valid runtime context");
        TraceData trace{};
        Check(PrepareTrace(context, native.traceDelta, trace) && native.filterCalls == 1 && native.setupCalls == 1,
              "Real native trace setup wrapper failed");
        NativeContact prepared{};
        std::memcpy(&prepared, trace.hitElements + sizeof(NativeContact), sizeof(prepared));
        Check(trace.numHits == 2 && prepared.penetrate == 1 && prepared.damage == 32,
              "Native setup contact data did not reach the caller");
        Check(ApplyBullet(context, trace) && native.bulletCalls == 1 && trace.numHits == 3 &&
              trace.hitArray == native.bulletContacts.data() && trace.arrayPointer == native.bulletElements.data() &&
              trace.capacity == 192 && trace.hitCapacity == 192 && trace.flags == 0 && trace.hitFlags == 0 &&
              native.bulletContacts[1].damage == 31 && native.bulletContacts[2].damage == 24,
              "Native penetration changed contact count or pointers without reaching caller");
        native.expectRetainedTrace = true;
        trace.numHits = trace.unknown1 = 0;
        Check(PrepareTrace(context, native.traceDelta, trace) && native.setupCalls == 2 &&
              ApplyBullet(context, trace) && native.bulletCalls == 2,
              "Consecutive native trace calls did not reuse their grown allocation");
        std::array<std::byte, sizeof(TraceData)> beforeCleanup{};
        std::memcpy(beforeCleanup.data(), &trace, sizeof(trace));
        combat_access::api.resetTrace = 0;
        Check(!ReleaseTrace(trace) && std::memcmp(beforeCleanup.data(), &trace, sizeof(trace)) == 0,
              "Missing native cleanup binding discarded owned grown arrays");
        combat_access::api.resetTrace = reinterpret_cast<Address>(ResetTrace);
        native.failReset = true;
        Check(!ReleaseTrace(trace) && native.resetCalls == 1 && std::memcmp(beforeCleanup.data(), &trace, sizeof(trace)) == 0,
              "Native cleanup failure discarded owned trace state");
        native.failReset = false;
        Check(ReleaseTrace(trace) && native.resetCalls == 2 && trace.arrayPointer == trace.elements.data() &&
              trace.hitArray == trace.hitElements && trace.capacity == 128 && trace.hitCapacity == 8 &&
              trace.flags == static_cast<int>(0x80000000) && trace.hitFlags == static_cast<int>(0x80000000) &&
              trace.unknown1 == 0 && trace.numHits == 0, "Native cleanup did not restore inline storage defaults");
        combat_access::api.resetTrace = 0;
        Check(ReleaseTrace(trace) && trace.arrayPointer == nullptr && trace.hitArray == nullptr,
              "Unowned inline trace could not reset without a cleanup binding");
        combat_access::api.resetTrace = reinterpret_cast<Address>(ResetTrace);
        native.expectRetainedTrace = false;
        native.failFilter = true;
        trace = {};
        Check(!PrepareTrace(context, native.traceDelta, trace) && native.setupCalls == 2,
              "Failed native filter continued into trace setup");
        native.failFilter = false;
        trace = {};
        Check(PrepareTrace(context, native.traceDelta, trace), "Native trace setup did not recover after failure");
        native.failBullet = true;
        Check(!ApplyBullet(context, trace), "Failed native penetration call reported success");
    }
    void ReadNativeShotgunSpread()
    {
        Fixture native;
        Bind(native);
        auto weapon = ReadContext(AddressOf(native.input)).weapon;
        weapon.bullets = 8;
        const auto spread = Spread(weapon, 8);
        Check(native.sampleCalls == 1 && spread.x == .125f && spread.y == -.25f,
              "Native shotgun spread did not receive full output arrays or return the first pellet");
        for (const auto invalidCount : {0, 65})
        {
            weapon.bullets = invalidCount;
            const auto invalid = Spread(weapon, 8);
            Check(native.sampleCalls == 1 && std::isnan(invalid.x) && std::isnan(invalid.y),
                  "Invalid native pellet count reached the sampler or became perfect accuracy");
        }
        weapon.bullets = 8;
        native.invalidLastPellet = true;
        auto invalid = Spread(weapon, 8);
        Check(native.sampleCalls == 2 && std::isnan(invalid.x) && std::isnan(invalid.y),
              "Nonfinite pellet beyond the first was accepted as valid native spread");
        native.invalidLastPellet = false;
        native.failSample = true;
        invalid = Spread(weapon, 8);
        Check(native.sampleCalls == 3 && std::isnan(invalid.x) && std::isnan(invalid.y),
              "Native spread exception escaped or became a valid hit chance sample");
    }
    void ReadNativeCommandCrc()
    {
        Fixture native;
        Bind(native);
        for (unsigned index = 0; index < native.crcBytes.size(); ++index) native.crcBytes[index] = static_cast<std::uint8_t>(index + 17);
        native.expectedCrcArena = AddressOf(native.data);
        Check(!(native.expectedCrcArena & 3), "Native CRC arena fixture is misaligned");
        Put(native.rawBase, 8, native.expectedCrcArena | Address{2});
        const auto slot = AddressOf(native.rawBase) + combat_bindings::kMessagePayload + offsetof(combat_bindings::BaseCommand, moveCrc);
        for (const auto size : {0, 15, 16, 64})
        {
            native.expectedCrcSize = size;
            Check(combat_access::NativeCrc(AddressOf(native.rawBase), slot, native.crcBytes.data(), size),
                  "Native CRC rejected the inline/borrowed string-size boundary");
        }
        std::array<Address, 1> arenaCell{native.expectedCrcArena};
        Put(native.rawBase, 8, reinterpret_cast<Address>(arenaCell.data()) | Address{1});
        native.expectedCrcSize = 16;
        Check(combat_access::NativeCrc(AddressOf(native.rawBase), slot, native.crcBytes.data(), 16) && native.crcCalls == 5,
              "Native CRC did not unwrap an indirect tagged arena");
        for (const auto size : {-1, 65})
            Check(!combat_access::NativeCrc(AddressOf(native.rawBase), slot, native.crcBytes.data(), size) && native.crcCalls == 5,
                  "Out-of-range CRC serialization size reached the native callback");
        Check(!combat_access::NativeCrc(AddressOf(native.rawBase), slot, nullptr, 1) && native.crcCalls == 5,
              "Null nonempty CRC payload reached the native callback");
        native.expectedCrcSize = 0;
        Check(combat_access::NativeCrc(AddressOf(native.rawBase), slot, nullptr, 0) && native.crcCalls == 6,
              "Empty CRC payload rejected a valid null source");
        native.tagOnlyCrc = true;
        Check(!combat_access::NativeCrc(AddressOf(native.rawBase), slot, nullptr, 0),"Tag-only native CRC allocation failure reported success");
        native.tagOnlyCrc = false;native.unreadableCrc = true;
        Check(!combat_access::NativeCrc(AddressOf(native.rawBase), slot, nullptr, 0),"Unreadable native CRC object reported success");
        native.unreadableCrc = false;
        Check(combat_access::NativeCrc(AddressOf(native.rawBase), slot, nullptr, 0),"Native CRC could not recover after allocation failure");
        const auto reused=memory::read<Address>(slot);
        Check(combat_access::NativeCrc(AddressOf(native.rawBase), slot, nullptr, 0)&&memory::read<Address>(slot)==reused,
              "Legitimate reuse of an existing native CRC object was rejected");
        native.failCrc = true;
        Check(!combat_access::NativeCrc(AddressOf(native.rawBase), slot, nullptr, 0) && native.crcCalls == 11,
              "Native CRC exception escaped its runtime boundary");
    }
    void PrepareEditableCommand(Fixture &native)
    {
        using namespace combat_bindings;
        native.inspectCrc=false;
        BaseCommand base{};Check(memory::copy(&base,AddressOf(native.rawBase)+kMessagePayload,sizeof(base)),"Read editable base failed");
        base.bits|=6;base.buttons=AddressOf(native.commandButtons);base.angles=AddressOf(native.commandAngles);
        Put(native.rawBase,kMessagePayload,base);
        Put(native.commandAngles,kMessagePayload,AnglePayload{7,0,10,25,0});
        Put(native.commandButtons,kMessagePayload,ButtonPayload{7,0,0,0,0});
        Put(native.command,kCommandButtons,ButtonState{});
        CommandProto proto{};Check(memory::copy(&proto,AddressOf(native.command)+kCommandProto,sizeof(proto)),"Read editable proto failed");
        proto.history={0,1,1,AddressOf(native.historyEntries)};
        Put(native.command,kCommandProto,proto);
        Put(native.historyEntries,0,1);Put(native.historyEntries,8,AddressOf(native.history));
        Put(native.history,kMessagePayload,HistoryHeader{1|kHistoryPlayerTickBit|kHistoryPlayerFractionBit,0,AddressOf(native.historyAngles)});
        Put(native.historyAngles,kMessagePayload,AnglePayload{7,0,10,25,0});
        Put(native.history,kMessagePayload+kHistoryPlayerTick,HistoryShotTiming{1200,.25f});
    }
    void ReadMatchedShootEye()
    {
        using namespace combat_bindings;
        Fixture native;Bind(native);PrepareEditableCommand(native);
        native.matchedEyeOffset={-16,8,-4};
        auto context=ReadContext(AddressOf(native.input));
        Check(context.valid && context.shotTiming && context.shotTiming->tick==1200 && context.shotTiming->fraction==.25f &&
              context.eye.x==84 && context.eye.y==208 && context.eye.z==360 && native.matchedEyeCalls==1 &&
              native.lastMatchedTiming.tick==1200 && native.lastMatchedTiming.fraction==.25f && !native.shootEyeCalls,
              "Context eye and history timestamp were not paired by the actual native shooting helper");
        Check(ApplyShot(context,{16,31,0},*context.shotTiming) && native.matchedEyeCalls==2,
              "Ordinary zero-roll shot did not revalidate the matched native eye and time");
        native.matchedEyeOffset.x-=1;
        Check(!ApplyShot(context,{16,31,0},*context.shotTiming) && native.crcCalls==1,
              "Changed time-matched eye authorized a stale shot direction");
        native.matchedEyeOffset.x+=1;
        Put(native.history,kMessagePayload+kHistoryPlayerFraction,.5f);
        Check(!ApplyShot(context,{16,31,0},*context.shotTiming) && native.crcCalls==1,
              "Changed history time authorized a stale native eye");
        Vec3 saved{1,2,3};
        for(const auto invalid: {HistoryShotTiming{0,0},HistoryShotTiming{1200,1},HistoryShotTiming{1200,NAN}})
            Check(!ReadShotPosition(AddressOf(native.pawn),invalid,saved),"Invalid timestamp reached native eye interpolation");
        Put(native.services,0x368,32);
        Check(!ReadShotPosition(AddressOf(native.pawn),{1200,.25f},saved),"Invalid native history head reached interpolation");
        Put(native.services,0x368,0);Put(native.services,0x36c,33);
        Check(!ReadShotPosition(AddressOf(native.pawn),{1200,.25f},saved),"Unbounded native history count reached interpolation");
        Put(native.services,0x36c,0);Put(native.pawn,0x680,std::uint32_t{0x10005});
        Check(!ReadShotPosition(AddressOf(native.pawn),{1200,.25f},saved),"Stale full controller serial reached interpolation");
        Put(native.pawn,0x680,std::uint32_t{0x8005});native.failMatchedEye=true;
        Check(!ReadShotPosition(AddressOf(native.pawn),{1200,.25f},saved) && saved.x==1,
              "Matched native eye exception escaped or changed caller output");
        native.failMatchedEye=false;native.invalidShootEye=true;
        Check(!ReadShotPosition(AddressOf(native.pawn),{1200,.25f},saved),"Nonfinite matched native eye accepted");
        native.invalidShootEye=false;native.nullShootEye=true;
        Check(!ReadShotPosition(AddressOf(native.pawn),{1200,.25f},saved),"Wrong native matched-eye return pointer accepted");
        native.nullShootEye=false;combat_access::api.matchedShootPosition=0;
        Failure(ReadContext(AddressOf(native.input)),"Player shot eye position is unavailable.");
    }
    void ReadTargetScan()
    {
        using namespace game_access;
        Fixture native;Bind(native);
        Put(native.page,3*0x70,AddressOf(native.enemyController));Put(native.page,3*0x70+0x10,std::uint32_t{0x8003});
        Put(native.page,4*0x70,AddressOf(native.enemyPawn));Put(native.page,4*0x70+0x10,std::uint32_t{0x8004});
        Put(native.enemyController,0x300,std::uint32_t{0x8004});
        Put(native.enemyPawn,field::C_BaseEntity::m_iHealth,100);
        Put(native.enemyPawn,field::C_BaseEntity::m_iTeamNum,std::uint8_t{3});
        Put(native.enemyPawn,field::C_BaseEntity::m_lifeState,std::uint8_t{});
        Put(native.enemyPawn,field::C_BaseEntity::m_pGameSceneNode,AddressOf(native.enemyScene));
        Put(native.enemyPawn,0x728,AddressOf(native.enemyItems));
        native.enemyHitboxes.count=2;
        auto context=ReadContext(AddressOf(native.input));
        TargetScan scan;auto targets=ReadTargets(context,&scan);
        Check(targets.size()==1&&scan.controllers==1&&scan.pawns==1&&scan.enemies==1&&scan.eligibleEnemies==1&&scan.hitboxPlayers==1&&scan.hitboxCount==2,
              "Valid remote target did not pass all reported scan gates");
        Put(native.page,3*0x70,Address{});
        Put(native.page,130*0x70,AddressOf(native.enemyController));Put(native.page,130*0x70+0x10,std::uint32_t{0x8082});
        ++context.commandNumber;
        native.cachedControllers={
            {AddressOf(native.pawn),0,130,systems::entities::type::player}, // Mismatched cached pointer first.
            {AddressOf(native.enemyController),0,130,systems::entities::type::player},
            {AddressOf(native.enemyController),0,130,systems::entities::type::player}, // Repeated valid index.
            {AddressOf(native.enemyController),0,131,systems::entities::type::player}, // Stale empty slot.
            {AddressOf(native.enemyController),0,3,systems::entities::type::player}}; // Fresh range owns low slots.
        targets=ReadTargets(context,&scan);
        Check(targets.size()==1&&scan.controllers==1&&scan.pawns==1&&scan.hitboxCount==2,"Independent controller discovery beyond64 was omitted or counted twice");
        Check(native.visualCacheReads==0,"Command discovery still depends on the ESP render cache");
        Put(native.page,130*0x70,Address{});targets=ReadTargets(context,&scan);
        Check(targets.empty()&&!scan.controllers,"Stale high-index cached controller pointer entered target scan");
        Put(native.page,3*0x70,AddressOf(native.enemyController));native.cachedControllers.clear();
        native.enemyHitboxes.count=0;targets=ReadTargets(context,&scan);
        Check(targets.size()==1&&!targets[0].hitboxes.count&&scan.enemies==1&&scan.eligibleEnemies==1&&!scan.hitboxPlayers&&!scan.hitboxCount,"Missing optional geometry removed an otherwise eligible enemy");
        for(const auto invalid:{-1,21}) {
            native.enemyHitboxes.count=invalid;targets=ReadTargets(context,&scan);
            Check(targets.size()==1&&!targets[0].hitboxes.count&&scan.eligibleEnemies==1&&!scan.hitboxPlayers,"Malformed hitbox count escaped target-reader bounds or removed the eligible enemy");
        }
        native.enemyHitboxes.count=2;Put(native.enemyPawn,0x700,true);targets=ReadTargets(context,&scan);
        Check(targets.empty()&&scan.enemies==1&&!scan.eligibleEnemies,"Immune enemy disappeared from live-enemy diagnostics");
        Put(native.enemyPawn,0x700,false);Put(native.enemyScene,0x200,true);targets=ReadTargets(context,&scan);
        Check(targets.empty()&&scan.enemies==1&&!scan.eligibleEnemies,"Dormant enemy disappeared from live-enemy diagnostics");
        Put(native.enemyScene,0x200,false);missingImmunityField=true;targets=ReadTargets(context,&scan);
        Check(targets.empty()&&scan.enemies==1&&!scan.eligibleEnemies,"Missing immunity schema bypassed target validation");
        missingImmunityField=false;Put(native.enemyPawn,field::C_BaseEntity::m_iTeamNum,std::uint8_t{2});targets=ReadTargets(context,&scan);
        Check(targets.empty()&&scan.pawns==1&&!scan.enemies,"Friendly pawn was counted as a live enemy");
        Put(native.enemyPawn,field::C_BaseEntity::m_iTeamNum,std::uint8_t{3});Put(native.enemyPawn,field::C_BaseEntity::m_iHealth,0);targets=ReadTargets(context,&scan);
        Check(targets.empty()&&scan.pawns==1&&!scan.enemies,"Dead pawn was counted as a live enemy");
        Put(native.enemyPawn,field::C_BaseEntity::m_iHealth,100);native.invalidPawnClass=true;targets=ReadTargets(context,&scan);
        Check(targets.empty()&&scan.controllers==1&&!scan.pawns,"Non-player pawn passed exact class validation");
        native.invalidPawnClass=false;Put(native.enemyController,0x300,std::uint32_t{0x10004});targets=ReadTargets(context,&scan);
        Check(targets.empty()&&scan.controllers==1&&!scan.pawns,"Stale full pawn handle passed target enumeration");
        Put(native.enemyController,0x300,std::uint32_t{0x8004});native.invalidControllerClass=true;targets=ReadTargets(context,&scan);
        Check(targets.empty()&&!scan.controllers,"Non-controller class entered target enumeration");
        auto invalid=context;invalid.valid=false;scan={1,1,1,1,1,1};targets=ReadTargets(invalid,&scan);
        Check(targets.empty()&&!scan.controllers&&!scan.pawns&&!scan.enemies&&!scan.eligibleEnemies&&!scan.hitboxPlayers&&!scan.hitboxCount,"Invalid context retained stale target diagnostics");
    }
    void ReconnectTargetDiscovery()
    {
        using namespace game_access;
        Fixture native;Bind(native);ResetTargets();
        std::array<std::byte,512*0x70> secondPage{}, replacementPage{}, finalPage{};
        Put(native.list,0x18,AddressOf(secondPage));
        Put(secondPage,(600&511)*0x70,AddressOf(native.enemyController));
        Put(secondPage,(600&511)*0x70+0x10,std::uint32_t{0x8000|600});
        Put(native.page,4*0x70,AddressOf(native.enemyPawn));Put(native.page,4*0x70+0x10,std::uint32_t{0x8004});
        Put(native.enemyController,0x300,std::uint32_t{0x8004});
        Put(native.enemyPawn,field::C_BaseEntity::m_iHealth,100);
        Put(native.enemyPawn,field::C_BaseEntity::m_iTeamNum,std::uint8_t{3});
        Put(native.enemyPawn,field::C_BaseEntity::m_pGameSceneNode,AddressOf(native.enemyScene));
        native.enemyHitboxes.count=1;
        auto context=ReadContext(AddressOf(native.input));TargetScan scan;
        auto targets=ReadTargets(context,&scan);
        Check(targets.empty()&&native.discoveryPageCopies==1,"Initial discovery exceeded its one-page budget");
        targets=ReadTargets(context,&scan);
        Check(targets.empty()&&native.discoveryPageCopies==1,"Repeated command rescanned every entity page");
        ++context.commandNumber;targets=ReadTargets(context,&scan);
        Check(targets.size()==1&&native.discoveryPageCopies==2&&!native.visualCacheReads,"High page was not independently discovered within the bounded command cycle");
        Put(native.list,0x18,AddressOf(replacementPage));
        Put(replacementPage,(700&511)*0x70,AddressOf(native.enemyController));
        Put(replacementPage,(700&511)*0x70+0x10,std::uint32_t{0x10000|700});
        targets=ReadTargets(context,&scan);
        Check(targets.size()==1&&native.discoveryPageCopies==3,"Replaced page in the same list was not detected immediately");
        Put(replacementPage,(700&511)*0x70+0x10,std::uint32_t{0x10000|701});
        targets=ReadTargets(context,&scan);
        Check(targets.empty(),"Mismatched controller-slot identity bypassed discovery");
        Put(replacementPage,(700&511)*0x70+0x10,std::uint32_t{0x10000|700});
        ++context.commandNumber;targets=ReadTargets(context,&scan);
        ++context.commandNumber;targets=ReadTargets(context,&scan);
        Check(targets.size()==1,"Controller allocation did not recover after a transient invalid identity");
        Put(native.page,4*0x70+0x10,std::uint32_t{0x10004});
        targets=ReadTargets(context,&scan);Check(targets.empty(),"Reused pawn slot accepted the previous connection's full serial");
        Put(native.enemyController,0x300,std::uint32_t{0x10004});
        targets=ReadTargets(context,&scan);Check(targets.size()==1&&targets[0].handle==0x10004,"Fresh pawn serial could not replace the old one");
        // Native allocated pages can extend beyond ESP's old index range.
        Put(native.list,0x10+63*8,AddressOf(finalPage));
        Put(finalPage,510*0x70,AddressOf(native.enemyController));
        Put(finalPage,510*0x70+0x10,std::uint32_t{0x18000|32766});
        Put(replacementPage,(700&511)*0x70,Address{});
        targets=ReadTargets(context,&scan);
        Check(targets.size()==1,"Last valid entity index was omitted or required an ESP snapshot");
        // Disconnect/rejoin can reuse both the list and controller allocations.
        ResetTargets();Put(native.list,0x10+63*8,Address{});Put(native.list,0x18,Address{});
        Put(native.page,130*0x70,AddressOf(native.enemyController));Put(native.page,130*0x70+0x10,std::uint32_t{0x18000|130});
        context.commandNumber=77;targets=ReadTargets(context,&scan);
        Check(targets.size()==1&&!native.visualCacheReads,"Same-pointer reconnect retained stale discovery or waited for render");
        Put(native.client,offset::dwEntityList,Address{});
        targets=ReadTargets(context,&scan);Check(targets.empty()&&!scan.controllers,"Old context survived loss of the current entity list");
        Put(native.client,offset::dwEntityList,AddressOf(native.list));
        targets=ReadTargets(context,&scan);Check(targets.size()==1,"Same-pointer list restoration did not rediscover current players");
        ResetTargets();
    }
    void HistoryTickDiscovery()
    {
        using namespace game_access;
        Fixture native;Bind(native);ResetTargets();
        std::array<std::byte,512*0x70> secondPage{},replacementPage{};
        Put(native.list,0x18,AddressOf(secondPage));
        Put(secondPage,(600&511)*0x70,AddressOf(native.enemyController));
        Put(secondPage,(600&511)*0x70+0x10,std::uint32_t{0x8000|600});
        Put(native.page,4*0x70,AddressOf(native.enemyPawn));
        Put(native.page,4*0x70+0x10,std::uint32_t{0x8004});
        Put(native.enemyController,0x300,std::uint32_t{0x8004});
        Put(native.enemyPawn,field::C_BaseEntity::m_iHealth,100);
        Put(native.enemyPawn,field::C_BaseEntity::m_iTeamNum,std::uint8_t{3});
        Put(native.enemyPawn,field::C_BaseEntity::m_pGameSceneNode,AddressOf(native.enemyScene));
        native.enemyHitboxes.count=1;
        auto local=ReadLocalPlayer(AddressOf(native.client));
        auto targets=ReadHistoryTargets(local,false);
        Check(targets.empty()&&native.discoveryPageCopies==1,"History discovery exceeded its initial page budget");
        for(int repeat=0;repeat<240;++repeat)targets=ReadHistoryTargets(local,false);
        Check(targets.empty()&&native.discoveryPageCopies==1,"Repeated render frames rescanned pages within one local tick");
        Put(native.controller,0x200,1101);targets=ReadHistoryTargets(local,false);
        Check(targets.size()==1&&native.discoveryPageCopies==2&&!targets[0].hitboxes.count&&!native.visualCacheReads,
              "Advancing history tick failed to discover a high-page enemy without geometry or ESP");
        for(int repeat=0;repeat<240;++repeat)targets=ReadHistoryTargets(local,false);
        Check(targets.size()==1&&native.discoveryPageCopies==2,"Known history enemy triggered repeated page scans");
        Put(native.page,4*0x70+0x10,std::uint32_t{0x10004});
        targets=ReadHistoryTargets(local,false);
        Check(targets.empty()&&native.discoveryPageCopies==2,"Cached history accepted a stale pawn serial within the same tick");
        Put(native.enemyController,0x300,std::uint32_t{0x10004});
        targets=ReadHistoryTargets(local,false);
        Check(targets.size()==1&&targets[0].handle==0x10004&&native.discoveryPageCopies==2,
              "Current pawn serial was not revalidated without rescanning pages");
        Put(native.enemyScene,0x200,true);targets=ReadHistoryTargets(local,false);
        Check(targets.empty()&&native.discoveryPageCopies==2,"History cache bypassed live dormant eligibility");
        Put(native.enemyScene,0x200,false);
        native.invalidControllerClass=true;targets=ReadHistoryTargets(local,false);
        Check(targets.empty()&&native.discoveryPageCopies==2,"History cache bypassed live controller type validation");
        native.invalidControllerClass=false;
        Put(native.list,0x18,AddressOf(replacementPage));
        Put(replacementPage,(700&511)*0x70,AddressOf(native.enemyController));
        Put(replacementPage,(700&511)*0x70+0x10,std::uint32_t{0x10000|700});
        targets=ReadHistoryTargets(local,false);
        Check(targets.size()==1&&native.discoveryPageCopies==3,"History missed a changed entity page within the same tick");
        Put(native.controller,0x200,10);targets=ReadHistoryTargets(local,false);
        Check(targets.empty()&&native.discoveryPageCopies==4,"Tick rollback retained stale history discovery state");
        Put(native.controller,0x200,11);targets=ReadHistoryTargets(local,false);
        Check(targets.size()==1&&native.discoveryPageCopies==5,"Tick rollback did not resume bounded history discovery");
        Put(native.client,offset::dwEntityList,Address{});targets=ReadHistoryTargets(local,false);
        Check(targets.empty()&&native.discoveryPageCopies==5,"Lost local list retained cached history targets");
        Put(native.client,offset::dwEntityList,AddressOf(native.list));
        targets=ReadHistoryTargets(local,false);
        Check(targets.empty()&&native.discoveryPageCopies==6,"Restored list did not reset discovery at the same tick");
        Put(native.controller,0x200,12);targets=ReadHistoryTargets(local,false);
        Check(targets.size()==1&&native.discoveryPageCopies==7,"Restored list failed to rediscover the current high-page enemy");
        for(int unavailable=0;unavailable<3;++unavailable) {
            ResetTargets();const auto before=native.discoveryPageCopies;
            Put(native.controller,0x200,unavailable==1?-1:0);missingTickField=unavailable==2;
            targets=ReadHistoryTargets(local,false);
            Check(targets.empty()&&native.discoveryPageCopies==before+1,"Unavailable tick violated history page budget");
            targets=ReadHistoryTargets(local,false);
            Check(targets.size()==1&&native.discoveryPageCopies==before+2,
                  "Unavailable tick stalled synthetic history discovery progress");
        }
        missingTickField=false;ResetTargets();
        std::cout<<"PASS: history discovery scans one page per local tick, revalidates cached identities/eligibility, and recovers after rollback/list loss or unavailable tick\n";
    }
    void ApplyCurrentCommand()
    {
        using namespace combat_bindings;
        Fixture native;Bind(native);PrepareEditableCommand(native);
        const auto context=ReadContext(AddressOf(native.input));
        Check(ApplyCommand(context,Vec3{15,30,0},true),"Current ready command did not commit");
        const auto buttons=memory::read<ButtonPayload>(AddressOf(native.commandButtons)+kMessagePayload);
        Check((buttons.value&kAttack)&&(buttons.changed&kAttack)&&(buttons.scroll&kAttack)&&native.crcCalls==1,
              "Ready command failed to commit all three attack states and checksum");
        Check(ApplyShot(context,{16,31,12},{1200,.25f}),"Current timed shot did not commit");
        const auto angle=memory::read<AnglePayload>(AddressOf(native.historyAngles)+kMessagePayload);
        Check(angle.pitch==16&&angle.yaw==31&&angle.roll==12&&native.crcCalls==2,"Timed shot lost its history angle/roll");
        for(int mode=0;mode<13;++mode)
        {
            Fixture stale;Bind(stale);PrepareEditableCommand(stale);
            const auto previous=ReadContext(AddressOf(stale.input));
            switch(mode)
            {
            case 0:Put(stale.pawn,game_access::field::C_BaseEntity::m_iHealth,0);break;
            case 1:Put(stale.pawn,game_access::field::C_BaseEntity::m_iTeamNum,std::uint8_t{3});break;
            case 2:Put(stale.services,game_access::field::CPlayer_WeaponServices::m_hActiveWeapon,std::uint32_t{0x8001});break;
            case 3:Put(stale.page,2*0x70+0x10,std::uint32_t{0x10002});break;
            case 4:Put(stale.weapon,0x120,0);break;
            case 5:Put(stale.weapon,0x170,true);break;
            case 6:Put(stale.weapon,0x124,1201);break;
            case 7:Put(stale.rawBase,kMessagePayload+offsetof(BaseCommand,clientTick),1198);break;
            case 8:Put(stale.scene,0x100,Vec3{101,200,300});break;
            case 9:stale.nullCommand=true;break;
            case 10:Put(stale.weapon,game_access::field::C_BaseEntity::m_nSubclassID+8,AddressOf(stale.scene));break;
            case 11:Put(stale.client,game_access::offset::dwLocalPlayerController,AddressOf(stale.pawn));break;
            case 12:Put(stale.command,kCommandSequence,78);break;
            }
            Check(!ApplyCommand(previous,Vec3{15,30,0},true)&&!ApplyShot(previous,{15,30,0},{1200,.25f})&&stale.crcCalls==0,
                  "A stale local/weapon/readiness/eye/command state committed a shot");
            Check(!memory::read<ButtonState>(AddressOf(stale.command)+kCommandButtons).value,"Rejected stale command modified its attack buttons");
        }
    }
    void ReadShotReadiness()
    {
        using namespace combat_bindings;
        Check(!ReadyForShot(Context{}), "An unavailable context authorized stopping for a shot");
        for (int mode = 0; mode < 6; ++mode) {
            Fixture native; Bind(native); PrepareEditableCommand(native);
            auto context = ReadContext(AddressOf(native.input));
            Check(context.valid && context.canShoot, "Readiness fixture did not acquire a ready firearm");
            switch (mode) {
            case 1: Put(native.services, game_access::field::CPlayer_WeaponServices::m_hActiveWeapon, std::uint32_t{0x8001}); break;
            case 2: Put(native.command, kCommandSequence, 78); break;
            case 3: native.matchedEyeOffset.x = 1; break;
            case 4: Put(native.weapon, 0x124, 1201); break;
            case 5: context.canShoot = false; break;
            }
            const auto command = native.command;
            const auto base = native.rawBase;
            const auto buttons = native.commandButtons;
            const auto angles = native.commandAngles;
            const auto history = native.history;
            const auto historyAngles = native.historyAngles;
            Check(ReadyForShot(context) == (mode == 0),
                  "Readiness accepted a replaced weapon/command/eye or a cooling-down firearm");
            Check(!native.crcCalls && native.command == command && native.rawBase == base &&
                  native.commandButtons == buttons && native.commandAngles == angles &&
                  native.history == history && native.historyAngles == historyAngles,
                  "Read-only shot readiness modified command, buttons, angles, history or checksum");
            if (mode == 0)
                Check(native.matchedEyeCalls == 2, "Shot readiness did not freshly verify the time-matched eye");
        }
    }

    void ScopeReadyFirearms()
    {
        using namespace combat_bindings;
        constexpr std::uint64_t secondary = 1ull << 11;
        constexpr auto definitionOffset = game_access::field::C_EconEntity::m_AttributeManager +
            game_access::field::C_AttributeContainer::m_Item + game_access::field::C_EconItemView::m_iItemDefinitionIndex;
        for (const std::uint16_t definition : {9,40,11,38,8,39}) {
            Fixture native; Bind(native); PrepareEditableCommand(native);
            Put(native.weapon, definitionOffset, definition);
            const auto context = ReadContext(AddressOf(native.input));
            const auto beforeHistory = native.history;
            const auto beforeAngles = native.commandAngles;
            Check(IsScoped(context) == std::optional{false} && Scope(context),
                  "Supported unscoped firearm did not issue a ready scope action");
            const auto raw = memory::read<ButtonState>(AddressOf(native.command) + kCommandButtons);
            const auto payload = memory::read<ButtonPayload>(AddressOf(native.commandButtons) + kMessagePayload);
            const auto proto = memory::read<CommandProto>(AddressOf(native.command) + kCommandProto);
            Check(raw.value == secondary && raw.changed == secondary && raw.scroll == secondary &&
                  payload.value == secondary && (proto.bits & 0x40) && proto.attack2History == 0 &&
                  !(proto.bits & 0x20) && native.crcCalls == 1,
                  "Scope action fired primary or omitted secondary history/checksum");
            Check(native.history == beforeHistory && native.commandAngles == beforeAngles,
                  "Scope action changed existing aiming or shot timing");
        }
        for (int mode = 0; mode < 15; ++mode) {
            Fixture native; Bind(native); PrepareEditableCommand(native);
            Put(native.weapon, definitionOffset, std::uint16_t{9});
            auto context = ReadContext(AddressOf(native.input));
            switch (mode) {
            case 0: context.weapon.definition = 7; break;
            case 1: context.weapon.type = 0; break;
            case 2: Put(native.pawn, 0x772, true); break;
            case 3: missingScopeField = true; break;
            case 4: missingSecondaryField = true; break;
            case 5: Put(native.weapon, 0x128, 1201); break;
            case 6: Put(native.weapon, 0x128, -1); break;
            case 7: context.canShoot = false; break;
            case 8: Put(native.data, 0x100, 0); break;
            case 9: Put(native.services, game_access::field::CPlayer_WeaponServices::m_hActiveWeapon, std::uint32_t{0x8001}); break;
            case 10: Put(native.pawn, game_access::field::C_BaseEntity::m_iHealth, 0); break;
            case 11: Put(native.weapon, 0x124, 1201); break;
            case 12: Put(native.scene, 0x100, Vec3{101,200,300}); break;
            case 13: Put(native.command, kCommandSequence, 78); break;
            case 14: Put(native.weapon, definitionOffset, std::uint16_t{7}); break;
            }
            const auto command = native.command;
            const auto base = native.rawBase;
            const auto buttons = native.commandButtons;
            Check(!Scope(context) && !native.crcCalls && native.command == command && native.rawBase == base &&
                  native.commandButtons == buttons,
                  "Unknown/nonfirearm/scoped/cooldown/stale scope state modified a command");
        }
    }

    void ReadAndApplyOrientation()
    {
        using namespace combat_bindings;
        Fixture native;Bind(native);PrepareEditableCommand(native);
        Put(native.weapon,0x124,9000);Put(native.weapon,0x120,0);Put(native.weapon,0x170,true);
        auto context=ReadOrientationContext(AddressOf(native.input));
        Check(context.valid && context.local.handle==0x8001 && context.commandNumber==77 && context.tick==1100 &&
              context.angles.x==10 && context.angles.y==25,"Orientation context used firearm cooldown, reload or ammo readiness");
        const auto historyBefore=native.history;const auto historyAnglesBefore=native.historyAngles;
        const auto cameraBefore=native.view;
        const auto shotsBefore=native.shootEyeCalls+native.matchedEyeCalls;
        Check(ApplyOrientation(context,{89,115,0}) && native.crcCalls==1,"Current nonshooting orientation did not commit");
        const auto angle=memory::read<AnglePayload>(AddressOf(native.commandAngles)+kMessagePayload);
        const auto buttons=memory::read<ButtonState>(AddressOf(native.command)+kCommandButtons);
        Check(angle.pitch==89 && angle.yaw==115 && !angle.roll && !(buttons.value&kAttack) && !(buttons.changed&kAttack) && !(buttons.scroll&kAttack),
              "Orientation lost base angles or synthesized an attack");
        Check(native.history==historyBefore && native.historyAngles==historyAnglesBefore && native.view.x==cameraBefore.x &&
              native.view.y==cameraBefore.y && native.shootEyeCalls+native.matchedEyeCalls==shotsBefore,
              "Orientation changed shot history, camera input or evaluated a shot eye");
        for(const auto type: {0,8,9,11}) {
            Put(native.data,0x100,type);context=ReadOrientationContext(AddressOf(native.input));
            Check(context.valid&&context.weaponType==type,"Nongun active weapon unnecessarily disabled orientation");
        }
        Put(native.data,0x100,3);
        for(const auto action: {kAttack, std::uint64_t{1ull<<11}, std::uint64_t{1ull<<5}})
            for(int field=0;field<3;++field) {
                Fixture manual;Bind(manual);PrepareEditableCommand(manual);
                auto raw=memory::read<ButtonState>(AddressOf(manual.command)+kCommandButtons);
                if(field==0)raw.value=action;else if(field==1)raw.changed=action;else raw.scroll=action;
                Put(manual.command,kCommandButtons,raw);
                Check(!ReadOrientationContext(AddressOf(manual.input)).valid&&manual.crcCalls==0,
                      "Manual raw button action allowed orientation");
                raw={};Put(manual.command,kCommandButtons,raw);
                auto pb=memory::read<ButtonPayload>(AddressOf(manual.commandButtons)+kMessagePayload);
                if(field==0)pb.value=action;else if(field==1)pb.changed=action;else pb.scroll=action;
                Put(manual.commandButtons,kMessagePayload,pb);
                Check(!ReadOrientationContext(AddressOf(manual.input)).valid,"Manual protobuf action allowed orientation");
            }
        for(const auto action: {kAttack,std::uint64_t{1ull<<11},std::uint64_t{1ull<<5}}) {
            Fixture manual;Bind(manual);PrepareEditableCommand(manual);
            auto base=memory::read<BaseCommand>(AddressOf(manual.rawBase)+kMessagePayload);
            base.subticks={0,1,1,AddressOf(manual.orientationSubtickEntries)};Put(manual.rawBase,kMessagePayload,base);
            Put(manual.orientationSubtickEntries,0,1);Put(manual.orientationSubtickEntries,8,AddressOf(manual.orientationSubtick));
            SubtickPayload move{};move.bits=3;move.button=action;move.pressed=true;
            Put(manual.orientationSubtick,kMessagePayload,move);
            Check(!ReadOrientationContext(AddressOf(manual.input)).valid,"Pressed manual subtick action allowed orientation");
        }
        for(int mode=0;mode<17;++mode) {
            Fixture blocked;Bind(blocked);PrepareEditableCommand(blocked);
            const auto previous=ReadOrientationContext(AddressOf(blocked.input));Check(previous.valid,"Orientation guard fixture invalid before mutation");
            switch(mode) {
            case 0:Put(blocked.pawn,0x760,std::uint8_t{7});break;
            case 1:Put(blocked.pawn,0x760,std::uint8_t{9});break;
            case 2:Put(blocked.pawn,0x764,std::uint32_t{1u<<6});break;
            case 3:Put(blocked.pawn,0x770,true);break;
            case 4:Put(blocked.pawn,0x771,true);break;
            case 5:Put(blocked.rules,0x40,true);break;
            case 6:Put(blocked.rules,0x100,true);break;
            case 7:Put(blocked.rules,0x84,4);break;
            case 8:Put(blocked.rules,0x70,21.f);break;
            case 9:Put(blocked.data,0x100,9);Put(blocked.weapon,0x1a0,1.f);break;
            case 10:Put(blocked.input,kInputWeaponSelection,std::uint32_t{0x8002});break;
            case 11:Put(blocked.pawn,game_access::field::C_BaseEntity::m_iHealth,0);break;
            case 12:Put(blocked.services,game_access::field::CPlayer_WeaponServices::m_hActiveWeapon,std::uint32_t{0x8001});break;
            case 13:Put(blocked.command,kCommandSequence,78);break;
            case 14:missingOrientationField=true;break;
            case 15:blocked.rulesSlot[0]=0;break;
            case 16:Put(blocked.command,kCommandButtons,ButtonState{0,kAttack,0,0});break;
            }
            Check(!ApplyOrientation(previous,{89,115,0})&&blocked.crcCalls==0,
                  "Stale player/action/weapon/round state committed an orientation command");
        }
        Fixture staleCamera;Bind(staleCamera);PrepareEditableCommand(staleCamera);
        const auto previous=ReadOrientationContext(AddressOf(staleCamera.input));staleCamera.view.y+=1;
        Check(!ApplyOrientation(previous,{89,115,0}),"Changed visible camera authorized a stale orientation");
        Check(!ReadOrientationContext(AddressOf(staleCamera.input)+8).valid,"Orientation accepted a different input object");
        Check(!ApplyOrientation(previous,{90,115,0})&&!ApplyOrientation(previous,{NAN,115,0}),"Invalid orientation pitch reached command editing");
    }
    void TargetRelativeOrientation()
    {
        using namespace game_access;using namespace combat_bindings;
        Fixture native;Bind(native);PrepareEditableCommand(native);ResetTargets();
        Put(native.page,3*0x70,AddressOf(native.enemyController));Put(native.page,3*0x70+0x10,std::uint32_t{0x8003});
        Put(native.page,4*0x70,AddressOf(native.enemyPawn));Put(native.page,4*0x70+0x10,std::uint32_t{0x8004});
        Put(native.enemyController,0x300,std::uint32_t{0x8004});
        Put(native.enemyPawn,field::C_BaseEntity::m_iHealth,100);Put(native.enemyPawn,field::C_BaseEntity::m_iTeamNum,std::uint8_t{3});
        Put(native.enemyPawn,field::C_BaseEntity::m_pGameSceneNode,AddressOf(native.enemyScene));
        Put(native.enemyScene,0x100,Vec3{200,200,300});
        native.enemyHitboxes.count=0;
        auto context=ReadOrientationContext(AddressOf(native.input));
        const auto target=ReadOrientationTarget(context);
        Check(target&&target->handle==0x8004&&target->pawn==AddressOf(native.enemyPawn)&&target->yaw==0,
              "Real orientation reference required custom-model hitboxes or failed native full-handle discovery");
        const auto history=native.history;const auto historyAngles=native.historyAngles;const auto camera=native.view;
        hvh_antiaim::Options options;options.yaw=hvh_antiaim::YawMode::Backward;
        const auto angles=hvh_antiaim::Angles({context.angles.x,context.angles.y,context.angles.z},context.tick,options,1.f/64.f,target->yaw);
        Check(angles&&ApplyOrientation(context,{angles->x,angles->y,angles->z},&*target),"Target-relative backwards command failed to commit");
        auto applied=memory::read<AnglePayload>(AddressOf(native.commandAngles)+kMessagePayload);
        Check(std::abs(applied.yaw)==180&&native.history==history&&native.historyAngles==historyAngles&&native.view.x==camera.x&&native.view.y==camera.y,
              "Backwards target yaw altered the visible camera or shot history");
        const auto rejected=[&](const char* message){const auto crc=native.crcCalls;
            Check(!ApplyOrientation(context,{89,180,0},&*target)&&native.crcCalls==crc,message);};
        Put(native.enemyScene,0x100,Vec3{100,300,300});rejected("Moved enemy committed an obsolete backwards direction");
        const auto moved=ReadOrientationTarget(context);Check(moved&&moved->yaw==90,"Moving target did not update body-facing bearing");
        Put(native.enemyScene,0x100,Vec3{200,200,300});
        Put(native.scene,0x100,Vec3{100,199,300});rejected("Moved local player committed an obsolete target bearing");Put(native.scene,0x100,Vec3{100,200,300});
        Put(native.page,4*0x70+0x10,std::uint32_t{0x10004});rejected("Reused enemy slot inherited prior backwards target");
        Check(!ReadOrientationTarget(context),"Unresolved full target serial selected again");Put(native.page,4*0x70+0x10,std::uint32_t{0x8004});
        Put(native.enemyPawn,field::C_BaseEntity::m_iHealth,0);rejected("Dead enemy remained an orientation reference");
        Check(!ReadOrientationTarget(context),"Dead enemy rediscovered for backwards mode");Put(native.enemyPawn,field::C_BaseEntity::m_iHealth,100);
        Put(native.enemyPawn,field::C_BaseEntity::m_iTeamNum,std::uint8_t{2});rejected("Friendly target retained backwards selection");Put(native.enemyPawn,field::C_BaseEntity::m_iTeamNum,std::uint8_t{3});
        Put(native.enemyScene,0x200,true);rejected("Dormant target retained backwards selection");Put(native.enemyScene,0x200,false);
        Put(native.enemyPawn,0x700,true);rejected("Spawn-protected target retained backwards selection");Put(native.enemyPawn,0x700,false);
        Put(native.client,offset::dwEntityList,Address{});rejected("Disconnected context retained backwards target");
        Check(!ReadOrientationTarget(context),"Missing entity list reused an orientation reference");ResetTargets();
    }
    void OrientationDuringFire()
    {
        using namespace combat_bindings;
        Fixture native;Bind(native);PrepareEditableCommand(native);
        const auto context=ReadContext(AddressOf(native.input));
        const auto timing=ShotTiming(context);Check(timing.has_value(),"Shooting orientation fixture missing timing");
        const Vec3 aim{15,30,7};
        Check(ApplyShot(context,aim,*timing),"Shooting orientation fixture did not commit a shot");
        const CommittedShot shot{context.command,context.commandNumber,context.local.handle,context.weapon.handle,*timing,aim};
        const auto history=native.history;
        const auto historyAngles=native.historyAngles;
        const auto proto=memory::read<CommandProto>(AddressOf(native.command)+kCommandProto);
        const auto buttons=memory::read<ButtonState>(AddressOf(native.command)+kCommandButtons);
        Check(!ReadOrientationContext(AddressOf(native.input)).valid,"Unqualified ready manual shot permitted orientation");
        const auto orientation=ReadOrientationContext(AddressOf(native.input),&shot);
        Check(orientation.valid && orientation.shot && ApplyOrientation(orientation,{89,-160,0}),
              "Validated HVH shot could not retain anti aim");
        const auto baseAngles=memory::read<AnglePayload>(AddressOf(native.commandAngles)+kMessagePayload);
        const auto after=memory::read<CommandProto>(AddressOf(native.command)+kCommandProto);
        const auto afterButtons=memory::read<ButtonState>(AddressOf(native.command)+kCommandButtons);
        Check(baseAngles.pitch==89 && baseAngles.yaw==-160 && native.history==history && native.historyAngles==historyAngles &&
              after.attack1History==proto.attack1History && afterButtons.value==buttons.value &&
              (afterButtons.changed&kAttack)==(buttons.changed&kAttack) && (afterButtons.scroll&kAttack)==(buttons.scroll&kAttack),
              "Anti aim altered the committed shot history, roll, timestamp or attack");
        Put(native.historyAngles,kMessagePayload,AnglePayload{7,0,15,31,7});
        Check(!ApplyOrientation(orientation,{89,100,0}),"Changed shot aim still authorized orientation");
        Put(native.historyAngles,kMessagePayload,AnglePayload{7,0,15,30,7});
        for(int field=0;field<7;++field) {
            auto stale=shot;
            switch(field) {
            case 0:++stale.command;break;
            case 1:++stale.sequence;break;
            case 2:stale.localHandle+=0x8000;break;
            case 3:stale.weaponHandle+=0x8000;break;
            case 4:++stale.timing.tick;break;
            case 5:stale.timing.fraction=.5f;break;
            case 6:stale.angles.z=0;break;
            }
            Check(!ReadOrientationContext(AddressOf(native.input),&stale).valid,"Stale shot identity/history authorized orientation");
        }
        for(int source=0;source<3;++source) {
            Fixture cooldown;Bind(cooldown);PrepareEditableCommand(cooldown);
            Put(cooldown.weapon,0x124,9000);
            if(source==0) Put(cooldown.command,kCommandButtons,ButtonState{0,kAttack,kAttack,kAttack});
            if(source==1) Put(cooldown.commandButtons,kMessagePayload,ButtonPayload{7,0,kAttack,kAttack,kAttack});
            if(source==2) {
                auto base=memory::read<BaseCommand>(AddressOf(cooldown.rawBase)+kMessagePayload);
                base.subticks={0,1,1,AddressOf(cooldown.orientationSubtickEntries)};Put(cooldown.rawBase,kMessagePayload,base);
                Put(cooldown.orientationSubtickEntries,0,1);Put(cooldown.orientationSubtickEntries,8,AddressOf(cooldown.orientationSubtick));
                SubtickPayload move{};move.bits=3;move.button=kAttack;move.pressed=true;
                Put(cooldown.orientationSubtick,kMessagePayload,move);
            }
            const auto held=ReadOrientationContext(AddressOf(cooldown.input));
            Check(held.valid&&ApplyOrientation(held,{89,100,0}),"Holding fire throughout cooldown disabled anti aim");
            Put(cooldown.weapon,0x124,1000);
            Check(!ApplyOrientation(held,{89,120,0}),"Cooldown becoming ready redirected a manual shot");
            Put(cooldown.weapon,0x170,true);
            Check(ReadOrientationContext(AddressOf(cooldown.input)).valid,"Held primary during reload disabled anti aim");
            Put(cooldown.weapon,0x170,false);Put(cooldown.weapon,0x120,0);
            Check(ReadOrientationContext(AddressOf(cooldown.input)).valid,"Held primary with empty clip disabled anti aim");
            Put(cooldown.data,0x100,0);
            Check(!ReadOrientationContext(AddressOf(cooldown.input)).valid,"Knife action was treated as firearm cooldown");
        }
    }
    void ReadShotTimingAndSeed()
    {
        using namespace combat_bindings;
        Fixture native;Bind(native);PrepareEditableCommand(native);
        const auto context=ReadContext(AddressOf(native.input));
        auto timing=ShotTiming(context);Check(timing&&timing->tick==1200&&timing->fraction==.25f,"Present native shot timing was lost");
        Put(native.history,kMessagePayload,std::uint32_t{1|kHistoryPlayerTickBit});
        Put(native.history,kMessagePayload+kHistoryPlayerFraction,std::numeric_limits<float>::quiet_NaN());
        timing=ShotTiming(context);Check(timing&&timing->fraction==0,"Absent optional shot fraction read its uninitialized field");
        Put(native.history,kMessagePayload,std::uint32_t{1|kHistoryPlayerTickBit|kHistoryPlayerFractionBit});
        Check(!ShotTiming(context),"Nonfinite present shot fraction accepted");
        Put(native.history,kMessagePayload,std::uint32_t{1});Check(!ShotTiming(context),"Shot timing invented an absent player tick");
        const auto seed=SpreadSeed({10,25,3},1200);Check(seed&&*seed==0&&native.seedCalls==1,"Valid zero native seed became unavailable");
        Check(!SpreadSeed({10,25,3},0)&&!SpreadSeed({std::numeric_limits<float>::quiet_NaN(),25,3},1200)&&native.seedCalls==1,"Invalid seed input reached native helper");
        native.failSeed=true;Check(!SpreadSeed({10,25,3},1200)&&native.seedCalls==2,"Native seed exception escaped or became seed zero");
        combat_access::api.spreadSeed=0;Check(!SpreadSeed({10,25,3},1200),"Absent seed helper became seed zero");
    }
}

// Native memory and schemas are the fixture boundaries; ReadContext and entity resolution are production code.
namespace memory
{
    bool copy(void *out, std::uintptr_t address, std::size_t size) noexcept {
        if(fixture&&size==512*0x70)++fixture->discoveryPageCopies;
        return game_access::Copy(address, out, size);
    }
    bool store(std::uintptr_t address, const void *value, std::size_t size) noexcept { return game_access::Store(address, value, size); }
    std::uintptr_t resolve_cached(std::string_view expression) {
        if (!initializeBindings || expression == absentBinding) return 0;
        namespace p = combat_bindings::patterns;
        if (expression == p::csgo_input) return AddressOf(fixture->input);
        if (expression == p::get_usercmd_base) return reinterpret_cast<Address>(GetCommandBase);
        if (expression == p::get_usercmd) return reinterpret_cast<Address>(GetCommand);
        if (expression == p::get_view_angles) return reinterpret_cast<Address>(GetView);
        if (expression == p::set_view_angles) return reinterpret_cast<Address>(SetView);
        return 1; // Other initialization-only functions are never called in this test.
    }
    std::string read_string(std::uintptr_t address, std::size_t maximum)
    {
        std::string result;
        for (std::size_t index = 0; index < maximum; ++index)
        {
            const auto next = safe_read<char>(address + index);
            if (!next || !*next) break;
            result.push_back(*next);
        }
        return result;
    }
}
namespace systems
{
    std::uint32_t schemas::lookup(const char *owner, std::uint32_t hash)
    {
        if (missingAccuracyField && hash == fnv1a::runtime_hash("m_flTurningInaccuracyDelta")) return 0;
        if (missingImmunityField && hash == fnv1a::runtime_hash("m_bGunGameImmunity")) return 0;
        if (missingOrientationField && hash == fnv1a::runtime_hash("m_nActualMoveType")) return 0;
        if (missingScopeField && hash == fnv1a::runtime_hash("m_bIsScoped")) return 0;
        if (missingSecondaryField && hash == fnv1a::runtime_hash("m_nNextSecondaryAttackTick")) return 0;
        if (missingTickField && hash == fnv1a::runtime_hash("m_nTickBase")) return 0;
        for (const auto &field : fields)
            if (std::string_view(owner) == field.owner && hash == fnv1a::runtime_hash(field.name)) return field.offset;
        return 0;
    }
    const char *entities::get_schema_name(std::uintptr_t entity) const {
        if(!fixture)return nullptr;
        if(entity==AddressOf(fixture->enemyController))return fixture->invalidControllerClass?"C_BaseEntity":"CCSPlayerController";
        if(entity==AddressOf(fixture->enemyPawn))return fixture->invalidPawnClass?"C_CSObserverPawn":"C_CSPlayerPawn";
        return nullptr;
    }
    std::vector<entities::cached> entities::get_by_type(type value) const {
        if(fixture)++fixture->visualCacheReads;
        return fixture&&value==type::player?fixture->cachedControllers:std::vector<cached>{};
    }
    std::array<bones::data, 27> bones::get_skeleton(std::uintptr_t) { return {}; }
}
namespace combat_access { Hitboxes ReadHitboxes(Address scene, bool) {
    return fixture&&scene==AddressOf(fixture->enemyScene)?fixture->enemyHitboxes:Hitboxes{};
} }
namespace game { bool module_matches(const char *, std::string_view) { return compatibleClient; } }
namespace math
{
    vector3 vector3::operator+(const vector3 &value) const noexcept { return {x + value.x, y + value.y, z + value.z}; }
    quaternion::quaternion() noexcept : x{}, y{}, z{}, w{1} {}
}
int main()
{
    try
    {
        InputInitializationIndependence();
        ReadNativeContext();
        ReadNativeCameraAndIdentity();
        ReadNativeShootEye();
        ReadNativeBulletTrace();
        ReadNativeShotgunSpread();
        ReadNativeCommandCrc();
        ReadMatchedShootEye();
        ReadTargetScan();
        ReconnectTargetDiscovery();
        HistoryTickDiscovery();
        ApplyCurrentCommand();
        ReadShotReadiness();
        ScopeReadyFirearms();
        ReadAndApplyOrientation();
        TargetRelativeOrientation();
        OrientationDuringFire();
        ReadShotTimingAndSeed();
        std::cout << "PASS: real ReadContext, native command sequence, firearm/readiness, accuracy rollback, trace/filter/penetration/cleanup ABI, retained storage, shotgun spread arrays, native CRC views/allocation failure, observational fresh shot readiness, scoped firearm secondary actions, timed command commits and seed/timing presence, target scan gates, bounded independent discovery and reconnect/full-identity validation\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
