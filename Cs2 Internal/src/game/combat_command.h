#pragma once
#include "game/sdk/CombatBindings.h"
#include <array>
#include <optional>

namespace combat_command
{
    using Address = std::uintptr_t;
    struct IO
    {
        bool (*read)(void *, Address, std::size_t);
        bool (*write)(Address, const void *, std::size_t);
        Address (*allocateButtons)(Address);
        bool (*crc)(Address, Address, const std::uint8_t *, int);
    };
    struct Shot
    {
        std::array<float, 3> angles{};
        int tick{};
        float fraction{};
        std::optional<combat_bindings::BacktrackTiming> backtrack;
    };
    struct Movement
    {
        std::optional<std::array<float, 2>> analog;
        std::optional<std::array<float, 3>> angles;
        // Replace preceding analog impulses while retaining native button events.
        bool replaceAnalog{};
        // Spectral's final movement pass clears base analogs once a complete
        // non-quantized subtick path carries the movement for this command.
        bool zeroBaseAnalog{};
        // Previously committed movement messages still owned by this command.
        std::array<Address,64> replaceSteps{};
        std::size_t replaceCount{};
        std::uint64_t clearButtons{}, setButtons{};
        std::array<combat_bindings::SubtickPayload, 64> steps{};
        std::size_t count{};
    };
    struct MovementIO
    {
        IO command;
        // Returns a raw native message and appends it to the live repeated field.
        Address (*acquireSubtick)(Address field){};
        std::array<Address,64> *committedSubticks{};
    };
    bool EditMovement(Address command, Address base, int commandNumber, const Movement &, const MovementIO &);
    enum class AngleMode { ShotHistory, BaseOnly };
    enum class FireButton { Primary, Secondary };
    // Edits command messages only. Camera and mouse input are never written here.
    bool Edit(Address command, Address base, int commandNumber,
              const std::optional<std::array<float, 3>> &aim, bool pressAttack, const IO &io,
              const std::optional<Shot> &shot = std::nullopt,
              AngleMode mode = AngleMode::ShotHistory,
              FireButton fire = FireButton::Primary);
}
