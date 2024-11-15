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
        CHECK(pspace.insert("/f", f, InOptions{.execution = ExecutionOptions{.category = ExecutionOptions::Category::Lazy}}).nbrTasksInserted == 1);
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
            int         x;
            std::string y;
            bool        operator==(const CustomStruct& other) const {
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
        auto ret = pspace.extractBlock<int>("/timeout", OutOptions{.block = BlockOptions{.behavior = BlockOptions::Behavior::Wait, .timeout = std::chrono::milliseconds(100)}});
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

    SUBCASE("PathSpace Extract Behavior") {
        SUBCASE("Debug Single Value Lifecycle") {
            PathSpace pspace;
            REQUIRE(pspace.insert("/test", 42).errors.empty());

            // Verify read doesn't remove value
            auto readVal = pspace.readBlock<int>("/test");
            REQUIRE(readVal.has_value());
            CHECK(readVal.value() == 42);
            MESSAGE("After initial read: value=" << readVal.value());

            // Extract should remove value
            auto extractVal = pspace.extractBlock<int>("/test");
            REQUIRE(extractVal.has_value());
            CHECK(extractVal.value() == 42);
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

using namespace std::chrono_literals;

TEST_CASE("PathSpace Glob Operations") {
    PathSpace pspace;

    SUBCASE("Basic Glob Insert and Read") {
        // Insert values to multiple paths
        CHECK(pspace.insert("/test/a", 1).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test/b", 2).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test/c", 3).nbrValuesInserted == 1);

        // Insert to all matching paths using glob
        CHECK(pspace.insert("/test/*", 10).nbrValuesInserted == 3);

        // Verify values were appended in order
        auto val1 = pspace.extract<int>("/test/a");
        CHECK(val1.has_value());
        CHECK(val1.value() == 1);
        val1 = pspace.extract<int>("/test/a");
        CHECK(val1.has_value());
        CHECK(val1.value() == 10);

        auto val2 = pspace.extract<int>("/test/b");
        CHECK(val2.has_value());
        CHECK(val2.value() == 2);
        val2 = pspace.extract<int>("/test/b");
        CHECK(val2.has_value());
        CHECK(val2.value() == 10);

        auto val3 = pspace.extract<int>("/test/c");
        CHECK(val3.has_value());
        CHECK(val3.value() == 3);
        val3 = pspace.extract<int>("/test/c");
        CHECK(val3.has_value());
        CHECK(val3.value() == 10);
    }

    SUBCASE("Glob Insert with Different Data Types") {
        // Insert different types to different paths
        CHECK(pspace.insert("/data/int", 42).nbrValuesInserted == 1);
        CHECK(pspace.insert("/data/float", 3.14f).nbrValuesInserted == 1);
        CHECK(pspace.insert("/data/string", std::string("hello")).nbrValuesInserted == 1);

        // Insert to all using glob
        CHECK(pspace.insert("/data/*", 100).nbrValuesInserted == 3);

        // Verify original values can be extracted
        auto intVal = pspace.extract<int>("/data/int");
        CHECK(intVal.has_value());
        CHECK(intVal.value() == 42);

        auto floatVal = pspace.extract<float>("/data/float");
        CHECK(floatVal.has_value());
        CHECK(floatVal.value() == 3.14f);

        auto strVal = pspace.extract<std::string>("/data/string");
        CHECK(strVal.has_value());
        CHECK(strVal.value() == "hello");

        // Verify glob-inserted values
        intVal = pspace.extract<int>("/data/int");
        CHECK(intVal.has_value());
        CHECK(intVal.value() == 100);

        intVal = pspace.extract<int>("/data/float");
        CHECK(intVal.has_value());
        CHECK(intVal.value() == 100);

        intVal = pspace.extract<int>("/data/string");
        CHECK(intVal.has_value());
        CHECK(intVal.value() == 100);
    }

    SUBCASE("Nested Glob Patterns") {
        // Create nested structure
        CHECK(pspace.insert("/root/a/1", 1).nbrValuesInserted == 1);
        CHECK(pspace.insert("/root/a/2", 2).nbrValuesInserted == 1);
        CHECK(pspace.insert("/root/b/1", 3).nbrValuesInserted == 1);
        CHECK(pspace.insert("/root/b/2", 4).nbrValuesInserted == 1);

        // Insert using nested glob
        CHECK(pspace.insert("/root/*/1", 10).nbrValuesInserted == 2);

        // Verify values
        auto val1 = pspace.extract<int>("/root/a/1");
        CHECK(val1.has_value());
        CHECK(val1.value() == 1);
        val1 = pspace.extract<int>("/root/a/1");
        CHECK(val1.has_value());
        CHECK(val1.value() == 10);

        auto val2 = pspace.extract<int>("/root/b/1");
        CHECK(val2.has_value());
        CHECK(val2.value() == 3);
        val2 = pspace.extract<int>("/root/b/1");
        CHECK(val2.has_value());
        CHECK(val2.value() == 10);

        // Verify unaffected paths
        auto val3 = pspace.extract<int>("/root/a/2");
        CHECK(val3.has_value());
        CHECK(val3.value() == 2);

        auto val4 = pspace.extract<int>("/root/b/2");
        CHECK(val4.has_value());
        CHECK(val4.value() == 4);
    }

    SUBCASE("Glob with Lazy Executions loop") {
        std::atomic<int> execution_count{0};

        auto func = [&execution_count](int ret) {
            return [&execution_count, ret]() -> int {
                execution_count++;
                return ret;
            };
        };

        InOptions options{.execution = ExecutionOptions{.category = ExecutionOptions::Category::Lazy}};
        CHECK(pspace.insert("/exec/a", func(1), options).nbrTasksInserted == 1);
        CHECK(pspace.insert("/exec/a", func(10), options).nbrTasksInserted == 1);
        auto val1 = pspace.extractBlock<int>("/exec/a");
        val1      = pspace.readBlock<int>("/exec/a");
    }

    SUBCASE("Glob with Lazy Executions2") {
        std::atomic<int> execution_count{0};

        // Insert functions to multiple paths
        auto func = [&execution_count](int ret) {
            return [&execution_count, ret]() -> int {
                execution_count++;
                return ret;
            };
        };

        // Insert lazy executions
        InOptions options{.execution = ExecutionOptions{.category = ExecutionOptions::Category::Lazy}};
        CHECK(pspace.insert("/exec/a", func(1), options).nbrTasksInserted == 1);
        CHECK(pspace.insert("/exec/*", func(10), options).nbrTasksInserted == 1);
        auto val1 = pspace.extractBlock<int>("/exec/a");
        val1      = pspace.readBlock<int>("/exec/a");
    }

    SUBCASE("Glob with Lazy Executions") {
        std::atomic<int> execution_count{0};

        // Insert functions to multiple paths
        auto func = [&execution_count](int ret) {
            return [&execution_count, ret]() -> int {
                execution_count++;
                return ret;
            };
        };

        // Insert lazy executions
        InOptions options{.execution = ExecutionOptions{.category = ExecutionOptions::Category::Lazy}};
        CHECK(pspace.insert("/exec/a", func(1), options).nbrTasksInserted == 1);
        CHECK(pspace.insert("/exec/b", func(2), options).nbrTasksInserted == 1);
        CHECK(pspace.insert("/exec/c", func(3), options).nbrTasksInserted == 1);

        // Insert to all matching paths using glob
        CHECK(pspace.insert("/exec/*", func(10), options).nbrTasksInserted == 3);

        // Verify executions happen in order
        auto val1 = pspace.extractBlock<int>("/exec/a");
        CHECK(val1.has_value());
        CHECK(val1.value() == 1);
        val1 = pspace.readBlock<int>("/exec/a");
        /*CHECK(val1.has_value());
        CHECK(val1.value() == 10);

        auto val2 = pspace.extractBlock<int>("/exec/b");
        CHECK(val2.has_value());
        CHECK(val2.value() == 2);
        val2 = pspace.readBlock<int>("/exec/b");
        CHECK(val2.has_value());
        CHECK(val2.value() == 10);

        auto val3 = pspace.extractBlock<int>("/exec/c");
        CHECK(val3.has_value());
        CHECK(val3.value() == 3);
        val3 = pspace.readBlock<int>("/exec/c");
        CHECK(val3.has_value());
        CHECK(val3.value() == 10);

        int e = execution_count.load();
        CHECK(e == 6); // All executions should have run*/
    }

    SUBCASE("Complex Glob Patterns") {
        // Setup test paths
        CHECK(pspace.insert("/test/foo/data1", 1).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test/foo/data2", 2).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test/bar/data1", 3).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test/bar/data2", 4).nbrValuesInserted == 1);

        // Test different glob patterns
        SUBCASE("Multiple wildcards") {
            CHECK(pspace.insert("/test/*/data*", 10).nbrValuesInserted == 4);

            // Verify all paths were affected
            auto val = pspace.extract<int>("/test/foo/data1");
            CHECK(val.has_value());
            CHECK(val.value() == 1);
            val = pspace.extract<int>("/test/foo/data1");
            CHECK(val.has_value());
            CHECK(val.value() == 10);

            val = pspace.extract<int>("/test/bar/data2");
            CHECK(val.has_value());
            CHECK(val.value() == 4);
            val = pspace.extract<int>("/test/bar/data2");
            CHECK(val.has_value());
            CHECK(val.value() == 10);
        }

        SUBCASE("Specific suffix pattern") {
            CHECK(pspace.insert("/test/*/data1", 20).nbrValuesInserted == 2);

            // Verify only data1 paths were affected
            auto val = pspace.extract<int>("/test/foo/data1");
            CHECK(val.has_value());
            CHECK(val.value() == 1);
            val = pspace.extract<int>("/test/foo/data1");
            CHECK(val.has_value());
            CHECK(val.value() == 20);

            // data2 paths should be unaffected
            val = pspace.extract<int>("/test/foo/data2");
            CHECK(val.has_value());
            CHECK(val.value() == 2);
        }

        SUBCASE("Specific prefix pattern") {
            CHECK(pspace.insert("/test/foo/*", 30).nbrValuesInserted == 2);

            // Verify only foo paths were affected
            auto val = pspace.extract<int>("/test/foo/data1");
            CHECK(val.has_value());
            CHECK(val.value() == 1);
            val = pspace.extract<int>("/test/foo/data1");
            CHECK(val.has_value());
            CHECK(val.value() == 30);

            // bar paths should be unaffected
            val = pspace.extract<int>("/test/bar/data1");
            CHECK(val.has_value());
            CHECK(val.value() == 3);
        }
    }

    SUBCASE("Glob Pattern Edge Cases") {
        SUBCASE("Empty paths") {
            CHECK(pspace.insert("/*", 1).errors.size() > 0);
            CHECK(pspace.insert("/", 1).nbrValuesInserted == 1);
        }

        SUBCASE("Invalid glob patterns") {
            CHECK(pspace.insert("/test/[", 1).errors.size() > 0);
            CHECK(pspace.insert("/test/]", 1).errors.size() > 0);
        }

        SUBCASE("Escaped wildcards") {
            CHECK(pspace.insert("/test/a*b", 1).nbrValuesInserted == 1);
            CHECK(pspace.insert("/test/a\\*b", 2).nbrValuesInserted == 1);

            // Verify they're treated as different paths
            auto val = pspace.extract<int>("/test/a*b");
            CHECK(val.has_value());
            CHECK(val.value() == 1);

            val = pspace.extract<int>("/test/a*b");
            CHECK(!val.has_value()); // Should be empty now

            val = pspace.extract<int>("/test/a\\*b");
            CHECK(val.has_value());
            CHECK(val.value() == 2);
        }
    }
}

TEST_CASE("PathSpace String Operations") {
    PathSpace pspace;

    SUBCASE("Basic String Operations") {
        // Test inserting various string types
        SUBCASE("String Literal") {
            CHECK(pspace.insert("/lit1", "hello").nbrValuesInserted == 1);

            auto val1 = pspace.read<std::string>("/lit1");
            CHECK(val1.has_value());
            CHECK(val1.value() == "hello");
        }

        SUBCASE("String Literals") {
            CHECK(pspace.insert("/strings/lit1", "hello").nbrValuesInserted == 1);
            CHECK(pspace.insert("/strings/lit2", "world").nbrValuesInserted == 1);

            auto val1 = pspace.read<std::string>("/strings/lit1");
            CHECK(val1.has_value());
            CHECK(val1.value() == "hello");

            auto val2 = pspace.extract<std::string>("/strings/lit2");
            CHECK(val2.has_value());
            CHECK(val2.value() == "world");
        }

        SUBCASE("std::string") {
            std::string str1 = "test string 1";
            std::string str2 = "test string 2";

            CHECK(pspace.insert("/strings/std1", str1).nbrValuesInserted == 1);
            CHECK(pspace.insert("/strings/std2", str2).nbrValuesInserted == 1);

            auto val1 = pspace.read<std::string>("/strings/std1");
            CHECK(val1.has_value());
            CHECK(val1.value() == str1);

            auto val2 = pspace.extract<std::string>("/strings/std2");
            CHECK(val2.has_value());
            CHECK(val2.value() == str2);
        }

        SUBCASE("Empty Strings") {
            std::string empty;
            CHECK(pspace.insert("/strings/empty1", empty).nbrValuesInserted == 1);
            CHECK(pspace.insert("/strings/empty2", "").nbrValuesInserted == 1);

            auto val1 = pspace.read<std::string>("/strings/empty1");
            CHECK(val1.has_value());
            CHECK(val1.value().empty());

            auto val2 = pspace.read<std::string>("/strings/empty2");
            CHECK(val2.has_value());
            CHECK(val2.value().empty());
        }
    }

    SUBCASE("String Concatenation and Multiple Values") {
        // Test appending multiple strings to same path
        CHECK(pspace.insert("/concat", "Hello").nbrValuesInserted == 1);
        CHECK(pspace.insert("/concat", " ").nbrValuesInserted == 1);
        CHECK(pspace.insert("/concat", "World").nbrValuesInserted == 1);

        auto val1 = pspace.extract<std::string>("/concat");
        CHECK(val1.has_value());
        CHECK(val1.value() == "Hello");

        auto val2 = pspace.extract<std::string>("/concat");
        CHECK(val2.has_value());
        CHECK(val2.value() == " ");

        auto val3 = pspace.extract<std::string>("/concat");
        CHECK(val3.has_value());
        CHECK(val3.value() == "World");

        // Should be empty now
        auto val4 = pspace.read<std::string>("/concat");
        CHECK_FALSE(val4.has_value());
    }

    SUBCASE("Mixed Data Types with Strings") {
        SUBCASE("Basic Mixed Types") {
            // Insert different types to same path
            CHECK(pspace.insert("/mixed", "hello").nbrValuesInserted == 1);
            CHECK(pspace.insert("/mixed", 42).nbrValuesInserted == 1);
            CHECK(pspace.insert("/mixed", 3.14f).nbrValuesInserted == 1);
            CHECK(pspace.insert("/mixed", "world").nbrValuesInserted == 1);

            // Extract in order
            auto str1 = pspace.extract<std::string>("/mixed");
            CHECK(str1.has_value());
            CHECK(str1.value() == "hello");

            auto num = pspace.extract<int>("/mixed");
            CHECK(num.has_value());
            CHECK(num.value() == 42);

            auto flt = pspace.extract<float>("/mixed");
            CHECK(flt.has_value());
            CHECK(flt.value() == 3.14f);

            auto str2 = pspace.extract<std::string>("/mixed");
            CHECK(str2.has_value());
            CHECK(str2.value() == "world");
        }

        SUBCASE("Complex Data Structures with Strings") {
            // Vector of strings
            std::vector<std::string> vec = {"one", "two", "three"};
            CHECK(pspace.insert("/complex/vector", vec).nbrValuesInserted == 1);

            auto vec_result = pspace.read<std::vector<std::string>>("/complex/vector");
            CHECK(vec_result.has_value());
            CHECK(vec_result.value() == vec);

            // Map with string keys/values
            std::map<std::string, std::string> map = {{"key1", "value1"}, {"key2", "value2"}};
            CHECK(pspace.insert("/complex/map", map).nbrValuesInserted == 1);

            auto map_result = pspace.read<std::map<std::string, std::string>>("/complex/map");
            CHECK(map_result.has_value());
            CHECK(map_result.value() == map);
        }

        SUBCASE("String with Functions") {
            // Function returning string
            auto str_func = []() -> std::string { return "generated string"; };
            CHECK(pspace.insert("/func/str", str_func).nbrTasksInserted == 1);

            auto str_result = pspace.readBlock<std::string>("/func/str");
            CHECK(str_result.has_value());
            CHECK(str_result.value() == "generated string");

            // Mixed function returns
            CHECK(pspace.insert("/func/mixed", "static string").nbrValuesInserted == 1);
            CHECK(pspace.insert("/func/mixed", []() -> int { return 42; }).nbrTasksInserted == 1);
            CHECK(pspace.insert("/func/mixed", []() -> std::string { return "dynamic string"; }).nbrTasksInserted == 1);

            auto static_str = pspace.extract<std::string>("/func/mixed");
            CHECK(static_str.has_value());
            CHECK(static_str.value() == "static string");

            auto num = pspace.extractBlock<int>("/func/mixed");
            CHECK(num.has_value());
            CHECK(num.value() == 42);

            auto dynamic_str = pspace.readBlock<std::string>("/func/mixed");
            CHECK(dynamic_str.has_value());
            CHECK(dynamic_str.value() == "dynamic string");
        }
    }

    SUBCASE("String Glob Operations") {
        // Setup multiple string paths
        CHECK(pspace.insert("/glob/str1", "first").nbrValuesInserted == 1);
        CHECK(pspace.insert("/glob/str2", "second").nbrValuesInserted == 1);
        CHECK(pspace.insert("/glob/str3", "third").nbrValuesInserted == 1);

        // Insert to all paths using glob
        CHECK(pspace.insert("/glob/*", "glob append").nbrValuesInserted == 3);

        // Verify original values
        auto val1 = pspace.extract<std::string>("/glob/str1");
        CHECK(val1.has_value());
        CHECK(val1.value() == "first");

        auto val2 = pspace.extract<std::string>("/glob/str2");
        CHECK(val2.has_value());
        CHECK(val2.value() == "second");

        auto val3 = pspace.extract<std::string>("/glob/str3");
        CHECK(val3.has_value());
        CHECK(val3.value() == "third");

        // Verify appended values
        val1 = pspace.extract<std::string>("/glob/str1");
        CHECK(val1.has_value());
        CHECK(val1.value() == "glob append");

        val2 = pspace.extract<std::string>("/glob/str2");
        CHECK(val2.has_value());
        CHECK(val2.value() == "glob append");

        val3 = pspace.extract<std::string>("/glob/str3");
        CHECK(val3.has_value());
        CHECK(val3.value() == "glob append");
    }

    SUBCASE("String Edge Cases") {
        SUBCASE("Special Characters") {
            std::string special = "!@#$%^&*()_+\n\t\r";
            CHECK(pspace.insert("/special", special).nbrValuesInserted == 1);

            auto result = pspace.read<std::string>("/special");
            CHECK(result.has_value());
            CHECK(result.value() == special);
        }

        SUBCASE("Very Long Strings") {
            std::string long_str(1000000, 'a'); // 1MB string
            CHECK(pspace.insert("/long", long_str).nbrValuesInserted == 1);

            auto result = pspace.read<std::string>("/long");
            CHECK(result.has_value());
            CHECK(result.value() == long_str);
        }

        SUBCASE("Unicode Strings") {
            std::string unicode = "Hello, 世界! 🌍 привет";
            CHECK(pspace.insert("/unicode", unicode).nbrValuesInserted == 1);

            auto result = pspace.read<std::string>("/unicode");
            CHECK(result.has_value());
            CHECK(result.value() == unicode);
        }
    }

    SUBCASE("String Concurrent Operations") {
        std::atomic<int> counter{0};
        const int        NUM_THREADS    = 4;
        const int        OPS_PER_THREAD = 100;

        // Create multiple threads appending strings
        std::vector<std::thread> threads;
        for (int i = 0; i < NUM_THREADS; ++i) {
            threads.emplace_back([&, i]() {
                for (int j = 0; j < OPS_PER_THREAD; ++j) {
                    std::string value = "Thread" + std::to_string(i) + "_" + std::to_string(j);
                    CHECK(pspace.insert("/concurrent", value).nbrValuesInserted == 1);
                    counter++;
                }
            });
        }

        // Join threads
        for (auto& t : threads) {
            t.join();
        }

        // Verify all strings were inserted
        CHECK(counter == NUM_THREADS * OPS_PER_THREAD);

        // Extract all values and verify they're valid strings
        std::set<std::string> extracted_values;
        while (true) {
            auto val = pspace.extract<std::string>("/concurrent");
            if (!val.has_value())
                break;
            CHECK(val.value().substr(0, 6) == "Thread");
            extracted_values.insert(val.value());
        }

        // Verify we got the expected number of unique strings
        CHECK(extracted_values.size() == NUM_THREADS * OPS_PER_THREAD);
    }
}