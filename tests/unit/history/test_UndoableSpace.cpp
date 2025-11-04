#include "history/UndoableSpace.hpp"

#include "PathSpace.hpp"
#include "path/ConcretePath.hpp"
#include "third_party/doctest.h"

using namespace SP;
using namespace SP::History;
using SP::ConcretePathStringView;

namespace {

auto makeUndoableSpace() -> std::unique_ptr<UndoableSpace> {
    auto inner = std::make_unique<PathSpace>();
    return std::make_unique<UndoableSpace>(std::move(inner));
}

} // namespace

TEST_SUITE("UndoableSpace") {

TEST_CASE("undo/redo round trip") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    auto enable = space->enableHistory(ConcretePathStringView{"/doc"});
    REQUIRE(enable.has_value());

    auto insertResult = space->insert("/doc/title", std::string{"alpha"});
    REQUIRE(insertResult.errors.empty());

    auto value = space->read<std::string>("/doc/title");
    REQUIRE(value.has_value());
    CHECK(value->c_str() == std::string{"alpha"});

    REQUIRE(space->undo(ConcretePathStringView{"/doc"}).has_value());
    auto missing = space->read<std::string>("/doc/title");
    CHECK_FALSE(missing.has_value());

    REQUIRE(space->redo(ConcretePathStringView{"/doc"}).has_value());
    auto restored = space->read<std::string>("/doc/title");
    REQUIRE(restored.has_value());
    CHECK(restored->c_str() == std::string{"alpha"});
}

TEST_CASE("transaction batching produces single history entry") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    REQUIRE(space->enableHistory(ConcretePathStringView{"/items"}).has_value());

    {
        auto txExpected = space->beginTransaction(ConcretePathStringView{"/items"});
        REQUIRE(txExpected.has_value());
        auto tx = std::move(txExpected.value());

        auto firstInsert = space->insert("/items/a", 1);
        REQUIRE(firstInsert.errors.empty());
        auto secondInsert = space->insert("/items/b", 2);
        REQUIRE(secondInsert.errors.empty());

        REQUIRE(tx.commit().has_value());
    }

    auto stats = space->getHistoryStats(ConcretePathStringView{"/items"});
    REQUIRE(stats.has_value());
    CHECK(stats->undoCount == 1);

    REQUIRE(space->undo(ConcretePathStringView{"/items"}).has_value());
    CHECK_FALSE(space->read<int>("/items/a").has_value());
    CHECK_FALSE(space->read<int>("/items/b").has_value());
}

}
