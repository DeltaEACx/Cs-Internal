#include "platform/windows.h"
#include "game/econ_schema.h"
#include "game/sdk/interfaces.hpp"
#include "game/sdk/EconBindings.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace
{
    // The supported client hash is pinned in SkinBindings.h.
    // Slot 129 jumps to the lazy singleton getter. Decode its initialized path;
    // never invoke its allocation/initialization path from the render thread.
    constexpr std::size_t kEconGetterSlot = econ_bindings::kGetterSlot;
    constexpr std::uintptr_t kClientVtableRva = econ_bindings::kClientVtableRva;
    constexpr std::uintptr_t kEconSystemVtableRva = econ_bindings::kSystemVtableRva;
    constexpr std::uintptr_t kEconSchemaVtableRva = econ_bindings::kSchemaVtableRva;
    constexpr std::size_t kEconSchemaOffset = econ_bindings::kSchemaPointerOffset;
    constexpr std::size_t kDefinitionMapOffset = econ_bindings::kDefinitionMapOffset;
    constexpr std::size_t kDefinitionIdOffset = econ_bindings::kDefinitionIdOffset;
    constexpr int kMaximumDefinitions = 20000;
    constexpr std::size_t kMaximumSections = 96;
    constexpr unsigned kMaximumThunks = 2;

    struct DefinitionMap
    {
        int count;
        std::uint32_t flags;
        std::uintptr_t data;
        int freeList;
        int activeCount;
    };

    struct DefinitionNode
    {
        int key;
        int padding;
        std::uintptr_t value;
        int validity;
        int padding2;
    };

    static_assert(sizeof(std::uintptr_t) == 8, "CS2 economy snapshots require x64");
    static_assert(sizeof(DefinitionMap) == 24 && offsetof(DefinitionMap, data) == 8 &&
                  offsetof(DefinitionMap, activeCount) == 20);
    static_assert(sizeof(DefinitionNode) == 24 && offsetof(DefinitionNode, value) == 8 &&
                  offsetof(DefinitionNode, validity) == 16);

    struct ClientImage
    {
        std::uintptr_t base = 0;
        std::size_t size = 0;
        std::array<IMAGE_SECTION_HEADER, kMaximumSections> sections{};
        std::size_t sectionCount = 0;
    };

    bool IsReadableAddress(std::uintptr_t address, std::size_t length)
    {
        constexpr std::uintptr_t highestUserAddress = 0x00007fffffff0000ull;
        return address >= 0x10000 && address < highestUserAddress && length <= highestUserAddress - address;
    }

    // Destructor-free SEH helper; never place vectors/strings inside __try.
    __declspec(noinline) bool SafeCopy(std::uintptr_t address, void *output, std::size_t length)
    {
        if (!IsReadableAddress(address, length))
            return false;
        __try
        {
            std::memcpy(output, reinterpret_cast<const void *>(address), length);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    template <typename T> bool ReadValue(std::uintptr_t address, T &result)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        return SafeCopy(address, &result, sizeof(result));
    }

    bool Contains(const ClientImage &image, std::uintptr_t address, std::size_t length)
    {
        return address >= image.base && address - image.base < image.size &&
               length <= image.size - (address - image.base);
    }

    bool IsExecutableRange(const ClientImage &image, std::uintptr_t address, std::size_t length)
    {
        if (!Contains(image, address, length))
            return false;
        const auto rva = address - image.base;
        for (std::size_t index = 0; index < image.sectionCount; ++index)
        {
            const auto &section = image.sections[index];
            const std::size_t size = (std::max)(section.Misc.VirtualSize, section.SizeOfRawData);
            if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 && rva >= section.VirtualAddress &&
                rva - section.VirtualAddress < size && length <= size - (rva - section.VirtualAddress))
                return true;
        }
        return false;
    }

    bool ReadClientImage(std::uintptr_t base, ClientImage &image)
    {
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        if (!ReadValue(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
            dos.e_lfanew < static_cast<LONG>(sizeof(dos)) || dos.e_lfanew > 0x100000 ||
            !ReadValue(base + static_cast<std::uintptr_t>(dos.e_lfanew), nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
            nt.FileHeader.SizeOfOptionalHeader > 0x1000 || nt.FileHeader.NumberOfSections == 0 ||
            nt.FileHeader.NumberOfSections > kMaximumSections || nt.OptionalHeader.SizeOfImage < 0x1000 ||
            nt.OptionalHeader.SizeOfImage > 0x40000000)
            return false;

        image.base = base;
        image.size = nt.OptionalHeader.SizeOfImage;
        image.sectionCount = nt.FileHeader.NumberOfSections;
        const auto sectionAddress = base + static_cast<std::uintptr_t>(dos.e_lfanew) +
                                    offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
        const std::size_t sectionBytes = image.sectionCount * sizeof(IMAGE_SECTION_HEADER);
        return Contains(image, sectionAddress, sectionBytes) &&
               SafeCopy(sectionAddress, image.sections.data(), sectionBytes);
    }

    bool RelativeTarget(std::uintptr_t instruction, std::size_t length, std::int32_t displacement,
                        std::uintptr_t &result)
    {
        if (!IsReadableAddress(instruction, length))
            return false;
        const std::int64_t target =
            static_cast<std::int64_t>(instruction) + static_cast<std::int64_t>(length) + displacement;
        if (target < 0)
            return false;
        result = static_cast<std::uintptr_t>(target);
        return IsReadableAddress(result, sizeof(std::uintptr_t));
    }

    bool DecodeExistingSystem(const ClientImage &image, std::uintptr_t getter, std::uintptr_t &econSystem)
    {
        std::array<std::uintptr_t, kMaximumThunks + 1> visited{};
        for (unsigned hop = 0; hop <= kMaximumThunks; ++hop)
        {
            if (std::find(visited.begin(), visited.begin() + hop, getter) != visited.begin() + hop)
                return false;
            visited[hop] = getter;
            std::array<std::uint8_t, 8> code{};
            if (!IsExecutableRange(image, getter, code.size()) || !SafeCopy(getter, code.data(), code.size()))
                return false;

            if (code[0] == 0xe9)
            {
                std::int32_t displacement = 0;
                std::memcpy(&displacement, code.data() + 1, sizeof(displacement));
                if (hop == kMaximumThunks || !RelativeTarget(getter, 5, displacement, getter) ||
                    !Contains(image, getter, code.size()))
                    return false;
                continue;
            }

            if (code[0] == 0x48 && code[1] == 0x83 && code[2] == 0xec && code[3] == 0x28)
            {
                // Observed current singleton fast path:
                // sub rsp,28; mov rax,[rip+global]; test rax,rax; jnz return;
                // return: add rsp,28; ret. Cold initialization is never executed.
                std::array<std::uint8_t, 20> prefix{};
                if (!IsExecutableRange(image, getter, prefix.size()) ||
                    !SafeCopy(getter, prefix.data(), prefix.size()) || prefix[4] != 0x48 || prefix[5] != 0x8b ||
                    prefix[6] != 0x05 || prefix[11] != 0x48 || prefix[12] != 0x85 || prefix[13] != 0xc0 ||
                    prefix[14] != 0x0f || prefix[15] != 0x85)
                    return false;
                std::int32_t globalDisplacement = 0, returnDisplacement = 0;
                std::memcpy(&globalDisplacement, prefix.data() + 7, sizeof(globalDisplacement));
                std::memcpy(&returnDisplacement, prefix.data() + 16, sizeof(returnDisplacement));
                std::uintptr_t global = 0, returnAddress = 0;
                std::array<std::uint8_t, 5> epilogue{};
                constexpr std::array<std::uint8_t, 5> expected{0x48, 0x83, 0xc4, 0x28, 0xc3};
                if (!RelativeTarget(getter + 4, 7, globalDisplacement, global) ||
                    !Contains(image, global, sizeof(std::uintptr_t)) ||
                    !RelativeTarget(getter, prefix.size(), returnDisplacement, returnAddress) ||
                    !IsExecutableRange(image, returnAddress, epilogue.size()) ||
                    !SafeCopy(returnAddress, epilogue.data(), epilogue.size()) || epilogue != expected ||
                    !ReadValue(global, econSystem))
                    return false;
                return IsReadableAddress(econSystem, kEconSchemaOffset + sizeof(std::uintptr_t));
            }

            // Also recognize complete, side-effect-free RIP-relative leaf getters.
            if (code[0] != 0x48 || (code[1] != 0x8d && code[1] != 0x8b) || code[2] != 0x05 || code[7] != 0xc3)
                return false;
            std::int32_t displacement = 0;
            std::memcpy(&displacement, code.data() + 3, sizeof(displacement));
            std::uintptr_t target = 0;
            if (!RelativeTarget(getter, 7, displacement, target) || !Contains(image, target, sizeof(std::uintptr_t)))
                return false;
            if (code[1] == 0x8d)
            {
                if (!Contains(image, target, kEconSchemaOffset + sizeof(std::uintptr_t)))
                    return false;
                econSystem = target;
            }
            else if (!ReadValue(target, econSystem))
                return false;
            return IsReadableAddress(econSystem, kEconSchemaOffset + sizeof(std::uintptr_t));
        }
        return false;
    }

    econ_schema::Snapshot Unavailable(const char *reason)
    {
        econ_schema::Snapshot snapshot;
        snapshot.status = reason;
        return snapshot;
    }

    econ_schema::Snapshot ReadDefinitions(std::uintptr_t schema)
    {
        DefinitionMap before{};
        if (!IsReadableAddress(schema, kDefinitionMapOffset + sizeof(before)) ||
            !ReadValue(schema + kDefinitionMapOffset, before))
            return Unavailable("EconItemSchema memory is unreadable.");
        if (before.count <= 0 || before.count > kMaximumDefinitions || before.activeCount <= 0 ||
            before.activeCount > before.count || (before.flags & 0x7fffffffu) == 0 ||
            !IsReadableAddress(before.data, static_cast<std::size_t>(before.count) * sizeof(DefinitionNode)))
            return Unavailable("EconItemSchema item map is unrecognized or outside read bounds.");

        std::vector<DefinitionNode> nodes(static_cast<std::size_t>(before.count));
        if (!SafeCopy(before.data, nodes.data(), nodes.size() * sizeof(DefinitionNode)))
            return Unavailable("EconItemSchema item-map memory could not be read safely.");

        econ_schema::Snapshot snapshot;
        snapshot.itemDefIds.reserve(nodes.size());
        int activeNodes = 0;
        for (const auto &node : nodes)
        {
            if (node.validity < -1)
                continue;
            ++activeNodes;
            if (node.key == 0)
                continue;
            std::uint16_t definitionId = 0;
            if (node.key < 0 || node.key > 65535 ||
                !IsReadableAddress(node.value, kDefinitionIdOffset + sizeof(definitionId)) ||
                !ReadValue(node.value + kDefinitionIdOffset, definitionId) || definitionId == 0 ||
                definitionId != node.key)
                return Unavailable("EconItemSchema definition keys/layout did not pass consistency checks.");
            snapshot.itemDefIds.push_back(node.key);
        }

        DefinitionMap after{};
        if (!ReadValue(schema + kDefinitionMapOffset, after) || before.count != after.count ||
            before.flags != after.flags || before.data != after.data || before.activeCount != after.activeCount ||
            activeNodes != before.activeCount)
            return Unavailable("EconItemSchema item map changed during this read; retry later.");
        if (snapshot.itemDefIds.empty())
            return Unavailable("EconItemSchema yielded no validated positive item-definition IDs.");

        std::sort(snapshot.itemDefIds.begin(), snapshot.itemDefIds.end());
        snapshot.itemDefIds.erase(std::unique(snapshot.itemDefIds.begin(), snapshot.itemDefIds.end()),
                                  snapshot.itemDefIds.end());
        snapshot.available = true;
        snapshot.status = "Economy schema loaded.";
        return snapshot;
    }
} // namespace

econ_schema::Snapshot econ_schema::Read()
{

    const auto client = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"client.dll"));
    if (!client)
        return Unavailable("client.dll is unavailable; using the extracted catalog.");
    ClientImage image;
    if (!ReadClientImage(client, image))
        return Unavailable("client.dll image headers could not be validated safely.");

    const auto interfaceAddress = client + cs2_dumper::interfaces::client_dll::Source2Client002;
    std::uintptr_t vtable = 0, getter = 0, econSystem = 0, schema = 0, systemVtable = 0, schemaVtable = 0;
    if (!Contains(image, interfaceAddress, sizeof(std::uintptr_t)) || !ReadValue(interfaceAddress, vtable) ||
        vtable != client + kClientVtableRva ||
        !Contains(image, vtable, (kEconGetterSlot + 1) * sizeof(std::uintptr_t)) ||
        !ReadValue(vtable + kEconGetterSlot * sizeof(std::uintptr_t), getter))
        return Unavailable("Source2Client002 interface/getter layout is unrecognized; using the extracted catalog.");
    if (!DecodeExistingSystem(image, getter, econSystem))
        return Unavailable("Economy getter is unrecognized or its singleton is not initialized.");
    if (!ReadValue(econSystem, systemVtable) || systemVtable != client + kEconSystemVtableRva ||
        !ReadValue(econSystem + kEconSchemaOffset, schema) || !schema || !ReadValue(schema, schemaVtable) ||
        schemaVtable != client + kEconSchemaVtableRva)
        return Unavailable("Decoded economy object has no readable EconItemSchema pointer.");
    return ReadDefinitions(schema);
}
