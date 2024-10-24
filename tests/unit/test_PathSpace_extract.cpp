#include "ext/doctest.h"
#include <pathspace/PathSpace.hpp>

// Standard library containers
#include <array>
#include <deque>
#include <list>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Standard library utility types
#include <any>
#include <optional>
#include <tuple>
#include <utility> // for std::pair
#include <variant>

// Other standard library types
#include <bitset>

using namespace SP;

TEST_CASE("PathSpace Extract") {
    PathSpace pspace;
    SUBCASE("Simple PathSpace Extract") {
        CHECK(pspace.insert("/test", 56).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test", 58).nbrValuesInserted == 1);
        auto ret = pspace.extract<int>("/test");
        CHECK(ret.has_value());
        CHECK(ret.value() == 56);
        auto ret2 = pspace.extract<int>("/test");
        CHECK(ret2.has_value());
        CHECK(ret2.value() == 58);
    }

    SUBCASE("Extract Different Types") {
        CHECK(pspace.insert("/test1", 56.45f).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test2", std::string("hello")).nbrValuesInserted == 1);
        auto ret = pspace.extract<float>("/test1");
        CHECK(ret.has_value());
        CHECK(ret.value() == 56.45f);
        auto ret2 = pspace.extract<std::string>("/test2");
        CHECK(ret2.has_value());
        CHECK(ret2.value() == "hello");
    }

    SUBCASE("Extract Different Types Same Place") {
        CHECK(pspace.insert("/test", 56.45f).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test", std::string("hello")).nbrValuesInserted == 1);
        auto ret = pspace.extract<float>("/test");
        CHECK(ret.has_value());
        CHECK(ret.value() == 56.45f);
        auto ret2 = pspace.extract<std::string>("/test");
        CHECK(ret2.has_value());
        CHECK(ret2.value() == "hello");
    }

    SUBCASE("Deeper PathSpace Extract") {
        CHECK(pspace.insert("/test1/test2", 56).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test1/test2", 58).nbrValuesInserted == 1);
        auto ret = pspace.extract<int>("/test1/test2");
        CHECK(ret.has_value());
        CHECK(ret.value() == 56);
        auto ret2 = pspace.extract<int>("/test1/test2");
        CHECK(ret2.has_value());
        CHECK(ret2.value() == 58);
    }

    SUBCASE("Deeper PathSpace Extract Different Types") {
        CHECK(pspace.insert("/test1/test2", 56.45f).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test1/test2", 'a').nbrValuesInserted == 1);
        CHECK(pspace.insert("/test1/test2", 34.5f).nbrValuesInserted == 1);
        auto ret = pspace.extract<float>("/test1/test2");
        CHECK(ret.has_value());
        CHECK(ret.value() == 56.45f);
        auto ret2 = pspace.extract<char>("/test1/test2");
        CHECK(ret2.has_value());
        CHECK(ret2.value() == 'a');
        auto ret3 = pspace.extract<float>("/test1/test2");
        CHECK(ret3.has_value());
        CHECK(ret3.value() == 34.5f);
    }

    SUBCASE("Simple PathSpace Execution Non-Immediate") {
        std::function<int()> f = []() -> int { return 58; };
        CHECK(pspace.insert("/f", f, InOptions{.execution = ExecutionOptions{.category = ExecutionOptions::Category::OnReadOrExtract}})
                      .nbrTasksCreated
              == 1);
        CHECK(pspace.extractBlock<int>("/f").value() == 58);
        CHECK(!pspace.extract<int>("/f").has_value());
    }
}

TEST_CASE("PathSpace Extract Extended Tests") {
    PathSpace pspace;

    SUBCASE("Extract std::string") {
        pspace.insert("/str", std::string("hello world"));
        auto ret = pspace.extract<std::string>("/str");
        REQUIRE(ret.has_value());
        CHECK(ret.value() == "hello world");
    }

    SUBCASE("Extract std::vector") {
        std::vector<int> vec = {1, 2, 3, 4, 5};
        pspace.insert("/vec", vec);
        auto ret = pspace.extract<std::vector<int>>("/vec");
        REQUIRE(ret.has_value());
        CHECK(ret.value() == vec);
    }

    SUBCASE("Extract std::map") {
        std::map<std::string, int> map = {{"one", 1}, {"two", 2}, {"three", 3}};
        pspace.insert("/map", map);
        auto ret = pspace.extract<std::map<std::string, int>>("/map");
        REQUIRE(ret.has_value());
        CHECK(ret.value() == map);
    }

    SUBCASE("Extract custom struct") {
        struct CustomStruct {
            int x;
            std::string y;
            bool operator==(const CustomStruct& other) const {
                return x == other.x && y == other.y;
            }
        };
        CustomStruct cs{42, "test"};
        pspace.insert("/custom", cs);
        auto ret = pspace.extract<CustomStruct>("/custom");
        REQUIRE(ret.has_value());
        CHECK(ret.value() == cs);
    }

    SUBCASE("Extract from non-existent path") {
        auto ret = pspace.extract<int>("/non_existent");
        CHECK_FALSE(ret.has_value());
    }

    SUBCASE("Extract with type mismatch") {
        pspace.insert("/int", 42);
        auto ret = pspace.extract<std::string>("/int");
        CHECK_FALSE(ret.has_value());
    }

    SUBCASE("Extract multiple times") {
        pspace.insert("/multi", 1);
        pspace.insert("/multi", 2);
        pspace.insert("/multi", 3);

        auto ret1 = pspace.extract<int>("/multi");
        auto ret2 = pspace.extract<int>("/multi");
        auto ret3 = pspace.extract<int>("/multi");
        auto ret4 = pspace.extract<int>("/multi");

        REQUIRE(ret1.has_value());
        REQUIRE(ret2.has_value());
        REQUIRE(ret3.has_value());
        CHECK_FALSE(ret4.has_value());

        CHECK(ret1.value() == 1);
        CHECK(ret2.value() == 2);
        CHECK(ret3.value() == 3);
    }

    SUBCASE("Extract with deep path") {
        pspace.insert("/deep/nested/path", 42);
        auto ret = pspace.extract<int>("/deep/nested/path");
        REQUIRE(ret.has_value());
        CHECK(ret.value() == 42);
    }

    SUBCASE("Extract lambda function") {
        auto lambda = []() -> int { return 42; };
        pspace.insert("/lambda", lambda, InOptions{.execution = ExecutionOptions{.category = ExecutionOptions::Category::OnReadOrExtract}});
        auto ret = pspace.extractBlock<std::function<int()>>("/lambda");
        REQUIRE(ret.has_value());
        CHECK(ret.value()() == 42);
    }

    SUBCASE("Extract with blocking") {
        std::thread t([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            pspace.insert("/delayed", 42);
        });

        auto ret = pspace.extractBlock<int>("/delayed");
        REQUIRE(ret.has_value());
        CHECK(ret.value() == 42);

        t.join();
    }

    SUBCASE("Extract with timeout") {
        auto ret = pspace.extractBlock<int>(
                "/timeout",
                OutOptions{.block = BlockOptions{.behavior = BlockOptions::Behavior::Wait, .timeout = std::chrono::milliseconds(100)}});
        CHECK_FALSE(ret.has_value());
    }

    SUBCASE("Extract after clear") {
        pspace.insert("/clear_test", 42);
        pspace.clear();
        auto ret = pspace.extract<int>("/clear_test");
        CHECK_FALSE(ret.has_value());
    }

    /*SUBCASE("Extract with periodic execution") {
        int counter = 0;
        auto periodic_func = [&counter]() -> int { return ++counter; };
        pspace.insert("/periodic",
                      periodic_func,
                      InOptions{.execution = ExecutionOptions{.category = ExecutionOptions::Category::PeriodicOnRead,
                                                              .updateInterval = std::chrono::milliseconds(50)}});

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        auto ret1 = pspace.extract<int>("/periodic");
        auto ret2 = pspace.extract<int>("/periodic");

        REQUIRE(ret1.has_value());
        REQUIRE(ret2.has_value());
        CHECK(ret1.value() > 0);
        CHECK(ret2.value() > ret1.value());
    }*/

    SUBCASE("Basic Extract Behavior") {
        PathSpace pspace;

        // Test 1: Single value insert and extract
        INFO("Testing single value extract");
        REQUIRE(pspace.insert("/test1", 42).errors.empty());

        {
            auto readVal = pspace.readBlock<int>("/test1");
            REQUIRE(readVal.has_value());
            CHECK(readVal.value() == 42);

            auto extractVal = pspace.extractBlock<int>("/test1");
            REQUIRE(extractVal.has_value());
            CHECK(extractVal.value() == 42);

            // Verify value is gone
            auto readAfterExtract = pspace.readBlock<int>("/test1");
            CHECK_FALSE(readAfterExtract.has_value());
        }

        // Test 2: Multiple values in sequence
        INFO("Testing sequential extracts");
        REQUIRE(pspace.insert("/test2", 1).errors.empty());
        REQUIRE(pspace.insert("/test2", 2).errors.empty());
        REQUIRE(pspace.insert("/test2", 3).errors.empty());

        {
            // Should extract in FIFO order
            auto val1 = pspace.extractBlock<int>("/test2");
            REQUIRE(val1.has_value());
            CHECK(val1.value() == 1);

            auto val2 = pspace.extractBlock<int>("/test2");
            REQUIRE(val2.has_value());
            CHECK(val2.value() == 2);

            auto val3 = pspace.extractBlock<int>("/test2");
            REQUIRE(val3.has_value());
            CHECK(val3.value() == 3);

            // Should be empty now
            auto val4 = pspace.extractBlock<int>("/test2");
            CHECK_FALSE(val4.has_value());
        }

        // Test 3: Mix of reads and extracts
        INFO("Testing read/extract mix");
        REQUIRE(pspace.insert("/test3", 100).errors.empty());
        REQUIRE(pspace.insert("/test3", 200).errors.empty());

        {
            // Read first value (shouldn't remove it)
            auto read1 = pspace.readBlock<int>("/test3");
            REQUIRE(read1.has_value());
            CHECK(read1.value() == 100);

            // Extract first value (should remove it)
            auto extract1 = pspace.extractBlock<int>("/test3");
            REQUIRE(extract1.has_value());
            CHECK(extract1.value() == 100);

            // Read second value
            auto read2 = pspace.readBlock<int>("/test3");
            REQUIRE(read2.has_value());
            CHECK(read2.value() == 200);

            // Extract second value
            auto extract2 = pspace.extractBlock<int>("/test3");
            REQUIRE(extract2.has_value());
            CHECK(extract2.value() == 200);

            // Should be empty
            auto read3 = pspace.readBlock<int>("/test3");
            CHECK_FALSE(read3.has_value());
            auto extract3 = pspace.extractBlock<int>("/test3");
            CHECK_FALSE(extract3.has_value());
        }

        // Test 4: Extract all with while loop
        INFO("Testing extract all loop");
        REQUIRE(pspace.insert("/test4", 1).errors.empty());
        REQUIRE(pspace.insert("/test4", 2).errors.empty());
        REQUIRE(pspace.insert("/test4", 3).errors.empty());

        {
            std::vector<int> extracted;
            while (auto val = pspace.extractBlock<int>("/test4")) {
                extracted.push_back(val.value());
            }

            REQUIRE(extracted.size() == 3);
            CHECK(extracted[0] == 1);
            CHECK(extracted[1] == 2);
            CHECK(extracted[2] == 3);

            // Verify really empty
            auto read = pspace.readBlock<int>("/test4");
            CHECK_FALSE(read.has_value());
        }

        // Test 5: Different paths shouldn't interfere
        INFO("Testing path isolation");
        REQUIRE(pspace.insert("/test5a", 10).errors.empty());
        REQUIRE(pspace.insert("/test5b", 20).errors.empty());

        {
            // Extract from first path
            auto extract1 = pspace.extractBlock<int>("/test5a");
            REQUIRE(extract1.has_value());
            CHECK(extract1.value() == 10);

            // Second path should still have its value
            auto read2 = pspace.readBlock<int>("/test5b");
            REQUIRE(read2.has_value());
            CHECK(read2.value() == 20);

            // Extract from second path
            auto extract2 = pspace.extractBlock<int>("/test5b");
            REQUIRE(extract2.has_value());
            CHECK(extract2.value() == 20);

            // Both should be empty
            CHECK_FALSE(pspace.readBlock<int>("/test5a").has_value());
            CHECK_FALSE(pspace.readBlock<int>("/test5b").has_value());
        }
    }
}

TEST_CASE("PathSpace Extract Std Datastructure") {
    PathSpace pspace;

    SUBCASE("PathSpace Extract std::string") {
        pspace.insert("/string", std::string("hello"));
        auto val = pspace.extract<std::string>("/string");
        CHECK(val.has_value());
        CHECK(val.value() == "hello");
        auto val2 = pspace.extract<std::string>("/string");
        CHECK_FALSE(val2.has_value());
    }

    SUBCASE("PathSpace Extract std::vector") {
        std::vector<int> vec = {1, 2, 3, 4, 5};
        pspace.insert("/vector", vec);
        auto val = pspace.extract<std::vector<int>>("/vector");
        CHECK(val.has_value());
        CHECK(val.value() == vec);
        auto val2 = pspace.extract<std::vector<int>>("/vector");
        CHECK_FALSE(val2.has_value());
    }

    SUBCASE("PathSpace Extract std::array") {
        std::array<double, 3> arr = {1.1, 2.2, 3.3};
        pspace.insert("/array", arr);
        auto val = pspace.extract<std::array<double, 3>>("/array");
        CHECK(val.has_value());
        CHECK(val.value() == arr);
        auto val2 = pspace.extract<std::array<double, 3>>("/array");
        CHECK_FALSE(val2.has_value());
    }

    SUBCASE("PathSpace Extract std::map") {
        std::map<std::string, int> map = {{"one", 1}, {"two", 2}, {"three", 3}};
        pspace.insert("/map", map);
        auto val = pspace.extract<std::map<std::string, int>>("/map");
        CHECK(val.has_value());
        CHECK(val.value() == map);
        auto val2 = pspace.extract<std::map<std::string, int>>("/map");
        CHECK_FALSE(val2.has_value());
    }

    SUBCASE("PathSpace Extract std::unordered_map") {
        std::unordered_map<std::string, double> umap = {{"pi", 3.14}, {"e", 2.71}};
        pspace.insert("/umap", umap);
        auto val = pspace.extract<std::unordered_map<std::string, double>>("/umap");
        CHECK(val.has_value());
        CHECK(val.value() == umap);
        auto val2 = pspace.extract<std::unordered_map<std::string, double>>("/umap");
        CHECK_FALSE(val2.has_value());
    }

    SUBCASE("PathSpace Extract std::set") {
        std::set<char> set = {'a', 'b', 'c', 'd'};
        pspace.insert("/set", set);
        auto val = pspace.extract<std::set<char>>("/set");
        CHECK(val.has_value());
        CHECK(val.value() == set);
        auto val2 = pspace.extract<std::set<char>>("/set");
        CHECK_FALSE(val2.has_value());
    }

    SUBCASE("PathSpace Extract std::unordered_set") {
        std::unordered_set<int> uset = {1, 2, 3, 4, 5};
        pspace.insert("/uset", uset);
        auto val = pspace.extract<std::unordered_set<int>>("/uset");
        CHECK(val.has_value());
        CHECK(val.value() == uset);
        auto val2 = pspace.extract<std::unordered_set<int>>("/uset");
        CHECK_FALSE(val2.has_value());
    }

    SUBCASE("PathSpace Extract std::pair") {
        std::pair<int, std::string> pair = {42, "answer"};
        pspace.insert("/pair", pair);
        auto val = pspace.extract<std::pair<int, std::string>>("/pair");
        CHECK(val.has_value());
        CHECK(val.value() == pair);
        auto val2 = pspace.extract<std::pair<int, std::string>>("/pair");
        CHECK_FALSE(val2.has_value());
    }

    SUBCASE("PathSpace Extract std::tuple") {
        std::tuple<int, double, char> tuple = {1, 3.14, 'a'};
        pspace.insert("/tuple", tuple);
        auto val = pspace.extract<std::tuple<int, double, char>>("/tuple");
        CHECK(val.has_value());
        CHECK(val.value() == tuple);
        auto val2 = pspace.extract<std::tuple<int, double, char>>("/tuple");
        CHECK_FALSE(val2.has_value());
    }

    SUBCASE("PathSpace Extract std::optional") {
        std::optional<int> opt = 42;
        pspace.insert("/optional", opt);
        auto val = pspace.extract<std::optional<int>>("/optional");
        CHECK(val.has_value());
        CHECK(val.value() == opt);
        auto val2 = pspace.extract<std::optional<int>>("/optional");
        CHECK_FALSE(val2.has_value());
    }

    SUBCASE("PathSpace Extract std::variant") {
        std::variant<int, double, std::string> var = "hello";
        pspace.insert("/variant", var);
        auto val = pspace.extract<std::variant<int, double, std::string>>("/variant");
        CHECK(val.has_value());
        CHECK(val.value() == var);
        auto val2 = pspace.extract<std::variant<int, double, std::string>>("/variant");
        CHECK_FALSE(val2.has_value());
    }

    SUBCASE("PathSpace Extract std::bitset") {
        std::bitset<8> bits(0b10101010);
        pspace.insert("/bitset", bits);
        auto val = pspace.extract<std::bitset<8>>("/bitset");
        CHECK(val.has_value());
        CHECK(val.value() == bits);
        auto val2 = pspace.extract<std::bitset<8>>("/bitset");
        CHECK_FALSE(val2.has_value());
    }

    SUBCASE("PathSpace Extract std::deque") {
        std::deque<int> deq = {1, 2, 3, 4, 5};
        pspace.insert("/deque", deq);
        auto val = pspace.extract<std::deque<int>>("/deque");
        CHECK(val.has_value());
        CHECK(val.value() == deq);
        auto val2 = pspace.extract<std::deque<int>>("/deque");
        CHECK_FALSE(val2.has_value());
    }

    SUBCASE("PathSpace Extract std::list") {
        std::list<std::string> lst = {"one", "two", "three"};
        pspace.insert("/list", lst);
        auto val = pspace.extract<std::list<std::string>>("/list");
        CHECK(val.has_value());
        CHECK(val.value() == lst);
        auto val2 = pspace.extract<std::list<std::string>>("/list");
        CHECK_FALSE(val2.has_value());
    }
}
