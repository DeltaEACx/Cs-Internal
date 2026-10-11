#pragma once
#include "features/visuals/engine.hpp"
#include "game/sdk/CombatBindings.h"

namespace autowall_probe {
using Vec3 = math::vector3;
enum class State : std::uint8_t { Unavailable, Clear, Pass, Blocked };
enum class Reason : std::uint8_t {
    Unavailable, NoLocalPlayer, InvalidCamera, InvalidEye, RayFailed, InvalidSurface,
    UnsupportedWeapon, InvalidWeapon, BindingsUnavailable, OutOfRange,
    TraceSetupFailed, InvalidTrace, SimulationFailed, InvalidContact,
    SurfaceMismatch, Clear, Penetrated, Stopped, DamageExhausted, NoPenetrationPower,
    InvalidFraction, AllSolid, InvalidHitPosition, InvalidHitNormal, InvalidHitDistance, RayEndpointMismatch,
    FirstMaterialMismatch, NoMaterialRecord, InvalidTraceRay,
    InvalidTarget, InvalidPoint, TargetReached, TargetNotReached,
    StaleFrame, ExcludedSurface, InvalidSurfaceIdentity
};
enum class SurfaceKind : std::uint8_t { Unknown, World, Prop, Excluded };
struct FrameStamp {
    std::uint64_t session{};
    game_access::Address list{}, controller{}, pawn{}, weapon{}, weaponData{};
    std::uint32_t localHandle{0xffffffffu}, weaponHandle{0xffffffffu};
    std::uint8_t team{};
    float damage{}, penetration{}, range{}, rangeModifier{};
    bool operator==(const FrameStamp &) const = default;
};
struct Frame {
    bool valid{};
    Reason reason{Reason::Unavailable};
    FrameStamp stamp{};
    Vec3 cameraOrigin{}, eyeOrigin{};
    math::matrix4x4 viewMatrix{};
    // Field anchors permit Render-thread freshness checks without native calls.
    game_access::Address sceneNode{};
    Vec3 playerOrigin{}, viewOffset{};
};
struct Result {
    State state{State::Unavailable};
    Reason reason{Reason::Unavailable};
    Vec3 position{}, normal{}, direction{};
    float damage{}, initialDamage{}, distance{}, eyeDistance{}, thickness{};
    bool surface{};
    int contactCount{}, contactIndex{-1};
    // Error diagnostics from the native surface ray. Position-valid is cached
    // contact-position metadata at +BC; it does not mean the ray starts solid.
    float fraction{-1}, normalLength{}, endpointError{};
    bool allSolid{}, positionValid{};
    Vec3 cameraOrigin{};
    // Setup may shorten its ray and renormalize all contact fractions. These
    // values expose the actual native basis and first material for diagnostics.
    Vec3 eyeOrigin{}, penetrationOrigin{}, penetrationDelta{}, materialEntry{};
    float requestedRange{}, penetrationRange{}, materialError{}, enterFraction{}, exitFraction{}, contactDamage{};
    int surfaceCount{}, setupContactCount{};
    std::uint8_t contactFlags{};
    FrameStamp stamp{};
    SurfaceKind kind{SurfaceKind::Unknown};
    game_access::Address hitEntity{}, physicsObject{}, physicsShape{};
    std::uint32_t hitHandle{0xffffffffu};
    Vec3 exitPoint{};
    game_access::Address sceneNode{};
    Vec3 entityOrigin{}, entityRotation{};
    // Selected material boundary, captured before comparing it with the basic
    // ray. These opaque tokens are diagnostics only, never dereferenced.
    game_access::Address materialPhysicsObject{}, materialPhysicsShape{};
    std::uint32_t materialOwner{0xffffffffu};
    std::uint8_t materialType{};
    bool materialIdentityValid{};
};
struct TargetResult {
    bool valid{}, reached{}, penetrated{};
    float rawDamage{}, damage{};
    game_access::Address pawn{};
    std::uint32_t targetHandle{0xffffffffu}, weaponHandle{0xffffffffu};
    int hitgroup{}, contactCount{}, contactIndex{-1};
    Vec3 position{}, eyeOrigin{};
    std::uint32_t localHandle{0xffffffffu};
    Reason reason{Reason::Unavailable};
};
// Ballistic reachability only; callers must separately check weapon readiness.
// Hitgroup is supplied by the caller's model/hitbox selection, not inferred from
// an unverified trace index. Group0 returns native pre-target damage unchanged.
TargetResult EvaluateTarget(game_access::Address pawn,std::uint32_t handle,const Vec3 &point,int hitgroup=0,
    std::optional<combat_bindings::HistoryShotTiming> shotTiming=std::nullopt);
// The caller must intersect the ray with verified recorded hitboxes first.
// This tests travel to that historical intersection against current world
// geometry; it never rewinds or mutates the live pawn's pose.
TargetResult EvaluateHistoricalTarget(game_access::Address pawn,std::uint32_t handle,const Vec3 &intersection,
    combat_bindings::HistoryShotTiming shotTiming,int hitgroup=0);
// Caller must use the native frame thread. These functions own no game hooks,
// input commands, target selection, firing state, or live entity patches.
bool Initialize();
Result Probe();
// Read-only capture and individual screen-ray queries. NDC coordinates are in
// [-1,1], with positive Y at the top of the view. Caller owns the session epoch,
// elapsed-time budget and sample cache; native calls stay on the frame thread.
Frame CaptureFrame(std::uint64_t session=0);
// Camera-independent weapon/local capture for bounded map discovery. The eye
// remains the native shooter position; viewMatrix is empty and cameraOrigin=eye.
Frame CaptureProfile(std::uint64_t session=0);
struct ShotContact { Vec3 position{}; bool hit{}; };
// First collision only, including players. No penetration simulation or retained
// native trace storage. A clear ray returns its range endpoint; failure is nullopt.
std::optional<ShotContact> TraceShot(const Frame& profile,const Vec3& origin,const Vec3& direction,float range);
bool IsCurrent(const Frame &frame);
Result ProbeAt(const Frame &frame,float ndcX,float ndcY);
// Both the basic ray and ballistic ray start at origin. Candidate origins may
// be synthetic, so this describes geometry penetrability from that origin,
// not current-player shootability. Use profile.eyeOrigin for player-origin rays.
// Returns only the first encountered surface; callers must match any expected
// candidate identity before assigning its result to a mesh or scene object.
Result ProbeRay(const Frame &profile,const Vec3 &origin,const Vec3 &direction);
// Opaque physics tokens are equality keys, never dereferenced. Props additionally
// require their exact full handle and captured scene transform to remain current.
bool IsSurfaceCurrent(const Result &result);
// Call on the frame thread or after its callbacks have quiesced. Failure retains
// native-owned storage for another cleanup attempt; the DLL must remain mapped.
bool Reset();
const char *ReasonText(Reason reason);
}
