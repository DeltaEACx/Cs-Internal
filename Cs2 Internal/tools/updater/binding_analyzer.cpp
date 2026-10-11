#include "platform/windows.h"
#include "nlohmann/json.hpp"
#include "minhook/src/hde/hde64.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <cstring>

// Read-only instruction decoding. Uses the same decoder as the project's hooks.
int wmain(int argc, wchar_t **argv)
{
    using nlohmann::json;
    try
    {
        if (argc != 4) throw std::runtime_error("Usage: BindingAnalyzer module.dll RVA byte-count");
        const auto rva = std::stoul(argv[2], nullptr, 0), count = std::stoul(argv[3], nullptr, 0);
        if (!count || count > 65536) throw std::runtime_error("Decode range must be 1..65536 bytes");
        std::ifstream stream(std::filesystem::path(argv[1]), std::ios::binary);
        if (!stream) throw std::runtime_error("Cannot open module");
        std::vector<unsigned char> image((std::istreambuf_iterator<char>(stream)), {});
        const auto at = [&](std::size_t offset, std::size_t size) -> const unsigned char * {
            if (offset > image.size() || size > image.size() - offset) throw std::runtime_error("Truncated PE");
            return image.data() + offset;
        };
        IMAGE_DOS_HEADER dos{}; std::memcpy(&dos, at(0, sizeof(dos)), sizeof(dos));
        if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) throw std::runtime_error("Invalid DOS header");
        IMAGE_NT_HEADERS64 nt{}; std::memcpy(&nt, at(dos.e_lfanew, sizeof(nt)), sizeof(nt));
        if (nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            throw std::runtime_error("Expected PE64");
        std::size_t raw = image.size();
        for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i)
        {
            IMAGE_SECTION_HEADER section{};
            const auto offset = dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
                                nt.FileHeader.SizeOfOptionalHeader + i * sizeof(section);
            std::memcpy(&section, at(offset, sizeof(section)), sizeof(section));
            if (rva >= section.VirtualAddress && rva - section.VirtualAddress < section.SizeOfRawData &&
                count <= section.SizeOfRawData - (rva - section.VirtualAddress))
                raw = section.PointerToRawData + rva - section.VirtualAddress;
        }
        const auto bytes = at(raw, count);
        std::vector<unsigned char> padded(bytes, bytes + count); padded.resize(count + 16, 0);
        json instructions = json::array(); std::string pattern;
        const char *hex = "0123456789ABCDEF";
        unsigned decoded = 0;
        while (decoded < count)
        {
            hde64s instruction{};
            hde64_disasm(padded.data() + decoded, &instruction);
            if (!instruction.len || instruction.flags & F_ERROR || decoded + instruction.len > count) break;
            const unsigned imm = ((instruction.flags & F_IMM8) ? 1 : 0) +
                                 ((instruction.flags & F_IMM16) ? 2 : 0) +
                                 ((instruction.flags & F_IMM32) ? 4 : 0) +
                                 ((instruction.flags & F_IMM64) ? 8 : 0);
            int operand = -1, width = 0; std::string kind; std::int64_t target = -1;
            if (instruction.flags & F_RELATIVE)
            {
                width = imm; operand = instruction.len - imm; kind = instruction.opcode == 0xE8 ? "call" : "branch";
                if (width == 4) target = std::int64_t(rva) + decoded + instruction.len + std::int32_t(instruction.imm.imm32);
                else if (width == 1) target = std::int64_t(rva) + decoded + instruction.len + std::int8_t(instruction.imm.imm8);
                else throw std::runtime_error("Unsupported relative instruction");
            }
            else if ((instruction.flags & F_MODRM) && instruction.modrm_mod == 0 && instruction.modrm_rm == 5 &&
                     !instruction.p_67 && (instruction.flags & F_DISP32))
            {
                width = 4; operand = instruction.len - imm - width; kind = "rip";
                target = std::int64_t(rva) + decoded + instruction.len + std::int32_t(instruction.disp.disp32);
            }
            std::string literal;
            for (unsigned i = 0; i < instruction.len; ++i)
            {
                const auto byte = bytes[decoded + i]; literal += hex[byte >> 4]; literal += hex[byte & 15];
                if (operand >= 0 && int(i) >= operand && int(i) < operand + width) pattern += "??";
                else { pattern += hex[byte >> 4]; pattern += hex[byte & 15]; }
            }
            instructions.push_back({{"offset", decoded}, {"length", instruction.len}, {"bytes", literal},
                                    {"kind", kind}, {"operand", operand}, {"width", width}, {"target", target}});
            decoded += instruction.len;
        }
        // Undecodable tails stay exact. They can never be silently treated as wildcards.
        for (unsigned i = decoded; i < count; ++i) { pattern += hex[bytes[i] >> 4]; pattern += hex[bytes[i] & 15]; }
        std::cout << json({{"pattern", pattern}, {"decoded", decoded}, {"length", count},
                          {"instructions", instructions}}).dump();
        return 0;
    }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
