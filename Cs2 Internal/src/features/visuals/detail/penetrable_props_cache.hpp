#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace visuals::detail::penetrable_props
{
    struct MapKey
    {
        std::uint64_t epoch{}, list{};
        bool operator==(const MapKey&) const noexcept = default;
    };
    struct Transform
    {
        std::array<float, 3> origin{}, angles{};
        bool operator==(const Transform&) const noexcept = default;
    };
    struct Stamp
    {
        MapKey map{};
        std::uint32_t handle{0xffffffffu};
        std::uint64_t entity{}, node{}, modelBinding{}, modelResource{}, collision{};
        Transform transform{};
        bool operator==(const Stamp&) const noexcept = default;
    };

    inline bool ValidToken(std::uint64_t token) noexcept
    { return token >= 0x10000 && token <= 0x7fffffffffffULL; }
    inline bool Valid(MapKey map) noexcept
    { return map.epoch != 0 && ValidToken(map.list); }
    inline bool Valid(const Stamp& stamp) noexcept
    {
        const auto slot = stamp.handle & 0x7fffu;
        if (!Valid(stamp.map) || slot == 0 || slot == 0x7fff || stamp.handle == 0xfffffffeu ||
            !ValidToken(stamp.entity) || !ValidToken(stamp.node) ||
            !ValidToken(stamp.modelBinding) || !ValidToken(stamp.modelResource) ||
            !ValidToken(stamp.collision)) return false;
        for (const auto& values : {stamp.transform.origin, stamp.transform.angles})
            for (float value : values)
                if (!std::isfinite(value) || std::abs(value) > 1e7f) return false;
        return true;
    }
    inline std::uint64_t Fingerprint(const Transform& transform) noexcept
    {
        std::uint64_t hash = 14695981039346656037ULL;
        for (const auto& values : {transform.origin, transform.angles})
            for (float value : values)
            {
                // Native signed zero describes the same transform. No position
                // tolerance is applied to other finite values.
                const auto bits = std::bit_cast<std::uint32_t>(value == 0 ? 0.f : value);
                for (unsigned shift = 0; shift < 32; shift += 8)
                { hash ^= (bits >> shift) & 0xffu; hash *= 1099511628211ULL; }
            }
        return hash;
    }

    // Selection lifetime is the current map, not a camera/weapon/time window.
    // The caller synchronizes access and supplies newly read native stamps for
    // Matches. Never validate a selected prop against its saved stamp itself.
    template<std::size_t Capacity = 256>
    class Cache
    {
        static_assert(Capacity > 0 && Capacity <= 4096);
        struct Entry { Stamp stamp{}; std::uint64_t transformFingerprint{}; };
        MapKey map_{};
        std::array<Entry, Capacity> entries_{};
        std::size_t size_{};
    public:
        bool SetMap(MapKey map) noexcept
        {
            if (!Valid(map)) { Clear(); return false; }
            if (!(map_ == map)) { Clear(); map_ = map; }
            return true;
        }
        void Clear() noexcept { map_ = {}; entries_ = {}; size_ = 0; }
        std::size_t Size() const noexcept { return size_; }
        bool Remember(const Stamp& stamp, bool qualified) noexcept
        {
            // Explicit SetMap owns generation changes. A late old-map native
            // callback must not reset or repopulate the current map's cache.
            if (!qualified || !Valid(stamp) || !(stamp.map == map_)) return false;
            const Entry entry{stamp, Fingerprint(stamp.transform)};
            for (std::size_t i = 0; i < size_; ++i)
                if ((entries_[i].stamp.handle & 0x7fffu) == (stamp.handle & 0x7fffu))
                { entries_[i] = entry; return true; }
            // Keep prior confirmed selections when saturated. Replacements of
            // an existing native slot above remain possible at full capacity.
            if (size_ == Capacity) return false;
            entries_[size_++] = entry;
            return true;
        }
        bool Matches(const Stamp& current) const noexcept
        {
            if (!Valid(current) || !(current.map == map_)) return false;
            const auto fingerprint = Fingerprint(current.transform);
            for (std::size_t i = 0; i < size_; ++i)
                if (entries_[i].stamp.handle == current.handle)
                    // Compare the complete transform too: a hash collision is
                    // never sufficient to reuse a selected model/collision.
                    return entries_[i].transformFingerprint == fingerprint && entries_[i].stamp == current;
            return false;
        }
        bool Forget(std::uint32_t fullHandle) noexcept
        {
            for (std::size_t i = 0; i < size_; ++i)
                if (entries_[i].stamp.handle == fullHandle)
                {
                    for (std::size_t next = i + 1; next < size_; ++next) entries_[next - 1] = entries_[next];
                    entries_[--size_] = {};
                    return true;
                }
            return false;
        }
    };
}
