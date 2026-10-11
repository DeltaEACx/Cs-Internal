#include "game/combat_access.h"

namespace combat_access {
    namespace {
        constexpr int maximumBones = 1024, maximumMeshes = 64, maximumSets = 32;
        constexpr int maximumBuckets = 4096, maximumHitboxes = 256;
        Address Add(Address address, std::size_t offset) {
            return address >= 0x10000 && address <= UINTPTR_MAX - offset ? address + offset : 0;
        }
        template<class T> std::optional<T> Read(Address address, std::size_t offset = 0) {
            const auto source = Add(address, offset);
            return source ? memory::safe_read<T>(source) : std::nullopt;
        }
        struct BoneMapHeader {
            std::uint32_t grow{}, allocation{};
            Address entries{};
            std::int32_t active{}, buckets{};
            bool operator==(const BoneMapHeader&) const = default;
        };
        struct BoneMapEntry {
            std::int32_t hash{};
            std::uint32_t token{};
            std::uint16_t index{}, padding{};
        };
        static_assert(sizeof(BoneMapHeader) == 0x18 && sizeof(BoneMapEntry) == 12);
        std::vector<BoneMapEntry> ReadBoneMap(Address model, int boneCount) {
            // Pure counterpart of client17718A0 ->176FC50, also used by ESP.
            const auto header = Read<BoneMapHeader>(model, 0x130);
            if (!header || header->active <= 0 || header->buckets <= 0 ||
                header->active > header->buckets || header->buckets > maximumBuckets ||
                (header->buckets & (header->buckets - 1)) ||
                (header->allocation & 0x7fffffffu) < static_cast<unsigned>(header->buckets) ||
                !Add(header->entries, static_cast<std::size_t>(header->buckets) * sizeof(BoneMapEntry) - 1)) return {};
            std::vector<BoneMapEntry> entries(static_cast<std::size_t>(header->buckets));
            if (!memory::copy(entries.data(), header->entries, entries.size() * sizeof(BoneMapEntry)) ||
                Read<BoneMapHeader>(model, 0x130) != header) return {};
            int active{};
            for (const auto& entry : entries) if (entry.hash >= 0) ++active;
            if (active != header->active) return {};
            for (auto& entry : entries)
                if (entry.hash >= 0 && (((static_cast<unsigned>(entry.hash) ^ entry.token) & 0x3fffffffu) ||
                    entry.index >= boneCount)) entry.hash = -1;
            return entries;
        }
    }
    Hitboxes ReadHitboxes( std::uintptr_t game_scene_node, bool seh )
    {
        Hitboxes result{};
        (void)seh; // Every access is guarded, including callers requesting fast reads.
        const auto stateOffset = systems::schemas::lookup("CSkeletonInstance", "m_modelState"_hash);
        const auto handleOffset = systems::schemas::lookup("CModelState", "m_hModel"_hash);
        if (!stateOffset || !handleOffset) return result;
        const auto state = Add(game_scene_node, stateOffset);
        const auto binding = Read<Address>(state, handleOffset).value_or(0);
        const auto model = Read<Address>(binding).value_or(0);
        // Native CModel::GetBoneCount at17747B0 reads model+178. The word
        // at ModelState+88 belongs to a second pointer, not a bone count.
        const int boneCount = Read<int>(model, 0x178).value_or(0);
        const auto meshCount = Read<int>(model, 0x70).value_or(0);
        const auto meshes = Read<Address>(model, 0x78).value_or(0);
        if (!model || boneCount <= 0 || boneCount > maximumBones ||
            meshCount <= 0 || meshCount > maximumMeshes || !meshes) return result;
        const auto remapCount = Read<int>(model, 0x220).value_or(0);
        const auto remap = Read<Address>(model, 0x228).value_or(0);
        const auto startCount = Read<int>(model, 0x238).value_or(0);
        const auto starts = Read<Address>(model, 0x240).value_or(0);
        const auto lodCount = Read<int>(model, 0x2e8).value_or(0);
        const auto lodStarts = Read<Address>(model, 0x2f0).value_or(0);
        std::vector<BoneMapEntry> boneMap;
        bool readBoneMap{};
        // Native mesh selector is essential: empty mesh0 does not mean the
        // entire model has no hitboxes. Each selector has separate remap starts.
        for (int selector{}; selector < meshCount && static_cast<std::size_t>(result.count) < result.entries.size(); ++selector) {
            const auto mesh = Read<Address>(meshes, static_cast<std::size_t>(selector) * sizeof(Address)).value_or(0);
            const auto sets = Read<int>(mesh, 0x174).value_or(0);
            const auto allocation = Read<std::uint32_t>(mesh, 0x164).value_or(0) & 0x7fffffffu;
            const auto set = allocation ? Read<Address>(mesh, 0x168).value_or(0) : 0;
            // Native storage has no inline set payload at mesh+168. For set0,
            // count/array are storage+28/+30; +18 is only the name subobject.
            if (sets <= 0 || sets > maximumSets || allocation < static_cast<unsigned>(sets) || !set) continue;
            const auto count = Read<int>(set, 0x28).value_or(0);
            const auto array = Read<Address>(set, 0x30).value_or(0);
            if (count <= 0 || count > maximumHitboxes || !array) continue;
            const auto start = selector < startCount ? Read<std::uint16_t>(starts, selector * 2) : std::nullopt;
            const auto lodStart = selector < lodCount ? Read<std::uint16_t>(lodStarts, selector * 2) : std::nullopt;
            for (int i{}; i < count && static_cast<std::size_t>(result.count) < result.entries.size(); ++i) {
                const auto hitbox = Add(array, static_cast<std::size_t>(i) * 0x70);
                const auto localBone = Read<std::uint16_t>(hitbox, 0x48);
                int bone = -1;
                if (start && lodStart && localBone && remapCount > 0 && remapCount <= 65536) {
                    const auto slot = static_cast<unsigned>(*start) + *lodStart + *localBone;
                    if (slot < static_cast<unsigned>(remapCount))
                        bone = Read<std::int16_t>(remap, slot * 2).value_or(-1);
                }
                if (bone < 0 || bone >= boneCount) {
                    if (!readBoneMap) { boneMap = ReadBoneMap(model, boneCount); readBoneMap = true; }
                    bone = -1;
                    if (const auto token = Read<std::uint32_t>(hitbox, 0x34))
                        for (const auto& entry : boneMap)
                            if (entry.hash >= 0 && entry.token == *token) { bone = entry.index; break; }
                }
                if (bone < 0) continue;
                const auto mins = Read<Vec3>(hitbox, 0x18), maxs = Read<Vec3>(hitbox, 0x24);
                const auto radius = Read<float>(hitbox, 0x30);
                const auto group = Read<int>(hitbox, 0x38);
                const auto shape = Read<std::uint8_t>(hitbox, 0x3c), translation = Read<std::uint8_t>(hitbox, 0x3d);
                if (!mins || !maxs || !Finite(*mins) || !Finite(*maxs) || !radius || !std::isfinite(*radius) ||
                    *radius < 0 || *radius > 100 || !group || *group < 0 || *group > 11 || !shape || !translation) continue;
                bool duplicate{};
                for (const auto& previous : result)
                    if (previous.index == i && previous.bone == bone && previous.group == *group) { duplicate = true; break; }
                if (duplicate) continue;
                auto& entry = result.entries[result.count++];
                entry = {i, bone, *mins, *maxs, *radius, *shape, *translation != 0, *group};
            }
        }
        if (Read<Address>(state, handleOffset).value_or(0) != binding ||
            Read<Address>(binding).value_or(0) != model || Read<int>(model, 0x178).value_or(0) != boneCount) return {};
        return result;
    }

    int Hitgroup( int hitbox )
    {
        switch ( hitbox )
        {
        case 0:  return 1;
        case 1:  return 8;
        case 2:  return 3;
        case 3:  return 3;
        case 4:  return 2;
        case 5:  return 2;
        case 6:  return 2;
        case 7:  return 7;
        case 8:  return 6;
        case 9:  return 7;
        case 10: return 6;
        case 11: return 7;
        case 12: return 6;
        case 13: return 5;
        case 14: return 4;
        case 15: return 5;
        case 16: return 5;
        case 17: return 4;
        case 18: return 4;
        default: return 0;
        }
    }

}
