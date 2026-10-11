#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace hvh_scan {
inline constexpr unsigned maximumPoseOrder = 16;
inline constexpr std::size_t maximumBudget = 256;
struct Point {
    std::size_t index{};
    std::uint64_t targetKey{};
    unsigned poseOrder{}; // 0=live, 1=newest history, then increasing age.
    bool multipoint{};
    unsigned priority{}; // Caller preference within one pawn/pose.
    float angularDistance{}, distance{};
};
inline bool Valid(const Point& point) noexcept {
    return point.index != std::numeric_limits<std::size_t>::max() && point.targetKey &&
        point.poseOrder <= maximumPoseOrder && point.priority <= 255 &&
        std::isfinite(point.angularDistance) && point.angularDistance >= 0.f && point.angularDistance <= 180.f &&
        std::isfinite(point.distance) && point.distance > 0.f && point.distance <= 32768.f;
}
namespace detail {
inline bool WithinTarget(const Point& left, const Point& right) noexcept {
    if (left.priority != right.priority) return left.priority < right.priority;
    if (left.angularDistance != right.angularDistance) return left.angularDistance < right.angularDistance;
    if (left.distance != right.distance) return left.distance < right.distance;
    return left.index < right.index;
}
inline unsigned Tier(const Point& point) noexcept { return point.poseOrder * 2u + (point.multipoint ? 1u : 0u); }
}
// Fair, deterministic scan order. Authoritative live poses precede history;
// centers precede multipoints within each pose age. Each pawn gets one point
// in a tier before another pawn gets its next point. This controls work order,
// never reachability, damage, confidence, or the caller's hitbox selection.
inline std::vector<std::size_t> Order(std::span<const Point> points, std::size_t budget) {
    budget = std::min(budget, maximumBudget);
    std::vector<std::size_t> output;
    if (!budget) return output;
    std::vector<const Point*> candidates;
    candidates.reserve(points.size());
    for (const auto& point : points) if (Valid(point)) candidates.push_back(&point);
    // An input index identifies one evaluation; duplicate metadata cannot
    // consume the scan budget twice. Prefer its first valid description.
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto* left, const auto* right) { return left->index < right->index; });
    candidates.erase(std::unique(candidates.begin(), candidates.end(), [](const auto* left, const auto* right) { return left->index == right->index; }), candidates.end());
    std::sort(candidates.begin(), candidates.end(), [](const auto* left, const auto* right) {
        if (detail::Tier(*left) != detail::Tier(*right)) return detail::Tier(*left) < detail::Tier(*right);
        if (left->targetKey != right->targetKey) return left->targetKey < right->targetKey;
        return detail::WithinTarget(*left, *right);
    });
    output.reserve(std::min(budget, candidates.size()));
    struct Bucket { std::size_t begin{}, end{}; };
    for (std::size_t begin{}; begin < candidates.size() && output.size() < budget;) {
        const auto tier = detail::Tier(*candidates[begin]);
        std::size_t end = begin;
        while (end < candidates.size() && detail::Tier(*candidates[end]) == tier) ++end;
        std::vector<Bucket> buckets;
        for (auto cursor = begin; cursor < end;) {
            const auto target = candidates[cursor]->targetKey;
            auto next = cursor + 1;
            while (next < end && candidates[next]->targetKey == target) ++next;
            buckets.push_back({cursor, next}); cursor = next;
        }
        std::sort(buckets.begin(), buckets.end(), [&](const auto& left, const auto& right) {
            const auto& a = *candidates[left.begin]; const auto& b = *candidates[right.begin];
            if (a.angularDistance != b.angularDistance) return a.angularDistance < b.angularDistance;
            if (a.distance != b.distance) return a.distance < b.distance;
            return a.targetKey < b.targetKey;
        });
        bool remaining = true;
        for (std::size_t round{}; remaining && output.size() < budget; ++round) {
            remaining = false;
            for (const auto& bucket : buckets) {
                if (round >= bucket.end - bucket.begin) continue;
                remaining = true; output.push_back(candidates[bucket.begin + round]->index);
                if (output.size() == budget) break;
            }
        }
        begin = end;
    }
    return output;
}
} // namespace hvh_scan
