#pragma once
#include <cstdint>

// Economy layout audited against the client build in SkinBindings.h.
namespace econ_bindings
{
    inline constexpr std::uint32_t kClientVtableRva = 0x1C630F0u;
    inline constexpr std::uint32_t kDefinitionIdOffset = 16u;
    inline constexpr std::uint32_t kDefinitionMapOffset = 248u;
    inline constexpr std::uint32_t kGetterSlot = 129u;
    inline constexpr std::uint32_t kSchemaPointerOffset = 8u;
    inline constexpr std::uint32_t kSchemaVtableRva = 0x1C06EE0u;
    inline constexpr std::uint32_t kSource2ClientInstanceRva = 0x255F3E0u;
    inline constexpr std::uint32_t kSystemVtableRva = 0x1D1E920u;
} // namespace econ_bindings
