#pragma once
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string_view>
#include "game/sdk/client_dll.hpp"
#include "game/sdk/offsets.hpp"

namespace game_access
{
    using Address = std::uintptr_t;
    namespace field = cs2_dumper::schemas::client_dll;
    namespace offset = cs2_dumper::offsets::client_dll;
    // Published only after skin_changer verifies the installed client hash/ABI.
    inline std::atomic<Address> verifiedClient{0};
    // Gameplay readers need the audited client/input ABI, independently of
    // whether the optional cosmetic frame-stage hook could be installed.
    inline std::atomic<Address> verifiedGameplayClient{0};
    inline Address GameplayClient()
    {
        const auto client = verifiedGameplayClient.load(std::memory_order_acquire);
        return client ? client : verifiedClient.load(std::memory_order_acquire);
    }
    inline bool SameModelPath(std::string_view a, std::string_view b)
    {
        if (a.ends_with("_c"))
            a.remove_suffix(2);
        if (b.ends_with("_c"))
            b.remove_suffix(2);
        if (a.size() != b.size())
            return false;
        auto fold = [](char c) { return c == '\\' ? '/' : c >= 'A' && c <= 'Z' ? char(c + 'a' - 'A') : c; };
        for (std::size_t i = 0; i < a.size(); ++i)
            if (fold(a[i]) != fold(b[i]))
                return false;
        return true;
    }
    inline bool Valid(Address p, std::size_t n)
    {
        return p >= 0x10000 && p < 0x00007fffffff0000ull && n <= 0x00007fffffff0000ull - p;
    }
    __declspec(noinline) inline bool Copy(Address p, void *out, std::size_t n)
    {
        if (!Valid(p, n))
            return false;
        __try
        {
            std::memcpy(out, reinterpret_cast<void *>(p), n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
    __declspec(noinline) inline bool Store(Address p, const void *in, std::size_t n)
    {
        if (!Valid(p, n))
            return false;
        __try
        {
            std::memcpy(reinterpret_cast<void *>(p), in, n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
    template <class T> bool Read(Address p, T &v)
    {
        return Copy(p, &v, sizeof(v));
    }
    template <class T> bool Write(Address p, const T &v)
    {
        return Store(p, &v, sizeof(v));
    }
    inline Address Entity(Address list, unsigned index)
    {
        Address page = 0, entity = 0;
        if (!list || !index || index >= 0x7fff || !Read(list + 0x10 + 8 * (index >> 9), page) || !page ||
            !Read(page + 0x70 * (index & 511), entity) || !Valid(entity, 8))
            return 0;
        return entity;
    }
    inline Address Resolve(Address list, std::uint32_t handle)
    {
        const unsigned index = handle & 0x7fff;
        Address page = 0, entity = 0;
        std::uint32_t actual = 0;
        if (!list || handle == 0xffffffffu || !index || index >= 0x7fff ||
            !Read(list + 0x10 + 8 * (index >> 9), page) || !page)
            return 0;
        const Address entry = page + 0x70 * (index & 511);
        return Read(entry + 0x10, actual) && actual == handle && Read(entry, entity) && Valid(entity, 8) ? entity : 0;
    }
    enum class PlayerState
    {
        Unavailable,
        NoController,
        NoPawn,
        Dead,
        NoTeam,
        Alive
    };
    struct LocalPlayer
    {
        Address list = 0, controller = 0, pawn = 0;
        std::uint32_t handle = 0xffffffffu;
        std::uint8_t team = 0;
        PlayerState state = PlayerState::Unavailable;
    };
    inline LocalPlayer ReadLocalPlayer(Address base)
    {
        LocalPlayer player;
        if (!base)
            return player;
        player.state = PlayerState::NoController;
        if (!Read(base + offset::dwEntityList, player.list) || !player.list ||
            !Read(base + offset::dwLocalPlayerController, player.controller) || !player.controller)
            return player;
        player.state = PlayerState::NoPawn;
        if (!Read(player.controller + field::CCSPlayerController::m_hPlayerPawn, player.handle) ||
            !(player.pawn = Resolve(player.list, player.handle)))
            return player;
        Read(player.pawn + field::C_BaseEntity::m_iTeamNum, player.team);
        int health = 0;
        std::uint8_t life = 1;
        player.state = PlayerState::Dead;
        if (!Read(player.pawn + field::C_BaseEntity::m_iHealth, health) || health <= 0 ||
            !Read(player.pawn + field::C_BaseEntity::m_lifeState, life) || life != 0)
            return player;
        player.state = player.team == 2 || player.team == 3 ? PlayerState::Alive : PlayerState::NoTeam;
        return player;
    }
} // namespace game_access
