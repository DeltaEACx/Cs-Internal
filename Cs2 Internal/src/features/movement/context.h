#pragma once
#include "features/movement/detail/movement_policy.hpp"
#include "game/combat_access.h"
#include "game/combat_command.h"
namespace movement::native {
struct Context {
    game_access::LocalPlayer local;
    combat_access::Address input{}, command{}, base{}, services{};
    combat_bindings::BaseCommand baseSnapshot{};
    combat_bindings::ButtonState buttons{};
    movement_policy::Input state{};
    std::uint64_t mask{};
    bool valid{}, hasMovementSubticks{}, jumpPressedFromButtons{}, jumpHeldFromButtons{}, jumpScrolled{};
    const char *failure{"Waiting for a game command."};
};
Context Read(std::uintptr_t input, bool shotRequested);
void Reset();
bool Fresh(const Context&);
std::optional<movement_policy::Trace> TraceHull(const Context&,movement_policy::Vec3 start,movement_policy::Vec3 end,
                                               movement_policy::Vec3 mins,movement_policy::Vec3 maxs);
combat_command::Address AcquireSubtick(combat_command::Address field);
}
