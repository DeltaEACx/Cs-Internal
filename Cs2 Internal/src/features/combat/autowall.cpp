#include "features/combat/autowall.h"

namespace autowall
{
    namespace
    {
        using namespace combat_access;
        // Spectral's trace_bullet contact layout; the engine applies range falloff
        // and material penetration before these records are consumed.
        struct Contact
        {
            float enterFraction{}, exitFraction{}, damage{};
            int team{};
            std::uint16_t enter{}, exit{};
            std::uint8_t penetrate{}, padding[3]{};
        };
        static_assert(sizeof(Contact) == 0x18);
        static_assert(offsetof(Contact, enter) == 0x10);
        static_assert(offsetof(Contact, penetrate) == 0x14);
        constexpr int kMaximumContacts = 128;
        constexpr int kMaximumCapacity = 65536;
        TraceData scratch{};
        std::mutex traceMutex;

        void ResetScratch(TraceData &trace)
        {
            // Native vectors can grow beyond their inline storage. Retain both
            // ownership triples while clearing per-ray counts and scratch fields.
            const auto surfaces = trace.arrayPointer;
            const int surfaceCapacity = trace.capacity, surfaceFlags = trace.flags;
            const auto contacts = trace.hitArray;
            const int contactCapacity = trace.hitCapacity, contactFlags = trace.hitFlags;
            trace = {};
            trace.arrayPointer = surfaces;
            trace.capacity = surfaceCapacity;
            trace.flags = surfaceFlags;
            trace.hitArray = contacts;
            trace.hitCapacity = contactCapacity;
            trace.hitFlags = contactFlags;
        }

        bool ValidWeapon(const Context &context)
        {
            const auto &weapon = context.weapon;
            return context.valid && context.local.pawn && Finite(context.eye) &&
                std::isfinite(weapon.damage) && weapon.damage > 0 &&
                std::isfinite(weapon.range) && weapon.range > 0 && weapon.range <= 32768 &&
                std::isfinite(weapon.penetration) && weapon.penetration >= 0 &&
                std::isfinite(weapon.rangeModifier) && weapon.rangeModifier >= 0;
        }
        bool MakeDelta(const Context &context, const Vec3 &direction, Vec3 &delta)
        {
            if (!ValidWeapon(context) || !Finite(direction)) return false;
            const float lengthSquared = direction.length_sqr();
            if (!std::isfinite(lengthSquared) || lengthSquared < 1.e-8f) return false;
            delta = direction * (context.weapon.range / std::sqrt(lengthSquared));
            return Finite(delta);
        }
        bool ValidTrace(const TraceData &trace)
        {
            if (trace.numHits < 0 || trace.numHits > kMaximumContacts ||
                trace.hitCapacity <= 0 || trace.hitCapacity > kMaximumCapacity ||
                trace.numHits > trace.hitCapacity || trace.capacity <= 0 ||
                trace.capacity > kMaximumCapacity || trace.unknown1 < 0 ||
                trace.unknown1 > static_cast<int>(trace.elements.size()) ||
                trace.unknown1 > trace.capacity) return false;
            if ((trace.numHits > 0 && (!trace.hitArray || !trace.arrayPointer)) ||
                (trace.unknown1 > 0 && !trace.arrayPointer)) return false;
            // Capacity describes allocated storage, not the live surface count.
            // Inline buffers must keep their actual bounds even after a failed call.
            if (trace.arrayPointer == trace.elements.data() &&
                trace.capacity > static_cast<int>(trace.elements.size())) return false;
            if (trace.hitArray == trace.hitElements &&
                trace.hitCapacity > static_cast<int>(sizeof(trace.hitElements) / sizeof(Contact))) return false;
            return true;
        }
        bool TraceBullet(const Context &context, const Vec3 &delta, TraceData &trace, ProbeResult *diagnostic = nullptr)
        {
            ResetScratch(trace);
            if (!PrepareTrace(context, delta, trace))
            {
                if (diagnostic) diagnostic->reason = ProbeReason::TraceSetupFailed;
                return false;
            }
            if (diagnostic) diagnostic->contactCount = trace.numHits;
            if (!ValidTrace(trace))
            {
                if (diagnostic) diagnostic->reason = ProbeReason::InvalidTrace;
                return false;
            }
            if (trace.numHits == 0) return true;
            if (!ApplyBullet(context, trace))
            {
                if (diagnostic) diagnostic->reason = ProbeReason::BulletSimulationFailed;
                return false;
            }
            // trace_bullet can truncate the contact stream and update its storage.
            if (diagnostic) diagnostic->contactCount = trace.numHits;
            if (!ValidTrace(trace))
            {
                if (diagnostic) diagnostic->reason = ProbeReason::InvalidTrace;
                return false;
            }
            return true;
        }
        bool ReadContact(const TraceData &trace, int index, Contact &contact)
        {
            if (index < 0 || index >= trace.numHits ||
                !memory::copy(&contact, reinterpret_cast<Address>(trace.hitArray) +
                    static_cast<Address>(index) * sizeof(Contact), sizeof(contact))) return false;
            if (!std::isfinite(contact.enterFraction) || !std::isfinite(contact.exitFraction) ||
                !std::isfinite(contact.damage) || contact.enterFraction < 0 || contact.enterFraction > 1 ||
                contact.exitFraction < 0 || contact.exitFraction > 1) return false;
            return !(contact.penetrate & 1) || contact.exitFraction >= contact.enterFraction;
        }
    }

    float ScaleDamage(float damage, int group, int armor, bool helmet, int team,
                      float armorRatio, float headMultiplier, const combat_access::DamageScales &scales)
    {
        if (!std::isfinite(damage) || damage <= 0) return 0;
        const float head = team == 3 ? scales.ctHead : scales.tHead;
        const float body = team == 3 ? scales.ctBody : scales.tBody;
        switch (group)
        {
        case 1:
            if (!std::isfinite(headMultiplier) || headMultiplier < 0 || !std::isfinite(head) || head < 0) return 0;
            damage *= headMultiplier * head;
            break;
        case 2: case 4: case 5: case 8:
            if (!std::isfinite(body) || body < 0) return 0;
            damage *= body;
            break;
        case 3:
            if (!std::isfinite(body) || body < 0) return 0;
            damage *= 1.25f * body;
            break;
        case 6: case 7:
            if (!std::isfinite(body) || body < 0) return 0;
            damage *= .75f * body;
            break;
        default: break;
        }
        const bool armored = (group >= 1 && group <= 5) || group == 8;
        if (armor > 0 && armored && (group != 1 || helmet))
        {
            if (!std::isfinite(armorRatio) || armorRatio < 0) return 0;
            constexpr float armorBonus = .5f;
            float healthDamage = damage * armorRatio * .5f;
            if ((damage - healthDamage) * armorBonus > static_cast<float>(armor))
                healthDamage = damage - static_cast<float>(armor) / armorBonus;
            damage = healthDamage;
        }
        return std::isfinite(damage) ? std::max(0.f, std::floor(damage)) : 0;
    }

    bool Evaluate(const combat_access::Context &context, const combat_access::Target &target,
                  const combat_access::Vec3 &point, Result &result)
    {
        using namespace combat_access;
        result = {};
        if (!target.pawn || target.pawn == context.local.pawn || target.handle == 0xffffffffu ||
            target.health <= 0 || target.team < 2 || target.team > 3 || target.team == context.local.team ||
            target.hitboxes.count <= 0 || target.hitboxes.count > static_cast<int>(target.hitboxes.entries.size()) ||
            !Finite(point)) return false;
        Vec3 delta{};
        if (!MakeDelta(context, point - context.eye, delta)) return false;
        float closest = 1;
        int actualHitbox = -1;
        for (const auto &hitbox : target.hitboxes)
        {
            if (hitbox.bone < 0 || hitbox.bone >= static_cast<int>(target.bones.size())) continue;
            float fraction = 1;
            if (RayHitbox(context.eye, delta, hitbox, target.bones[hitbox.bone], fraction) &&
                std::isfinite(fraction) && fraction >= 0 && fraction < closest)
            {
                closest = fraction;
                actualHitbox = hitbox.index;
            }
        }
        if (actualHitbox < 0) return false;
        std::lock_guard lock(traceMutex);
        auto &trace = scratch;
        if (!TraceBullet(context, delta, trace)) return false;
        bool penetrated = false;
        for (int i = 0; i < trace.numHits; ++i)
        {
            Contact contact{};
            if (!ReadContact(trace, i, contact) || contact.damage <= 0) return false;
            if (contact.penetrate & 1)
            {
                // Spectral uses exit == 1 as a stopped bullet, not a completed wall.
                if (contact.exitFraction >= 1) return false;
                penetrated = true;
                continue;
            }
            // A free-travel interval starts at the preceding surface and ends at
            // the next collision. Match its exit boundary before flesh penetration
            // can consume the bullet or truncate the following contact records.
            const unsigned boundary = contact.exit & 0x7fff;
            if (boundary >= static_cast<unsigned>(trace.unknown1)) return false;
            const auto handle = memory::safe_read<std::uint32_t>(reinterpret_cast<Address>(trace.arrayPointer) +
                static_cast<Address>(boundary) * sizeof(TraceElement) + 0x2c);
            if (!handle) return false;
            if (*handle != target.handle || game_access::Resolve(context.local.list, *handle) != target.pawn) continue;
            const int hitgroup = Hitgroup(actualHitbox);
            const float damage = ScaleDamage(contact.damage, hitgroup, target.armor, target.helmet, target.team,
                                              context.weapon.armorRatio, context.weapon.headMultiplier, context.scales);
            if (damage <= 0) return false;
            result = {damage, actualHitbox, hitgroup, penetrated};
            return true;
        }
        return false;
    }

    bool Probe(const combat_access::Context &context, const combat_access::Vec3 &direction,
               ProbeResult &result)
    {
        using namespace combat_access;
        result = {};
        if (!ValidWeapon(context))
        {
            result.reason = ProbeReason::InvalidContext;
            return false;
        }
        Vec3 delta{};
        if (!MakeDelta(context, direction, delta))
        {
            result.reason = ProbeReason::InvalidDirection;
            return false;
        }
        std::lock_guard lock(traceMutex);
        auto &trace = scratch;
        if (!TraceBullet(context, delta, trace, &result)) return false;
        for (int i = 0; i < trace.numHits; ++i)
        {
            Contact contact{};
            result.contactIndex = i;
            if (!ReadContact(trace, i, contact))
            {
                result.reason = ProbeReason::InvalidContact;
                return false;
            }
            result.flags = contact.penetrate;
            result.enterFraction = contact.enterFraction;
            result.exitFraction = contact.exitFraction;
            result.contactDamage = contact.damage;
            const bool material = (contact.penetrate & 1) != 0;
            // Native records with bit 0 clear describe free travel/range falloff.
            // The builder commonly inserts one before the first material span.
            result.hitSurface = material && contact.enterFraction < 1;
            if (contact.damage <= 0)
            {
                result.valid = true;
                result.reason = material && contact.exitFraction >= 1 ?
                    ProbeReason::Stopped : ProbeReason::DamageExhausted;
                return false;
            }
            if (!material) continue;
            result.valid = true;
            if (contact.exitFraction >= 1)
            {
                result.reason = ProbeReason::Stopped;
                return false;
            }
            if (context.weapon.penetration <= 0)
            {
                result.reason = ProbeReason::NoPenetrationPower;
                return false;
            }
            result.damage = contact.damage;
            result.penetrated = true;
            result.reason = ProbeReason::Penetrated;
            return true;
        }
        result.valid = true;
        result.reason = ProbeReason::Clear;
        return false;
    }

    bool Reset()
    {
        std::lock_guard lock(traceMutex);
        if (!combat_access::ReleaseTrace(scratch)) return false;
        scratch = {};
        return true;
    }

    const char *ProbeReasonText(ProbeReason reason)
    {
        switch (reason)
        {
        case ProbeReason::Unavailable: return "Waiting for a wall trace.";
        case ProbeReason::InvalidContext: return "Weapon or local player data is invalid.";
        case ProbeReason::InvalidDirection: return "The crosshair ray direction is invalid.";
        case ProbeReason::TraceSetupFailed: return "The native wall trace failed.";
        case ProbeReason::InvalidTrace: return "The native contact storage is invalid.";
        case ProbeReason::BulletSimulationFailed: return "The native bullet simulation failed.";
        case ProbeReason::InvalidContact: return "A native bullet contact is invalid.";
        case ProbeReason::Clear: return "No material span along the crosshair ray.";
        case ProbeReason::NoPenetration: return "No material span was penetrated.";
        case ProbeReason::DamageExhausted: return "Bullet damage was exhausted.";
        case ProbeReason::Stopped: return "The bullet stopped inside the material.";
        case ProbeReason::NoPenetrationPower: return "The equipped weapon has no penetration power.";
        case ProbeReason::Penetrated: return "The first material span was penetrated.";
        }
        return "Unknown wall trace result.";
    }
}
