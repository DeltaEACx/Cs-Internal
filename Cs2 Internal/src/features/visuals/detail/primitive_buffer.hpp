#pragma once
#include "features/visuals/engine.hpp"
#include "features/visuals/settings.hpp"

namespace visuals::chams::detail
{
    struct Primitive
    {
        std::byte pad00[0x18]{};
        std::uintptr_t scene{}, material{}, materialCopy{};
        std::byte pad30[0x20]{};
        std::uint32_t tint{};
        float opacity{};
        std::int32_t drawOrder{};
        std::byte pad5c[6]{};
        std::uint16_t flags{}, flags2{};
        std::byte pad66[10]{};
    };
    static_assert(sizeof(Primitive) == 0x70);
    static_assert(offsetof(Primitive, material) == 0x20);
    static_assert(offsetof(Primitive, materialCopy) == 0x28);
    static_assert(offsetof(Primitive, tint) == 0x50);
    struct Output
    {
        std::uintptr_t fixed{};
        std::int32_t fixedCapacity{}, fixedCount{}, overflowCount{};
        std::uint32_t reserved{};
        std::uintptr_t overflow{};
        std::int32_t overflowCapacity{};
        std::uint32_t allocationFlags{};
        int Count() const
        {
            constexpr int limit = 1 << 20;
            if (fixedCount < 0 || overflowCount < 0 || fixedCapacity < 0 || overflowCapacity < 0 ||
                fixedCount > fixedCapacity || overflowCount > overflowCapacity ||
                (fixedCount && !fixed) || (overflowCount && !overflow) ||
                fixedCount > limit || overflowCount > limit - fixedCount) return -1;
            return fixedCount + overflowCount;
        }
        std::uintptr_t At(int index) const
        {
            const auto total = Count();
            if (index < 0 || total < 0 || index >= total) return 0;
            const auto base = index < fixedCount ? fixed : overflow;
            const auto offset = static_cast<std::size_t>(index < fixedCount ? index : index - fixedCount) * sizeof(Primitive);
            if (!game_access::Valid(base, offset + sizeof(Primitive))) return 0;
            return base + offset;
        }
    };
    static_assert(sizeof(Output) == 0x28);
    static_assert(offsetof(Output, fixedCount) == 0xc);
    static_assert(offsetof(Output, overflowCount) == 0x10);
    static_assert(offsetof(Output, overflow) == 0x18);
    inline std::optional<Output> ReadOutput(std::uintptr_t address)
    {
        const auto output = memory::safe_read<Output>(address);
        return output && output->Count() >= 0 ? output : std::nullopt;
    }
    inline bool Replace(std::uintptr_t address, std::uintptr_t scene, std::uintptr_t material, color tint)
    {
        const auto original = memory::safe_read<Primitive>(address);
        if (!original || original->scene != scene || !material) return false;
        const std::uint32_t rgba = tint.r | (std::uint32_t{tint.g} << 8) | (std::uint32_t{tint.b} << 16) | (std::uint32_t{tint.a} << 24);
        if (memory::safe_write(address + offsetof(Primitive, material), material) &&
            memory::safe_write(address + offsetof(Primitive, materialCopy), material) &&
            memory::safe_write(address + offsetof(Primitive, tint), rgba)) return true;
        memory::safe_write(address, *original);
        return false;
    }
}
