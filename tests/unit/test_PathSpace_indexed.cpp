#include "third_party/doctest.h"
#include <pathspace/PathSpace.hpp>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

using namespace SP;
using namespace std::chrono_literals;

TEST_SUITE_BEGIN("pathspace.indexed");

TEST_CASE("Indexed read[0] matches front and preserves queue") {
    PathSpace space;
    for (int i = 0; i < 3; ++i) {
        CHECK(space.insert("/ints", i).nbrValuesInserted == 1);
    }

    auto val0 = space.read<int>("/ints[0]");
    REQUIRE(val0.has_value());
    CHECK(val0.value() == 0);

    // Front still intact
    auto front = space.read<int>("/ints");
    REQUIRE(front.has_value());
    CHECK(front.value() == 0);
}

TEST_CASE("Indexed read in the middle returns correct element without popping") {
    PathSpace space;
    for (int i = 0; i < 5; ++i) {
        CHECK(space.insert("/ints", i).nbrValuesInserted == 1);
    }

    auto mid = space.read<int>("/ints[3]");
    REQUIRE(mid.has_value());
    CHECK(mid.value() == 3);

    // Queue remains unchanged (pop to verify ordering)
    std::vector<int> snapshot;
    for (int i = 0; i < 5; ++i) {
        auto v = space.take<int>("/ints");
        REQUIRE(v.has_value());
        snapshot.push_back(v.value());
    }
    CHECK(snapshot == std::vector<int>({0, 1, 2, 3, 4}));
}

TEST_CASE("Indexed take removes only the targeted element") {
    PathSpace space;
    for (int i = 0; i < 6; ++i) {
        CHECK(space.insert("/ints", i).nbrValuesInserted == 1);
    }

    auto target = space.take<int>("/ints[4]");
    REQUIRE(target.has_value());
    CHECK(target.value() == 4);

    std::vector<int> remaining;
    for (int i = 0; i < 5; ++i) {
        auto v = space.take<int>("/ints");
        if (v.has_value()) {
            remaining.push_back(v.value());
        }
    }
    CHECK(remaining == std::vector<int>({0, 1, 2, 3, 5}));
}

TEST_CASE("Indexed take at zero behaves like normal pop") {
    PathSpace space;
    CHECK(space.insert("/ints", 10).nbrValuesInserted == 1);
    CHECK(space.insert("/ints", 11).nbrValuesInserted == 1);

    auto first = space.take<int>("/ints[0]");
    REQUIRE(first.has_value());
    CHECK(first.value() == 10);

    auto second = space.take<int>("/ints");
    REQUIRE(second.has_value());
    CHECK(second.value() == 11);
}

TEST_CASE("Indexed read returns no value when index out of range") {
    PathSpace space;
    CHECK(space.insert("/ints", 1).nbrValuesInserted == 1);

    auto missing = space.read<int>("/ints[5]");
    CHECK_FALSE(missing.has_value());
    CHECK(missing.error().code == Error::Code::NoObjectFound);
}

TEST_CASE("Indexed take returns no value on empty path") {
    PathSpace space;
    auto missing = space.take<int>("/empty[0]");
    CHECK_FALSE(missing.has_value());
    CHECK(missing.error().code == Error::Code::NoSuchPath);
}

TEST_CASE("Indexed read skips nested space front matter") {
    PathSpace space;
    auto nested = std::make_unique<PathSpace>();
    CHECK(space.insert("/mixed", std::move(nested)).nbrSpacesInserted == 1);
    CHECK(space.insert("/mixed", 42).nbrValuesInserted == 1);
    CHECK(space.insert("/mixed", 43).nbrValuesInserted == 1);

    auto val = space.read<int>("/mixed[0]");
    REQUIRE(val.has_value());
    CHECK(val.value() == 42);
}

TEST_CASE("Indexed read skips different types and finds matching occurrence") {
    PathSpace space;
    CHECK(space.insert("/mixed", 1).nbrValuesInserted == 1);
    CHECK(space.insert("/mixed", std::string("first")).nbrValuesInserted == 1);
    CHECK(space.insert("/mixed", 2).nbrValuesInserted == 1);
    CHECK(space.insert("/mixed", std::string("second")).nbrValuesInserted == 1);

    auto wrongType = space.read<std::string>("/mixed[0]");
    CHECK_FALSE(wrongType.has_value());
    CHECK(wrongType.error().code == Error::Code::InvalidType);

    auto str0 = space.read<std::string>("/mixed[1]");
    REQUIRE(str0.has_value());
    CHECK(str0.value() == "first");

    auto str1 = space.read<std::string>("/mixed[3]");
    REQUIRE(str1.has_value());
    CHECK(str1.value() == "second");
}

TEST_CASE("Invalid indexed component rejects path") {
    PathSpace space;
    CHECK(space.insert("/ints", 1).nbrValuesInserted == 1);

    auto bad = space.read<int>("/ints[a]");
    CHECK_FALSE(bad.has_value());
    CHECK(bad.error().code == Error::Code::InvalidPath);
}

TEST_CASE("Multiple indexed takes compact correctly across runs") {
    PathSpace space;
    for (int i = 0; i < 8; ++i) {
        CHECK(space.insert("/ints", i).nbrValuesInserted == 1);
    }

    // Remove two middle elements
    auto first = space.take<int>("/ints[2]");
    auto second = space.take<int>("/ints[3]");
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first.value() == 2);
    CHECK(second.value() == 4); // original index shifted after first removal

    std::vector<int> remaining;
    for (int i = 0; i < 6; ++i) {
        auto v = space.take<int>("/ints");
        if (v.has_value()) remaining.push_back(v.value());
    }
    CHECK(remaining == std::vector<int>({0, 1, 3, 5, 6, 7}));
}

TEST_CASE("Indexed read blocks until value arrives") {
    PathSpace space;
    std::thread producer([&space]() {
        std::this_thread::sleep_for(50ms);
        space.insert("/ints", 99);
    });
    auto ret = space.read<int>("/ints[0]", Block{200ms});
    producer.join();
    REQUIRE(ret.has_value());
    CHECK(ret.value() == 99);
}

TEST_CASE("Indexed read skips execution front to reach data") {
    PathSpace space;
    std::function<int()> f = []() { return 7; };
    CHECK(space.insert("/mix", f, In{.executionCategory = ExecutionCategory::Lazy}).nbrTasksInserted == 1);
    CHECK(space.insert("/mix", 123).nbrValuesInserted == 1);

    auto val = space.read<int>("/mix[0]", Block{});
    REQUIRE(val.has_value());
    CHECK(val.value() == 123);

    // execution still present
    auto exec = space.read<int>("/mix", Block{});
    CHECK(exec.has_value());
    CHECK(exec.value() == 7);
}

TEST_CASE("Indexed take with nested space present ignores nested for value indexing") {
    PathSpace space;
    auto nested = std::make_unique<PathSpace>();
    CHECK(space.insert("/mixed", std::move(nested)).nbrSpacesInserted == 1);
    CHECK(space.insert("/mixed", 10).nbrValuesInserted == 1);
    CHECK(space.insert("/mixed", 11).nbrValuesInserted == 1);

    auto val = space.take<int>("/mixed[1]");
    REQUIRE(val.has_value());
    CHECK(val.value() == 11);
}

TEST_CASE("Indexed read supports last element lookup") {
    PathSpace space;
    for (int i = 0; i < 4; ++i) {
        CHECK(space.insert("/ints", i).nbrValuesInserted == 1);
    }
    auto val = space.read<int>("/ints[3]");
    REQUIRE(val.has_value());
    CHECK(val.value() == 3);
}

TEST_CASE("Indexed read with very large index returns no object") {
    PathSpace space;
    CHECK(space.insert("/ints", 1).nbrValuesInserted == 1);
    auto val = space.read<int>("/ints[9999]");
    CHECK_FALSE(val.has_value());
    CHECK(val.error().code == Error::Code::NoObjectFound);
}

TEST_SUITE_END();
