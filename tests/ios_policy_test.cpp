#include "PerformancePolicy.hpp"
#include <array>
#include <stdexcept>

using namespace npm;
using namespace npm::mobile;

int main() {
    constexpr uint64_t mib = 1024 * 1024;
    struct Case { Conditions conditions; int frames; unsigned workers; };
    for (const auto& test : std::array<Case, 9>{{
        {{0, Heat::normal, false, false}, 4, 1},
        {{639*mib, Heat::normal, false, false}, 4, 1},
        {{640*mib, Heat::normal, false, false}, 8, 2},
        {{1535*mib, Heat::normal, false, false}, 8, 2},
        {{1536*mib, Heat::normal, false, false}, 16, 4},
        {{4096*mib, Heat::normal, true, false}, 8, 2},
        {{4096*mib, Heat::serious, false, false}, 8, 1},
        {{4096*mib, Heat::critical, true, false}, 4, 1},
        {{4096*mib, Heat::normal, false, true}, 4, 1},
    }}) {
        auto policy = recommend(test.conditions);
        if (policy.max_default_frames != test.frames || policy.row_workers != test.workers ||
            !policy.confirm(32) || policy.confirm(test.frames) || !policy.confirm(test.frames*2))
            throw std::runtime_error("Unexpected resource policy or confirmation");
        std::vector<Group> groups(4);
        for (size_t i = 0; i < groups.size(); ++i) groups[i].expected = 4 << i;
        auto selected = preferred_group(groups, {"","","",""}, policy);
        if (!selected || groups[*selected].expected != test.frames)
            throw std::runtime_error("Incorrect default sequence");
        if (preferred_group(groups, {"missing","missing","missing",""}, policy))
            throw std::runtime_error("32 frames must require manual selection");
    }
}
