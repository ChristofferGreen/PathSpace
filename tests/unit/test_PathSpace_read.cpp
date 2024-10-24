#include "ext/doctest.h"
#include <pathspace/PathSpace.hpp>

// Standard library containers
#include <array>
#include <deque>
#include <forward_list>
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
#include <queue> // for std::queue and std::priority_queue
#include <stack>

using namespace SP;

TEST_CASE("PathSpace Read") {
    PathSpace pspace;
    SUBCASE("Simple PathSpace Read") {
        CHECK(pspace.insert("/test", 56).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test", 58).nbrValuesInserted == 1);
        auto ret = pspace.read<int>("/test");
        CHECK(ret.has_value());
        CHECK(ret.value() == 56);
        auto ret2 = pspace.read<int>("/test");
        CHECK(ret2.has_value());
        CHECK(ret2.value() == 56);
    }

    SUBCASE("Deeper PathSpace Read") {
        CHECK(pspace.insert("/test1/test2", 56).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test1/test2", 58).nbrValuesInserted == 1);
        auto ret = pspace.read<int>("/test1/test2");
        CHECK(ret.has_value());
        CHECK(ret.value() == 56);
        auto ret2 = pspace.read<int>("/test1/test2");
        CHECK(ret2.has_value());
        CHECK(ret2.value() == 56);
    }

    SUBCASE("Simple PathSpace Read Function Pointer Execution") {
        using TestFuncPtr = int (*)();
        TestFuncPtr f = []() -> int { return 58; };
        TestFuncPtr f2 = []() -> int { return 25; };
        CHECK(pspace.insert("/f", f).nbrTasksCreated == 1);
        CHECK(pspace.insert("/f2", f2).nbrTasksCreated == 1);
        CHECK(pspace.readBlock<int>("/f").value() == 58);
        CHECK(pspace.readBlock<int>("/f").value() == 58);
        CHECK(pspace.readBlock<int>("/f2").value() == 25);
    }

    SUBCASE("Simple PathSpace Execution Non-Immidiate") {
        std::function<int()> f = []() -> int { return 58; };
        CHECK(pspace.insert("/f", f, InOptions{.execution = ExecutionOptions{.category = ExecutionOptions::Category::OnReadOrExtract}})
                      .nbrTasksCreated
              == 1);
        CHECK(pspace.readBlock<int>("/f").value() == 58);
    }

    SUBCASE("PathSpace Read Function Pointer Execution Blocking Simple") {
        auto const f1 = [&pspace]() -> int { return pspace.readBlock<int>("/f2").value() + 11; };
        int (*f2)() = []() -> int { return 10; };

        CHECK(pspace.insert("/f1", f1).errors.size() == 0);
        CHECK(pspace.insert("/f2", f2).errors.size() == 0);

        auto const val = pspace.readBlock<int>("/f1");
        CHECK(val.has_value() == true);
        CHECK(val.value() == 21);
    }

    SUBCASE("PathSpace Read Function Pointer Execution Blocking") {
        auto const f1 = [&pspace]() -> int {
            auto val = pspace.readBlock<int>("/f2").value();
            sp_log(std::format("f1 returning {} + 1 = {} from f2.", val, val + 1), "INFO");
            return val + 1;
        };
        auto const f2 = [&pspace]() -> int {
            auto val = pspace.readBlock<int>("/f3").value();
            sp_log(std::format("f2 returning {} + 10 = {} from f3.", val, val + 10), "INFO");
            return val + 10;
        };
        int (*f3)() = []() -> int {
            sp_log("f3 returning 100.", "INFO");
            return 100;
        };

        CHECK(pspace.insert("/f1", f1).errors.size() == 0);
        CHECK(pspace.insert("/f2", f2).errors.size() == 0);
        CHECK(pspace.insert("/f3", f3).errors.size() == 0);

        auto const val = pspace.readBlock<int>("/f1");
        CHECK(val.has_value() == true);
        CHECK(val.value() == 111);
    }

    SUBCASE("PathSpace Read Block") {
        pspace.insert("/i", 46);
        auto const val = pspace.readBlock<int>("/i");
        CHECK(val.has_value());
        CHECK(val.value() == 46);
    }

    SUBCASE("PathSpace Read Block Delayed") {
        pspace.insert("/i", +[] { return 46; });
        auto const val = pspace.readBlock<int>("/i");
        CHECK(val.has_value());
        CHECK(val.value() == 46);
    }

    SUBCASE("Read with timeout") {
        auto ret = pspace.readBlock<int>(
                "/timeout",
                OutOptions{.block = BlockOptions{.behavior = BlockOptions::Behavior::Wait, .timeout = std::chrono::milliseconds(100)}});
        CHECK_FALSE(ret.has_value());
    }
}
TEST_CASE("PathSpace Read Std Datastructure") {
    PathSpace pspace;

    SUBCASE("PathSpace Read std::string") {
        pspace.insert("/string", std::string("hello"));
        auto const val = pspace.read<std::string>("/string");
        CHECK(val.has_value());
        CHECK(val.value() == "hello");
    }

    SUBCASE("PathSpace Read std::vector") {
        std::vector<int> vec = {1, 2, 3, 4, 5};
        pspace.insert("/vector", vec);
        auto const val = pspace.read<std::vector<int>>("/vector");
        CHECK(val.has_value());
        CHECK(val.value() == vec);
    }

    SUBCASE("PathSpace Read std::array") {
        std::array<double, 3> arr = {1.1, 2.2, 3.3};
        pspace.insert("/array", arr);
        auto const val = pspace.read<std::array<double, 3>>("/array");
        CHECK(val.has_value());
        CHECK(val.value() == arr);
    }

    SUBCASE("PathSpace Read std::map") {
        std::map<std::string, int> map = {{"one", 1}, {"two", 2}, {"three", 3}};
        pspace.insert("/map", map);
        auto const val = pspace.read<std::map<std::string, int>>("/map");
        CHECK(val.has_value());
        CHECK(val.value() == map);
    }

    SUBCASE("PathSpace Read std::unordered_map") {
        std::unordered_map<std::string, double> umap = {{"pi", 3.14}, {"e", 2.71}};
        pspace.insert("/umap", umap);
        auto const val = pspace.read<std::unordered_map<std::string, double>>("/umap");
        CHECK(val.has_value());
        CHECK(val.value() == umap);
    }

    SUBCASE("PathSpace Read std::set") {
        std::set<char> set = {'a', 'b', 'c', 'd'};
        pspace.insert("/set", set);
        auto const val = pspace.read<std::set<char>>("/set");
        CHECK(val.has_value());
        CHECK(val.value() == set);
    }

    SUBCASE("PathSpace Read std::unordered_set") {
        std::unordered_set<int> uset = {1, 2, 3, 4, 5};
        pspace.insert("/uset", uset);
        auto const val = pspace.read<std::unordered_set<int>>("/uset");
        CHECK(val.has_value());
        CHECK(val.value() == uset);
    }

    SUBCASE("PathSpace Read std::pair") {
        std::pair<int, std::string> pair = {42, "answer"};
        pspace.insert("/pair", pair);
        auto const val = pspace.read<std::pair<int, std::string>>("/pair");
        CHECK(val.has_value());
        CHECK(val.value() == pair);
    }

    SUBCASE("PathSpace Read std::tuple") {
        std::tuple<int, double, char> tuple = {1, 3.14, 'a'};
        pspace.insert("/tuple", tuple);
        auto const val = pspace.read<std::tuple<int, double, char>>("/tuple");
        CHECK(val.has_value());
        CHECK(val.value() == tuple);
    }

    SUBCASE("PathSpace Read std::optional") {
        std::optional<int> opt = 42;
        pspace.insert("/optional", opt);
        auto const val = pspace.read<std::optional<int>>("/optional");
        CHECK(val.has_value());
        CHECK(val.value() == opt);
    }

    SUBCASE("PathSpace Read std::variant") {
        std::variant<int, double, std::string> var = "hello";
        pspace.insert("/variant", var);
        auto const val = pspace.read<std::variant<int, double, std::string>>("/variant");
        CHECK(val.has_value());
        CHECK(val.value() == var);
    }

    SUBCASE("PathSpace Read std::bitset") {
        std::bitset<8> bits(0b10101010);
        pspace.insert("/bitset", bits);
        auto const val = pspace.read<std::bitset<8>>("/bitset");
        CHECK(val.has_value());
        CHECK(val.value() == bits);
    }

    SUBCASE("PathSpace Read std::deque") {
        std::deque<int> deq = {1, 2, 3, 4, 5};
        pspace.insert("/deque", deq);
        auto const val = pspace.read<std::deque<int>>("/deque");
        CHECK(val.has_value());
        CHECK(val.value() == deq);
    }

    SUBCASE("PathSpace Read std::list") {
        std::list<std::string> lst = {"one", "two", "three"};
        pspace.insert("/list", lst);
        auto const val = pspace.read<std::list<std::string>>("/list");
        CHECK(val.has_value());
        CHECK(val.value() == lst);
    }
}