#include "core/BlockOptions.hpp"
#include "third_party/doctest.h"

#include <chrono>

using namespace std::chrono_literals;

TEST_SUITE("core.block_options") {
TEST_CASE("BlockOptions defaults to non-blocking with no timeout") {
    BlockOptions opts{};
    CHECK(opts.behavior == BlockOptions::Behavior::DontWait);
    CHECK_FALSE(opts.timeout.has_value());
}

TEST_CASE("BlockOptions captures wait behavior and timeout") {
    BlockOptions opts{};
    opts.behavior = BlockOptions::Behavior::Wait;
    opts.timeout  = 5ms;

    CHECK(opts.behavior == BlockOptions::Behavior::Wait);
    REQUIRE(opts.timeout.has_value());
    CHECK(*opts.timeout == 5ms);
}
}
