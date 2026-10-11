#pragma once
#include "features/visuals/engine.hpp"
#include "game/sdk/CombatBindings.h"

namespace combat_command { struct IO; }
namespace combat_access
{
    using Vec3 = math::vector3;
    using Address = game_access::Address;
    struct Hitbox
    {
        int index{-1}, bone{-1};
        Vec3 mins{}, maxs{};
        float radius{};
        std::uint8_t shape_type{};
        bool translation_only{};
        int group{-1}; // Declared native hitgroup; -1 denotes legacy ordinal metadata.
    };
    struct Hitboxes
    {
        std::array<Hitbox, 20> entries{};
        int count{};
        const Hitbox *begin() const { return entries.data(); }
        const Hitbox *end() const { return entries.data() + count; }
    };
    struct DamageScales { float ctHead{1}, tHead{1}, ctBody{1}, tBody{1}; };
    struct Weapon
    {
        Address address{}, data{};
        std::uint32_t handle{0xffffffffu};
        int type{}, definition{}, bullets{}, clip{};
        float damage{}, penetration{}, range{}, rangeModifier{}, armorRatio{}, headMultiplier{};
        float recoil{}, inaccuracy{}, spread{};
    };
    struct Context
    {
        game_access::LocalPlayer local{};
        Address command{}, base{}, input{};
        combat_bindings::BaseCommand baseSnapshot{};
        Weapon weapon{};
        Vec3 eye{}, angles{}, punch{};
        DamageScales scales{};
        int tick{}, commandNumber{};
        float sensitivity{1}, fovAdjust{1};
        bool canShoot{}, valid{}, accuracyValid{};
        std::optional<combat_bindings::HistoryShotTiming> shotTiming;
        const char *failure{"Waiting for a game command."};
    };
    struct CommittedShot
    {
        Address command{};
        int sequence{};
        std::uint32_t localHandle{0xffffffffu}, weaponHandle{0xffffffffu};
        combat_bindings::HistoryShotTiming timing{};
        Vec3 angles{};
        std::optional<combat_bindings::BacktrackTiming> backtrack;
    };
    struct OrientationContext
    {
        game_access::LocalPlayer local{};
        Address command{}, base{}, input{}, weapon{};
        std::uint32_t weaponHandle{0xffffffffu};
        Vec3 angles{};
        int tick{}, commandNumber{}, weaponType{};
        bool valid{};
        std::optional<CommittedShot> shot;
        const char *failure{"Waiting for a game command."};
    };
    struct Target
    {
        Address pawn{}, controller{};
        std::uint32_t handle{0xffffffffu};
        int health{}, armor{}, team{};
        bool helmet{};
        Hitboxes hitboxes{};
        std::array<systems::bones::data, 27> bones{};
    };
    struct OrientationTarget
    {
        Address pawn{},scene{},localScene{};
        std::uint32_t handle{0xffffffffu};
        Vec3 origin{},localOrigin{};
        float yaw{};
    };
    // Progressive remote-player gates for diagnosing an empty target scan.
    struct TargetScan
    {
        unsigned controllers{}, pawns{}, enemies{}, eligibleEnemies{}, hitboxPlayers{}, hitboxCount{};
    };
    struct TraceElement { std::byte bytes[0x38]; };
    struct TraceData
    {
        int unknown1{};
        float unknown2{52};
        void *arrayPointer{};
        int capacity{128}, flags{static_cast<int>(0x80000000)};
        std::array<TraceElement, 128> elements{};
        std::byte padding[8]{};
        int numHits{}, unknown5{};
        void *hitArray{};
        int hitCapacity{8}, hitFlags{static_cast<int>(0x80000000)};
        std::byte hitElements[0xc0]{};
        Vec3 start{}, direction{};
        float fraction{1};
        bool unknown6{};
        std::byte tail[0x4b]{};
    };
    static_assert(offsetof(TraceData, numHits) == 0x1c20);
    static_assert(offsetof(TraceData, hitArray) == 0x1c28);
    static_assert(sizeof(TraceData) == 0x1d60);

    template <class T> std::optional<T> Field(Address object, const char *type, const char *field)
    {
        if (!object) return {};
        const auto offset = systems::schemas::lookup(type, fnv1a::runtime_hash(field));
        return offset ? memory::safe_read<T>(object + offset) : std::nullopt;
    }
    bool InitializeInput();
    bool Initialize();
    const char *BindingStatus();
    Address CreateMoveTarget();
    Address HandleViewAnglesTarget();
    struct CommandIdentity
    {
        Address command{};
        int sequence{};
        std::uint32_t localHandle{0xffffffffu};
        bool subtickOverwrite{}, weaponSelectionPending{};
    };
    std::optional<CommandIdentity> ReadInputCommand(Address input);
    bool ReadViewAngles(Address input, Vec3 &angles);
    bool WriteViewAngles(Address input, const Vec3 &angles);
    bool WriteViewAngles(const Vec3 &angles);
    bool ReadShootPosition(Address pawn, Vec3 &position);
    bool ReadShotPosition(Address pawn, combat_bindings::HistoryShotTiming timing, Vec3 &position);
    Address InputEventTarget();
    combat_command::IO CommandIO();
    Context ReadContext(Address input, bool readAncillarySettings = true);
    OrientationContext ReadOrientationContext(Address input, const CommittedShot *shot = nullptr);
    std::optional<OrientationTarget> ReadOrientationTarget(const OrientationContext& context,
        Address preferredPawn=0,std::uint32_t preferredHandle=0xffffffffu);
    bool ApplyOrientation(const OrientationContext &context, const Vec3 &angles,const OrientationTarget* target=nullptr);
    std::vector<Target> ReadTargets(const Context &context, TargetScan *scan = nullptr,bool geometry=true);
    std::vector<Target> ReadHistoryTargets(const game_access::LocalPlayer& local, bool geometry = true);
    void ResetTargets();
    bool PrepareTrace(const Context &context, const Vec3 &direction, TraceData &trace);
    bool ApplyBullet(const Context &context, TraceData &trace);
    bool ReleaseTrace(TraceData &trace);
    bool PressAttack(const Context &context);
    bool ReadyForShot(const Context &context);
    // Only the six firearms with a scope return a known state.
    std::optional<bool> IsScoped(const Context &context);
    bool Scope(const Context &context);
    bool AttackDown(const Context &context);
    bool ApplyCommand(const Context &context, const std::optional<Vec3> &aim, bool pressAttack);
    // Spectral's angle/tick seed uses the shot direction before recoil removal.
    std::optional<std::uint32_t> SpreadSeed(const Vec3 &angles, int tick);
    std::optional<combat_bindings::HistoryShotTiming> ShotTiming(const Context &context);
    bool ApplyShot(const Context &context, const Vec3 &angles, combat_bindings::HistoryShotTiming timing,
        std::optional<combat_bindings::BacktrackTiming> backtrack = std::nullopt);
    math::vector2 Spread(const Weapon &weapon, unsigned seed);
    float Convar(const char *name, float fallback);
    std::optional<bool> ConvarBool(const char *name);
    bool ReadViewAngles(Vec3 &angles);
    bool ReadAccuracy(Address weapon, float &spread, float &inaccuracy);
    bool Finite(const Vec3 &value);
    Hitboxes ReadHitboxes(Address scene, bool seh = true);
    int Hitgroup(int hitbox);
    bool RayCapsule(const Vec3 &origin, const Vec3 &delta, const Vec3 &a, const Vec3 &b, float radius, float &fraction);
    bool RayHitbox(const Vec3 &origin, const Vec3 &delta, const Hitbox &hitbox,
                   const systems::bones::data &bone, float &fraction);
}
