#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/layer/BoundedPathSpace.hpp>

#include <memory>
#include <string>

using namespace SP;

TEST_SUITE_BEGIN("pathspace.bounded.counts");

TEST_CASE("the bound recovers when the queue was drained through the backing space") {
    auto             backing = std::make_shared<PathSpace>();
    BoundedPathSpace bounded{backing, 2};

    REQUIRE(bounded.insert("/queue", 1).errors.empty());
    REQUIRE(bounded.insert("/queue", 2).errors.empty());
    // Consumers of the backing space empty the queue; the wrapper's count still says "full".
    REQUIRE(backing->take<int>("/queue").has_value());
    REQUIRE(backing->take<int>("/queue").has_value());

    // Making room finds nothing to evict. That means there is room, not that the insert has to
    // be thrown away - otherwise the path stays unusable for good.
    auto ret = bounded.insert("/queue", 3);
    CHECK(ret.errors.empty());
    auto stored = backing->read<int>("/queue");
    REQUIRE(stored.has_value());
    CHECK(*stored == 3);
}

TEST_CASE("an indexed take through the wrapper is counted against its queue") {
    auto             backing = std::make_shared<PathSpace>();
    BoundedPathSpace bounded{backing, 2};

    REQUIRE(bounded.insert("/queue", 1).errors.empty());
    REQUIRE(bounded.insert("/queue", 2).errors.empty());
    auto firstByIndex = bounded.take<int>("/queue[0]");
    REQUIRE(firstByIndex.has_value());
    CHECK(*firstByIndex == 1);

    // One slot is free again, so this insert must not evict the 2.
    REQUIRE(bounded.insert("/queue", 3).errors.empty());
    auto a = backing->take<int>("/queue");
    REQUIRE(a.has_value());
    CHECK(*a == 2);
    auto b = backing->take<int>("/queue");
    REQUIRE(b.has_value());
    CHECK(*b == 3);
}

TEST_CASE("an insert dropped because nothing could be evicted is reported") {
    auto             backing = std::make_shared<PathSpace>();
    BoundedPathSpace bounded{backing, 1};

    REQUIRE(bounded.insert("/queue", std::string{"old"}).errors.empty());
    // The queue is full of strings; an int cannot evict one, so the int is not stored.
    auto ret = bounded.insert("/queue", 7);
    CHECK(ret.nbrValuesInserted == 0);
    CHECK_FALSE(ret.errors.empty());

    auto kept = backing->read<std::string>("/queue");
    REQUIRE(kept.has_value());
    CHECK(*kept == "old");
}

TEST_SUITE_END();
