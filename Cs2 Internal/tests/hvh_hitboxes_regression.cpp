#include "game/combat_access.h"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
    using combat_access::Address;
    constexpr unsigned stateOffset = 0x80, handleOffset = 0xc0;
    constexpr std::uint32_t headToken = 1462898209u;
    void Check(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    template<class T> Address At(const T& object) { return reinterpret_cast<Address>(&object); }
    template<class T, class Buffer> void Put(Buffer& buffer, std::size_t offset, const T& value)
    {
        Check(offset <= sizeof(buffer) && sizeof(value) <= sizeof(buffer) - offset, "fixture write overflow");
        std::memcpy(reinterpret_cast<std::byte*>(&buffer) + offset, &value, sizeof(value));
    }
    struct Range { Address address{}; std::size_t size{}; };
    std::vector<Range> ranges;
    unsigned deniedReads{}, handleReads{};
    Address changingHandle{};
    bool missingState{}, missingHandle{};
    struct BoneMapHeader
    {
        std::uint32_t grow{}, allocation{};
        Address entries{};
        int active{}, buckets{};
    };
    struct BoneMapEntry
    {
        std::int32_t hash{-1};
        std::uint32_t token{};
        std::uint16_t index{}, pad{};
    };
    static_assert(sizeof(BoneMapHeader) == 0x18 && sizeof(BoneMapEntry) == 12);
    struct Fixture
    {
        std::array<std::byte, 0x400> scene{}, model{};
        Address binding{};
        std::array<Address, 4> meshes{};
        std::array<std::byte, 0x200> mesh0{}, mesh1{};
        std::array<std::byte, 0x90> set0{}, set1{};
        std::array<std::byte, 0x70 * 32> hitboxes{};
        std::array<std::uint16_t, 4> starts{3, 8, 0, 0}, lodStarts{4, 2, 0, 0};
        std::array<std::int16_t, 128> remap{};
        std::array<BoneMapEntry, 8> boneMap{};
        BoneMapHeader mapHeader{};
        template<class T> void Register(const T& object) { ranges.push_back({At(object), sizeof(object)}); }
        void Activate()
        {
            ranges.clear(); deniedReads = handleReads = 0; changingHandle = 0;
            missingState = missingHandle = false;
            Register(scene); Register(model); Register(binding); Register(meshes);
            Register(mesh0); Register(mesh1); Register(set0); Register(set1); Register(hitboxes);
            Register(starts); Register(lodStarts); Register(remap); Register(boneMap);
        }
        void SyncMap() { Put(model, 0x130, mapHeader); }
        Fixture()
        {
            binding = At(model);
            Put(scene, stateOffset + handleOffset, At(binding));
            Put(scene, stateOffset + 0x88, At(remap)); // Second cache pointer, never a bone count.
            Put(scene, stateOffset + 0x90, 128);
            Put(scene, stateOffset + 0x94, std::uint16_t{115});
            Put(scene, stateOffset + 0x96, std::uint16_t{0xffff});
            Put(model, 0x178, 115);
            Put(model, 0x70, 2); Put(model, 0x78, At(meshes));
            meshes[0] = At(mesh0); meshes[1] = At(mesh1);
            Put(mesh1, 0x164, std::uint32_t{0x80000001});
            Put(mesh1, 0x168, At(set1)); Put(mesh1, 0x174, 1);
            Put(set1, 0x18, Address{0x12345678}); // Name subobject, not the set base.
            Put(set1, 0x28, 1); Put(set1, 0x30, At(hitboxes));
            Put(model, 0x220, static_cast<int>(remap.size())); Put(model, 0x228, At(remap));
            Put(model, 0x238, 2); Put(model, 0x240, At(starts));
            Put(model, 0x2e8, 2); Put(model, 0x2f0, At(lodStarts));
            remap.fill(-1); remap[14] = 5; remap[17] = 73;
            for (auto& entry : boneMap) entry.hash = -1;
            boneMap[0] = {static_cast<std::int32_t>(headToken & 0x3fffffffu), headToken, 14, 0};
            mapHeader = {0, 8, At(boneMap), 1, 8}; SyncMap();
            Hitbox(0, 7, headToken);
            Activate();
        }
        void Hitbox(unsigned index, std::uint16_t localBone, std::uint32_t token)
        {
            const auto offset = std::size_t(index) * 0x70;
            Put(hitboxes, offset + 0x18, combat_access::Vec3{-2, -3, -4});
            Put(hitboxes, offset + 0x24, combat_access::Vec3{2, 3, 4});
            Put(hitboxes, offset + 0x30, 4.f); Put(hitboxes, offset + 0x34, token);
            Put(hitboxes, offset + 0x38, 1); Put(hitboxes, offset + 0x3c, std::uint8_t{2});
            Put(hitboxes, offset + 0x3d, std::uint8_t{1});
            Put(hitboxes, offset + 0x40, std::uint32_t{0xdeadc0de}); // Native resource hash, not an ordinal.
            Put(hitboxes, offset + 0x48, localBone);
        }
        auto Read(bool safe = true) { return combat_access::ReadHitboxes(At(scene), safe); }
    };
}

namespace memory
{
    bool copy(void* destination, std::uintptr_t address, std::size_t size) noexcept
    {
        if (address == changingHandle && ++handleReads == 2)
        {
            const Address unavailable = 0;
            std::memcpy(reinterpret_cast<void*>(address), &unavailable, sizeof(unavailable));
        }
        for (const auto& range : ranges)
            if (address >= range.address && address - range.address <= range.size && size <= range.size - (address - range.address))
            {
                std::memcpy(destination, reinterpret_cast<const void*>(address), size);
                return true;
            }
        ++deniedReads;
        return false;
    }
}
namespace systems
{
    std::uint32_t schemas::lookup(const char* owner, std::uint32_t hash)
    {
        if (!missingState && std::strcmp(owner, "CSkeletonInstance") == 0 && hash == fnv1a::runtime_hash("m_modelState")) return stateOffset;
        if (!missingHandle && std::strcmp(owner, "CModelState") == 0 && hash == fnv1a::runtime_hash("m_hModel")) return handleOffset;
        return 0;
    }
}
namespace combat_access
{
    bool Finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
}

namespace
{
    void NativeLayout()
    {
        Fixture fixture;
        const auto result = fixture.Read();
        Check(result.count == 1 && result.entries[0].index == 0 && result.entries[0].bone == 73,
              "empty mesh0 hid mesh1 or remap used selector0 offsets");
        const auto& hit = result.entries[0];
        Check(hit.mins.x == -2 && hit.mins.y == -3 && hit.mins.z == -4 &&
              hit.maxs.x == 2 && hit.maxs.y == 3 && hit.maxs.z == 4 && hit.radius == 4 &&
              hit.shape_type == 2 && hit.translation_only && hit.group == 1, "native hitbox fields were read at incorrect offsets");
        Put(fixture.hitboxes,0x38,2);
        Check(fixture.Read().entries[0].group==2,"Declared native torso group was replaced by head ordinal0");
        Put(fixture.hitboxes,0x38,0);
        Check(fixture.Read().entries[0].group==0,"Declared generic group was converted to legacy ordinal metadata");
        Put(fixture.hitboxes,0x38,-1);
        Check(fixture.Read().count==0,"Negative native hitgroup escaped metadata validation");
        Put(fixture.hitboxes,0x38,1);
        Check(deniedReads == 0, "valid parser layout attempted an unmapped read");
        Check(fixture.Read(false).count == 1, "fast request lost guarded native parser");
        for (const std::uint32_t upper : {1006u, 65536u, UINT32_MAX})
        {
            Put(fixture.scene, stateOffset + 0x88, (Address{upper} << 32) | 0x10000);
            const auto metadata = fixture.Read();
            Check(metadata.count == 1 && metadata.entries[0].bone == 73,
                  "cache pointer high bits were interpreted as the live bone count");
        }
        // Each mesh selector has its own remap starts, even when storage is shared.
        Put(fixture.mesh0, 0x164, std::uint32_t{1}); Put(fixture.mesh0, 0x168, At(fixture.set1)); Put(fixture.mesh0, 0x174, 1);
        const auto both = fixture.Read();
        Check(both.count == 2 && both.entries[0].bone == 5 && both.entries[1].bone == 73,
              "mesh-local native remap starts are not honored");
        fixture.remap[14] = 73;
        Check(fixture.Read().count == 1, "duplicate mesh metadata produced duplicate hitbox");
    }

    void BoneNames()
    {
        Fixture fixture;
        Put(fixture.model, 0x220, 0); Put(fixture.model, 0x228, Address{});
        const auto named = fixture.Read();
        Check(named.count == 1 && named.entries[0].bone == 14, "bone-name map did not resolve custom native head14");
        Put(fixture.hitboxes, 0x34, headToken + 1);
        Check(fixture.Read().count == 0, "missing named bone invented a native index");
        Put(fixture.hitboxes, 0x34, headToken);
        fixture.boneMap[0].index = 115;
        Check(fixture.Read().count == 0, "bone map index beyond live bone count accepted");
        fixture.boneMap[0].index = 14; fixture.boneMap[0].hash ^= 1;
        Check(fixture.Read().count == 0, "bone map hash/token mismatch accepted");
        fixture.boneMap[0].hash = static_cast<std::int32_t>(headToken & 0x3fffffffu);
        fixture.mapHeader.active = 2; fixture.SyncMap();
        Check(fixture.Read().count == 0, "bone map active count mismatch accepted");
        fixture.mapHeader.active = 1; fixture.mapHeader.buckets = 7; fixture.SyncMap();
        Check(fixture.Read().count == 0, "non-power-of-two bone map buckets accepted");
        fixture.mapHeader.buckets = 8; fixture.mapHeader.allocation = 4; fixture.SyncMap();
        Check(fixture.Read().count == 0, "bone map capacity smaller than buckets accepted");
        fixture.mapHeader.allocation = 8; fixture.mapHeader.entries = UINTPTR_MAX - 4; fixture.SyncMap();
        Check(fixture.Read().count == 0, "overflowing bone map span accepted");
        fixture.mapHeader.entries = 0x10000; fixture.SyncMap();
        Check(fixture.Read().count == 0, "unmapped bone map span accepted");
    }

    void NoGuessedLayout()
    {
        Fixture fixture;
        // Zero allocation flags mean no native set data, despite retained or
        // tempting inline/legacy pointers that older guessed readers accepted.
        Put(fixture.mesh1, 0x164, std::uint32_t{0x80000000});
        Put(fixture.mesh1, 0x150, At(fixture.set1));
        Put(fixture.mesh1, 0x190, 1); Put(fixture.mesh1, 0x198, At(fixture.hitboxes));
        Check(fixture.Read().count == 0, "reader invented inline or legacy set storage");
        Put(fixture.mesh1, 0x164, std::uint32_t{1});
        Check(fixture.Read().count == 1, "schema-handle rejection fixture was not valid initially");
        Put(fixture.scene, stateOffset + handleOffset, Address{});
        Put(fixture.scene, 0x1e0, At(fixture.binding)); Put(fixture.scene, 0x210, At(fixture.binding));
        Check(fixture.Read().count == 0, "reader ignored schema model handle in favor of hardcoded offsets");
        Put(fixture.scene, stateOffset + handleOffset, At(fixture.binding));
        fixture.Activate();
        Check(fixture.Read().count == 1, "missing-schema rejection fixture was not valid initially");
        missingState = true;
        Check(fixture.Read().count == 0, "missing model-state schema accepted");
        missingState = false; missingHandle = true;
        Check(fixture.Read().count == 0, "missing model-handle schema accepted");
    }

    void InvalidMetadata()
    {
        auto reject = [](auto mutate)
        {
            Fixture fixture; mutate(fixture);
            Check(fixture.Read().count == 0, "malformed native metadata accepted");
        };
        reject([](Fixture& f) { Put(f.model, 0x70, 65); });
        reject([](Fixture& f) { Put(f.model, 0x70, -1); });
        reject([](Fixture& f) { Put(f.model, 0x78, UINTPTR_MAX - 4); });
        reject([](Fixture& f) { Put(f.model, 0x178, 1025); });
        reject([](Fixture& f) { Put(f.model, 0x178, 0); });
        reject([](Fixture& f) { Put(f.mesh1, 0x174, 33); });
        reject([](Fixture& f) { Put(f.mesh1, 0x174, 2); }); // Allocated capacity is only one.
        reject([](Fixture& f) { Put(f.mesh1, 0x168, Address{0x10000}); });
        reject([](Fixture& f) { Put(f.set1, 0x28, 257); });
        reject([](Fixture& f) { Put(f.set1, 0x28, -1); });
        reject([](Fixture& f) { Put(f.set1, 0x30, UINTPTR_MAX - 4); });
        reject([](Fixture& f) { Put(f.hitboxes, 0x30, -1.f); });
        reject([](Fixture& f) { Put(f.hitboxes, 0x30, 101.f); });
        reject([](Fixture& f) { Put(f.hitboxes, 0x30, std::numeric_limits<float>::quiet_NaN()); });
        reject([](Fixture& f) { Put(f.hitboxes, 0x18, combat_access::Vec3{std::numeric_limits<float>::infinity(), 0, 0}); });
        Fixture missingStarts;
        Put(missingStarts.model, 0x238, 1);
        const auto named = missingStarts.Read();
        Check(named.count == 1 && named.entries[0].bone == 14, "missing selector remap start read unrelated slot instead of bone-name fallback");
        Fixture bounded;
        Put(bounded.set1, 0x28, 32);
        for (unsigned i = 0; i < 32; ++i) bounded.Hitbox(i, 7, headToken);
        const auto many = bounded.Read();
        Check(many.count == 20 && many.entries[19].index == 19, "output exceeded bounded hitbox storage or lost ordinals");
        Fixture changed;
        changingHandle = At(changed.scene) + stateOffset + handleOffset;
        Check(changed.Read().count == 0 && handleReads == 2, "model resource changed during reads without invalidating snapshot");
        Check(combat_access::ReadHitboxes(0).count == 0 && combat_access::ReadHitboxes(0, false).count == 0,
              "null scene read escaped guards");
    }
}

int main()
{
    try
    {
        NativeLayout(); BoneNames(); NoGuessedLayout(); InvalidMetadata();
        std::cout << "HVH hitboxes regression: PASS\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "HVH hitboxes regression: FAIL: " << error.what() << '\n';
        return 1;
    }
}
