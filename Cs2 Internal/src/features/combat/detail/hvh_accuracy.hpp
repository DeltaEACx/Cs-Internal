#pragma once
#include "features/combat/detail/hvh_math.hpp"
#include <array>

namespace hvh_accuracy {
struct Estimate {
    unsigned hits{}, samples{};
    float chance{};
    bool passed{};
};

// Construct for one immutable weapon/accuracy snapshot and command. Invalid
// native samples are cached too; no later point can turn missing data valid.
template<unsigned Capacity = 128>
class SpreadCache {
    static_assert(Capacity > 0 && Capacity <= 512);
    std::array<std::optional<hvh_math::Vec2>, Capacity> values{};
    std::array<bool, Capacity> sampled{};
public:
    template<class Sampler>
    std::optional<hvh_math::Vec2> Get(unsigned seed, Sampler&& sampler) {
        if (seed >= Capacity) return {};
        if (!sampled[seed]) {
            sampled[seed] = true;
            values[seed] = sampler(seed);
            if (values[seed] && (!std::isfinite(values[seed]->x) || !std::isfinite(values[seed]->y))) values[seed].reset();
        }
        return values[seed];
    }
};

struct QualifiedShot {
    float damage{}, chance{}, angularDistance{}, distance{};
    int health{};
    bool penetrated{};
    std::size_t index{};
};
inline bool Valid(const QualifiedShot& shot) noexcept {
    return std::isfinite(shot.damage) && shot.damage > 0.f &&
        std::isfinite(shot.chance) && shot.chance > 0.f && shot.chance <= 1.f &&
        std::isfinite(shot.angularDistance) && shot.angularDistance >= 0.f && shot.angularDistance <= 180.f &&
        std::isfinite(shot.distance) && shot.distance > 0.f && shot.distance <= 32768.f && shot.health > 0;
}
// Call only after reachability, minimum damage and the configured confidence
// threshold have qualified each shot. Lethal shots take priority. Otherwise
// compare damage weighted by estimated hit probability, then prefer a clear
// path and a smaller angular correction. No guessed geometry can qualify a
// candidate merely because its ranking score is high.
inline bool Better(const QualifiedShot& shot, const QualifiedShot& previous) noexcept {
    if (!Valid(shot)) return false;
    if (!Valid(previous)) return true;
    const bool lethal = shot.damage >= shot.health, previousLethal = previous.damage >= previous.health;
    if (lethal != previousLethal) return lethal;
    if (lethal) {
        if (shot.chance != previous.chance) return shot.chance > previous.chance;
    } else {
        const auto expected = double(shot.damage) * shot.chance;
        const auto previousExpected = double(previous.damage) * previous.chance;
        if (expected != previousExpected) return expected > previousExpected;
        if (shot.damage != previous.damage) return shot.damage > previous.damage;
    }
    if (shot.penetrated != previous.penetrated) return !shot.penetrated;
    if (shot.angularDistance != previous.angularDistance) return shot.angularDistance < previous.angularDistance;
    if (shot.distance != previous.distance) return shot.distance < previous.distance;
    return shot.index < previous.index;
}
inline std::optional<std::size_t> Choose(std::span<const QualifiedShot> shots) noexcept {
    std::optional<std::size_t> best;
    for (std::size_t i{}; i < shots.size(); ++i)
        if (Valid(shots[i]) && (!best || Better(shots[i], shots[*best]))) best = i;
    return best;
}

// The sampler supplies native first-pellet spread for each seed. The caller
// intersects each full-length ray with a fresh snapshot of the target's actual
// hitboxes. Missing geometry or a failed native sample is unavailable, never a
// claim that the weapon is accurate. This estimates a probability; it cannot
// reconstruct a server's private random seed or account for untraced surfaces.
template<class Sample, class Intersects>
std::optional<Estimate> Evaluate(hvh_math::Vec3 physicalAngles, float range,
                                float minimumChance, bool accuracyValid,
                                Sample&& sample, Intersects&& intersects,
                                unsigned samples = 128) {
    if (!accuracyValid || !hvh_math::detail::Finite(physicalAngles) ||
        !std::isfinite(range) || range <= 0.f || range > 32768.f ||
        !std::isfinite(minimumChance) || minimumChance < 0.f || minimumChance > 1.f ||
        !samples || samples > 512) return {};
    const auto basis=hvh_math::MakeShotBasis(physicalAngles);
    if(!basis)return {};
    unsigned hits{};
    for (unsigned seed{}; seed < samples; ++seed) {
        const auto spread = sample(seed);
        if (!spread) return {};
        const auto direction = hvh_math::DirectionWithBasis(*basis, *spread);
        if (!direction) return {};
        const hvh_math::Vec3 delta{direction->x * range, direction->y * range, direction->z * range};
        const auto hit = intersects(delta);
        if (!hit) return {};
        hits += *hit ? 1u : 0u;
        const auto processed = seed + 1, remaining = samples - processed;
        // A passing estimate still samples the full set for equivalent ranking.
        // Once even every remaining ray hitting cannot meet the threshold, a
        // failed verdict is final. Report only the actually observed samples.
        if (double(hits + remaining) / samples < double(minimumChance))
            return Estimate{hits, processed, static_cast<float>(double(hits) / processed), false};
    }
    const auto chance = static_cast<float>(double(hits) / samples);
    return Estimate{hits, samples, chance, double(hits) / samples >= double(minimumChance)};
}

// Damage is evaluated for one first-impact hitgroup. Samples hitting another
// part of the pawn cannot inherit that group's damage or lethal score. The
// geometry callback returns group0 for a clear miss and nullopt when the pose
// cannot establish an impact, preserving unavailable data as unavailable.
template<class Sample, class FirstGroup>
std::optional<Estimate> EvaluateGroup(hvh_math::Vec3 physicalAngles, float range,
                                     float minimumChance, bool accuracyValid,
                                     int requiredGroup, Sample&& sample, FirstGroup&& firstGroup,
                                     unsigned samples = 128) {
    if (requiredGroup < 1 || requiredGroup > 8) return {};
    return Evaluate(physicalAngles, range, minimumChance, accuracyValid, sample,
        [&](hvh_math::Vec3 delta) -> std::optional<bool> {
            const auto group = firstGroup(delta);
            if (!group || *group < 0 || *group > 8) return {};
            return *group == requiredGroup;
        }, samples);
}
} // namespace hvh_accuracy
