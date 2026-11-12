#include "PathSpace.hpp"
#include "task/Future.hpp"
#include "task/IFutureAny.hpp"
#include "third_party/doctest.h"

#include <atomic>
#include <string>
#include <typeinfo>

using namespace SP;

namespace {
// peekFuture/typedPeekFuture are protected; expose them the way the other header tests do.
class FutureProbeSpace : public PathSpace {
public:
    using PathSpace::PathSpace;
    using PathSpace::peekFuture;
    using PathSpace::typedPeekFuture;
};
} // namespace

TEST_SUITE("pathspace.future.handles") {

TEST_CASE("peekFuture of a lazy task is valid, not ready, and does not start the task") {
    FutureProbeSpace  space;
    std::atomic<int> runs{0};

    REQUIRE(space.insert("/jobs/lazy",
                         [&runs]() -> int {
                             runs.fetch_add(1);
                             return 11;
                         },
                         In{.executionCategory = ExecutionCategory::Lazy})
                    .errors.empty());

    auto legacy = space.peekFuture("/jobs/lazy");
    REQUIRE(legacy.has_value());
    CHECK(legacy->valid());
    CHECK_FALSE(legacy->ready());

    int out = -1;
    CHECK_FALSE(legacy->try_copy_result_to(&out));
    CHECK(out == -1);
    CHECK(runs.load() == 0);
}

TEST_CASE("read<FutureAny> is non-blocking and consistent across repeated reads") {
    PathSpace        space;
    std::atomic<int> runs{0};

    REQUIRE(space.insert("/jobs/typed",
                         [&runs]() -> int {
                             runs.fetch_add(1);
                             return 21;
                         },
                         In{.executionCategory = ExecutionCategory::Lazy})
                    .errors.empty());

    auto first = space.read("/jobs/typed");
    REQUIRE(first.has_value());
    CHECK(first->valid());
    CHECK(first->type() == typeid(int));
    CHECK_FALSE(first->ready());

    int out = -1;
    CHECK_FALSE(first->try_copy_to(&out));
    CHECK(out == -1);

    auto second = space.read("/jobs/typed");
    REQUIRE(second.has_value());
    CHECK(second->shared_state() == first->shared_state());
    CHECK(runs.load() == 0);
}

TEST_CASE("a FutureAny peeked before execution observes the result after a blocking read") {
    PathSpace space;

    REQUIRE(space.insert("/jobs/lazy", []() -> int { return 42; }, In{.executionCategory = ExecutionCategory::Lazy})
                    .errors.empty());

    auto handle = space.read("/jobs/lazy");
    REQUIRE(handle.has_value());
    CHECK_FALSE(handle->ready());

    auto value = space.read<int>("/jobs/lazy", Block{});
    REQUIRE(value.has_value());
    CHECK(*value == 42);

    CHECK(handle->ready());
    int out = 0;
    CHECK(handle->try_copy_to(&out));
    CHECK(out == 42);

    out = 0;
    CHECK(handle->copy_to(&out));
    CHECK(out == 42);
}

TEST_CASE("legacy Future and typed FutureAny agree once an eager task completes") {
    FutureProbeSpace space;

    REQUIRE(space.insert("/jobs/eager", []() -> int { return 7; }, In{.executionCategory = ExecutionCategory::Immediate})
                    .errors.empty());

    auto legacy = space.peekFuture("/jobs/eager");
    auto typed  = space.typedPeekFuture("/jobs/eager");
    REQUIRE(legacy.has_value());
    REQUIRE(typed.has_value());
    CHECK(legacy->valid());
    CHECK(typed->valid());

    // Both wait on completion events, not on time.
    legacy->wait();
    typed->wait();
    CHECK(legacy->ready());
    CHECK(typed->ready());

    int legacyOut = 0;
    int typedOut  = 0;
    CHECK(legacy->try_copy_result_to(&legacyOut));
    CHECK(typed->try_copy_to(&typedOut));
    CHECK(legacyOut == 7);
    CHECK(typedOut == 7);
}

TEST_CASE("typed future reports the task result type") {
    PathSpace space;

    REQUIRE(space.insert("/jobs/text", []() -> std::string { return "hello"; }, In{.executionCategory = ExecutionCategory::Lazy})
                    .errors.empty());
    auto handle = space.read("/jobs/text");
    REQUIRE(handle.has_value());
    CHECK(handle->type() == typeid(std::string));
}

TEST_CASE("future reads fail cleanly for plain values, missing paths and globs") {
    PathSpace space;
    REQUIRE(space.insert("/jobs/value", 3).errors.empty());
    REQUIRE(space.insert("/jobs/exec", []() -> int { return 1; }, In{.executionCategory = ExecutionCategory::Lazy})
                    .errors.empty());

    auto plain = space.read("/jobs/value");
    REQUIRE_FALSE(plain.has_value());
    CHECK(plain.error().code == Error::Code::NoObjectFound);

    auto missing = space.read("/jobs/missing");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == Error::Code::NoObjectFound);

    auto glob = space.read("/jobs/*");
    CHECK_FALSE(glob.has_value());
}

TEST_CASE("a held FutureAny stays usable after the execution is taken") {
    PathSpace space;

    REQUIRE(space.insert("/jobs/once", []() -> int { return 5; }, In{.executionCategory = ExecutionCategory::Lazy})
                    .errors.empty());
    auto handle = space.read("/jobs/once");
    REQUIRE(handle.has_value());

    auto taken = space.take<int>("/jobs/once", Block{});
    REQUIRE(taken.has_value());
    CHECK(*taken == 5);

    int out = 0;
    CHECK(handle->copy_to(&out));
    CHECK(out == 5);

    auto after = space.read("/jobs/once");
    CHECK_FALSE(after.has_value());
}

} // TEST_SUITE
