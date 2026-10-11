#pragma once
#include "features/combat/backtrack.h"
#include <deque>

namespace backtrack::detail {
inline constexpr std::size_t maxRecords = 16, maxPlayers = 128, maxBones = 1024;
inline bool ValidTime(float simulation, float now, float window) {
    return std::isfinite(simulation) && std::isfinite(now) && std::isfinite(window) &&
        simulation > 0 && window > 0 && simulation <= now + .001f && now - simulation <= window;
}
inline bool SameModel(const Record& a, const Record& b) {
    return a.epoch == b.epoch && a.generation == b.generation && a.list == b.list && a.localHandle == b.localHandle &&
        a.pawn == b.pawn && a.handle == b.handle && a.scene == b.scene && a.binding == b.binding &&
        a.model == b.model && a.team == b.team && a.bones.size() == b.bones.size();
}
class History {
public:
    std::deque<Snapshot> records;
    void Prune(float now, float window) {
        std::erase_if(records, [&](const Snapshot& record) { return !ValidTime(record->simulationTime, now, window); });
    }
    void Add(Snapshot record, float now, float window) {
        if (!record || !ValidTime(record->simulationTime, now, window)) { Prune(now, window); return; }
        if (!records.empty()) {
            const auto& previous = *records.front();
            if (!SameModel(previous, *record) || record->simulationTime < previous.simulationTime ||
                (record->origin - previous.origin).length_sqr() > 64.f * 64.f) records.clear();
        }
        Prune(now, window);
        if (!records.empty() && records.front()->simulationTime == record->simulationTime) return;
        records.push_front(std::move(record));
        while (records.size() > maxRecords) records.pop_back();
    }
};
}
