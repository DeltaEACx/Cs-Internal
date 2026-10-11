#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace econ_schema
{
    struct Snapshot
    {
        bool available = false;
        std::vector<int> itemDefIds;
        std::string status;
    };

    // Read only. Does not invoke game functions or modify game memory. Callers
    // should cache snapshots on their renderer thread, for example once a second.
    Snapshot Read();
} // namespace econ_schema
