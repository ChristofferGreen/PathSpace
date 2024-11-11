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

    SUBCASE("Simple PathSpace Execution Lazy") {
        std::function<int()> f = []() -> int { return 58; };
        CHECK(pspace.insert("/f", f, InOptions{.execution = ExecutionOptions{.category = ExecutionOptions::Category::Lazy}}).nbrTasksCreated
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
        CustomStruct cs{756, "test"};
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
        pspace.insert("/int", 756);
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
        pspace.insert("/deep/nested/path", 756);
        auto ret = pspace.extract<int>("/deep/nested/path");
        REQUIRE(ret.has_value());
        CHECK(ret.value() == 756);
    }

    SUBCASE("Extract with blocking") {
        std::thread t([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            pspace.insert("/delayed", 756);
        });

        auto ret = pspace.extractBlock<int>("/delayed");
        REQUIRE(ret.has_value());
        CHECK(ret.value() == 756);

        t.join();
    }

    SUBCASE("Extract with timeout") {
        auto ret = pspace.extractBlock<int>(
                "/timeout",
                OutOptions{.block = BlockOptions{.behavior = BlockOptions::Behavior::Wait, .timeout = std::chrono::milliseconds(100)}});
        CHECK_FALSE(ret.has_value());
    }

    SUBCASE("Extract after clear") {
        pspace.insert("/clear_test", 756);
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

    SUBCASE("PathSpace Extract Behavior") {
        SUBCASE("Debug Single Value Lifecycle") {
            PathSpace pspace;
            REQUIRE(pspace.insert("/test", 756).errors.empty());

            // Verify read doesn't remove value
            auto readVal = pspace.readBlock<int>("/test");
            REQUIRE(readVal.has_value());
            CHECK(readVal.value() == 756);
            MESSAGE("After initial read: value=" << readVal.value());

            // Extract should remove value
            auto extractVal = pspace.extractBlock<int>("/test");
            REQUIRE(extractVal.has_value());
            CHECK(extractVal.value() == 756);
            MESSAGE("After extract: value=" << extractVal.value());

            // Verify value is gone using non-blocking read
            auto readAfterExtract = pspace.read<int>("/test");
            MESSAGE("After final read: has_value=" << readAfterExtract.has_value());
            if (readAfterExtract.has_value()) {
                MESSAGE("Unexpected value still present: " << readAfterExtract.value());
            }
            CHECK_FALSE(readAfterExtract.has_value());
        }

        SUBCASE("FIFO Order with Multiple Values") {
            PathSpace pspace;
            REQUIRE(pspace.insert("/test", 1).errors.empty());
            REQUIRE(pspace.insert("/test", 2).errors.empty());
            REQUIRE(pspace.insert("/test", 3).errors.empty());

            // Values should be extracted in insertion order
            auto val1 = pspace.extractBlock<int>("/test");
            REQUIRE(val1.has_value());
            CHECK(val1.value() == 1);

            auto val2 = pspace.extractBlock<int>("/test");
            REQUIRE(val2.has_value());
            CHECK(val2.value() == 2);

            auto val3 = pspace.extractBlock<int>("/test");
            REQUIRE(val3.has_value());
            CHECK(val3.value() == 3);

            // Verify empty using non-blocking read
            auto emptyCheck = pspace.read<int>("/test");
            CHECK_FALSE(emptyCheck.has_value());
        }

        SUBCASE("Path Isolation") {
            PathSpace pspace;
            REQUIRE(pspace.insert("/path1", 10).errors.empty());
            REQUIRE(pspace.insert("/path2", 20).errors.empty());

            // Extract from first path
            auto val1 = pspace.extractBlock<int>("/path1");
            REQUIRE(val1.has_value());
            CHECK(val1.value() == 10);

            // Second path should be unaffected
            auto val2 = pspace.readBlock<int>("/path2");
            REQUIRE(val2.has_value());
            CHECK(val2.value() == 20);

            // First path should be empty
            auto check1 = pspace.read<int>("/path1");
            CHECK_FALSE(check1.has_value());

            // Extract from second path
            auto val3 = pspace.extractBlock<int>("/path2");
            REQUIRE(val3.has_value());
            CHECK(val3.value() == 20);

            // Both paths should be empty
            auto check2 = pspace.read<int>("/path1");
            auto check3 = pspace.read<int>("/path2");
            CHECK_FALSE(check2.has_value());
            CHECK_FALSE(check3.has_value());
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
        std::pair<int, std::string> pair = {756, "answer"};
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
        std::optional<int> opt = 756;
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
