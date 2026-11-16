#pragma once
#include "groups.hpp"
#include <cstdint>
#include <optional>

namespace npm::mobile {
enum class Heat { normal, serious, critical };
struct Conditions {
    uint64_t available_bytes;
    Heat heat;
    bool low_power;
    bool memory_warning;
};
struct Recommendation {
    int max_default_frames;
    unsigned row_workers;
    bool confirm(int frames) const { return frames == 32 || frames > max_default_frames; }
};

inline Recommendation recommend(Conditions c) {
    // App estimates based on measured peaks (~500 / 1281 MiB) plus headroom.
    // Apple's available-memory reading is advisory; zero means unknown or exhausted.
    constexpr uint64_t mib = 1024 * 1024;
    if (c.memory_warning || c.heat == Heat::critical || c.available_bytes < 640 * mib)
        return {4, 1};
    if (c.heat == Heat::serious) return {8, 1};
    if (c.low_power || c.available_bytes < 1536 * mib) return {8, 2};
    return {16, 4};
}

// Only choose whole, validated camera sequences. A 32-only folder starts unselected.
inline std::optional<size_t> preferred_group(const std::vector<Group>& groups,
                                             const std::vector<std::string>& problems,
                                             Recommendation advice) {
    std::optional<size_t> selected;
    for (size_t i = 0; i < groups.size(); ++i) {
        if (problems.at(i).empty() && groups[i].expected <= advice.max_default_frames &&
            (!selected || groups[i].expected > groups[*selected].expected)) selected = i;
    }
    return selected;
}
}
