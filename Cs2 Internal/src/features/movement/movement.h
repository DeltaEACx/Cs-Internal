#pragma once
#include <cstdint>
#include <optional>
namespace movement {
struct CameraStamp { std::uintptr_t command{}; int sequence{}; std::uint32_t localHandle{0xffffffffu}; };
std::optional<CameraStamp> Process(std::uintptr_t input, bool shotRequested=false);
// Countersteer an eligible grounded command after the caller rejects a shot
// for accuracy, or prepares a still-valid acquisition during weapon cooldown.
// Only a caller that has qualified a target before committing any shot may
// permit held primary input. A committed shot always takes priority.
// The returned stamp confirms a committed movement command, not a shot.
std::optional<CameraStamp> PrepareShot(std::uintptr_t input, bool allowHeldPrimary=false, bool shotCommitted=false);
// Refresh an existing movement snapshot after the caller's base-only AA pass.
void FinishCommand(std::uintptr_t input);
void Reset();
void SetInputState(bool menuOpen);
// A static-lifetime failure message, or nullptr when the shared input hook is ready.
void SetPipelineStatus(const char* failure);
void DrawMenu();
}
