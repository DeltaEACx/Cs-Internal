#pragma once
#include <cstdint>
#include <atomic>

namespace hvh_input {
inline std::atomic<unsigned> worldEpoch{1};
inline void LevelChanged() noexcept { worldEpoch.fetch_add(1, std::memory_order_acq_rel); }
enum class ActivationMode { Hold, Always };
inline bool Activated(bool enabled, bool gameplay, ActivationMode mode, bool keyDown) noexcept {
    return enabled && gameplay &&
        (mode == ActivationMode::Always || (mode == ActivationMode::Hold && keyDown));
}
struct CommandStamp {
    std::uintptr_t command{};
    int sequence{};
    std::uint32_t localHandle{0xffffffffu};
    bool subtickOverwrite{};
};

// Native CreateMove rebuilds an already processed command from camera input.
// Preserve only a command this instance actually committed, never an unrelated
// command that happens to carry the native overwrite marker.
class SilentCommand {
public:
    void Clear() { applied_ = {}; }
    void Applied(CommandStamp command) { applied_ = command; }
    bool Matches(const CommandStamp& command) const {
        return applied_.command && applied_.sequence > 0 && applied_.localHandle != 0xffffffffu &&
            command.command == applied_.command && command.sequence == applied_.sequence &&
            command.localHandle == applied_.localHandle;
    }
    bool Preserve(const CommandStamp& command) const {
        return command.subtickOverwrite && Matches(command);
    }
private:
    CommandStamp applied_{};
};
}
