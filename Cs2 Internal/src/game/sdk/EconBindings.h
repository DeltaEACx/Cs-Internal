#pragma once
#include <cstdint>

// Economy layout audited against the client build in SkinBindings.h.
namespace econ_bindings
{
    inline constexpr std::uint32_t kClientVtableRva = 0x1C65030u;
    inline constexpr std::uint32_t kDefinitionIdOffset = 16u;
    inline constexpr std::uint32_t kDefinitionMapOffset = 248u;
    inline constexpr std::uint32_t kGetterSlot = 129u;
    inline constexpr std::uint32_t kSchemaPointerOffset = 8u;
    inline constexpr std::uint32_t kSchemaVtableRva = 0x1C08E78u;
    inline constexpr std::uint32_t kSource2ClientInstanceRva = 0x25613A0u;
    inline constexpr std::uint32_t kSystemVtableRva = 0x1D208A0u;
} // namespace econ_bindings
