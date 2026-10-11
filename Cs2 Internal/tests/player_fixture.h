#pragma once
#include "game/game_access.h"
#include "game/sdk/ViewBindings.h"
#include <vector>
#include <stdexcept>
inline void Check(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}
struct PlayerFixture
{
    std::vector<unsigned char> globals = std::vector<unsigned char>(0x2720000), list = std::vector<unsigned char>(0x210),
                               page = std::vector<unsigned char>(512 * 0x70),
                               controller = std::vector<unsigned char>(0x1000),
                               pawn = std::vector<unsigned char>(0x4000), input = std::vector<unsigned char>(0x1000);
    game_access::Address Base() const
    {
        return reinterpret_cast<game_access::Address>(globals.data());
    }
    game_access::Address Pawn() const
    {
        return reinterpret_cast<game_access::Address>(pawn.data());
    }
    template <class T> void Set(std::vector<unsigned char> &block, size_t offset, T value)
    {
        std::memcpy(block.data() + offset, &value, sizeof(value));
    }
    void Team(std::uint8_t team)
    {
        Set(pawn, game_access::field::C_BaseEntity::m_iTeamNum, team);
    }
    void Health(int health)
    {
        Set(pawn, game_access::field::C_BaseEntity::m_iHealth, health);
    }
    PlayerFixture()
    {
        Set(globals, game_access::offset::dwEntityList, reinterpret_cast<game_access::Address>(list.data()));
        Set(globals, game_access::offset::dwLocalPlayerController,
            reinterpret_cast<game_access::Address>(controller.data()));
        Set(globals, view_bindings::kInputPointer, reinterpret_cast<game_access::Address>(input.data()));
        Set(list, 0x10, reinterpret_cast<game_access::Address>(page.data()));
        Set(page, 0x70, Pawn());
        Set(page, 0x80, std::uint32_t{0x8001});
        Set(controller, game_access::field::CCSPlayerController::m_hPlayerPawn, std::uint32_t{0x8001});
        Team(2);
        Health(100);
    }
};
