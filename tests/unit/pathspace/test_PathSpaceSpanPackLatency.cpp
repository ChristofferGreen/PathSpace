#include "PathSpacePodFastpathTestSupport.hpp"

#include <chrono>

TEST_SUITE_BEGIN("pathspace.pod.fastpath.span_pack.latency");

TEST_CASE("span pack take does not stall on spaces that never saw a pack insert") {
    PathSpace space;
    // Ordinary inserts only: the lanes exist, but no pack insert ever ran in this space.
    REQUIRE(space.insert("/pair/a", 1).errors.empty());
    REQUIRE(space.insert("/pair/b", 2).errors.empty());

    constexpr int Calls = 60;
    auto const    start = std::chrono::steady_clock::now();
    for (int i = 0; i < Calls; ++i) {
        auto take = space.take<"a", "b">("/pair", [&](std::span<int> a, std::span<int> b) {
            REQUIRE(a.size() == 1);
            REQUIRE(b.size() == 1);
            return false;
        });
        REQUIRE(take.has_value());
    }
    auto const elapsed = std::chrono::steady_clock::now() - start;

    // Each call used to sleep 5 x 1 ms waiting for a "first pack insert" that was never going to
    // come: at least 300 ms for these calls. Without the wait they take microseconds.
    CHECK(elapsed < std::chrono::milliseconds(150));
}

TEST_SUITE_END();
