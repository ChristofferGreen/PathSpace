#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>

#include <span>
#include <vector>

using namespace SP;

namespace {
std::vector<int>* gObserved = nullptr;

void capture_ints(std::span<const int> ints) {
    if (!gObserved) {
        return;
    }
    gObserved->assign(ints.begin(), ints.end());
}

void increment_ints(std::span<int> ints) {
    for (auto& value : ints) {
        ++value;
    }
}
} // namespace

TEST_SUITE("pathspace.span.callback") {
TEST_CASE("Span read accepts function pointer callbacks") {
    PathSpace space;
    CHECK(space.insert("/ints", 1).errors.empty());
    CHECK(space.insert("/ints", 2).errors.empty());
    CHECK(space.insert("/ints", 3).errors.empty());

    std::vector<int> observed;
    gObserved = &observed;
    auto result = space.read("/ints", &capture_ints);
    gObserved = nullptr;

    REQUIRE(result.has_value());
    CHECK(observed == std::vector<int>{1, 2, 3});
}

TEST_CASE("Span read accepts compile-time path with function pointer") {
    PathSpace space;
    CHECK(space.insert("/ints", 7).errors.empty());
    CHECK(space.insert("/ints", 8).errors.empty());

    std::vector<int> observed;
    gObserved = &observed;
    auto result = space.read<"/ints">(&capture_ints);
    gObserved = nullptr;

    REQUIRE(result.has_value());
    CHECK(observed == std::vector<int>{7, 8});
}

TEST_CASE("Span take accepts function pointer mutators") {
    PathSpace space;
    CHECK(space.insert("/ints", 4).errors.empty());
    CHECK(space.insert("/ints", 5).errors.empty());

    auto mutResult = space.take("/ints", &increment_ints);
    REQUIRE(mutResult.has_value());

    std::vector<int> observed;
    auto readResult = space.read("/ints", [&](std::span<const int> ints) {
        observed.assign(ints.begin(), ints.end());
    });

    REQUIRE(readResult.has_value());
    CHECK(observed == std::vector<int>{5, 6});
}

TEST_CASE("Span take accepts compile-time path with function pointer") {
    PathSpace space;
    CHECK(space.insert("/ints", 10).errors.empty());
    CHECK(space.insert("/ints", 11).errors.empty());

    auto mutResult = space.take<"/ints">(&increment_ints);
    REQUIRE(mutResult.has_value());

    std::vector<int> observed;
    auto readResult = space.read("/ints", [&](std::span<const int> ints) {
        observed.assign(ints.begin(), ints.end());
    });

    REQUIRE(readResult.has_value());
    CHECK(observed == std::vector<int>{11, 12});
}
}
