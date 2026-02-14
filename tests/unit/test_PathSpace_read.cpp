#include "third_party/doctest.h"
#include <pathspace/PathSpace.hpp>

// Standard library containers
#include <array>
#include <deque>
#include <forward_list>
#include <list>
#include <map>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Standard library utility types
#include <any>
#include <optional>
#include <tuple>
#include <typeinfo>
#include <utility> // for std::pair
#include <variant>

// Other standard library types
#include <bitset>
#include <queue> // for std::queue and std::priority_queue
#include <stack>

using namespace SP;
using namespace std::chrono_literals;

TEST_SUITE_BEGIN("pathspace.read");

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

    SUBCASE("Indexed PathSpace Read returns nth value without pop") {
        for (int i = 0; i < 5; ++i) {
            CHECK(pspace.insert("/ints", i).nbrValuesInserted == 1);
        }
        auto ret = pspace.read<int>("/ints[3]");
        REQUIRE(ret.has_value());
        CHECK(ret.value() == 3);
        // Front of queue remains unchanged
        auto front = pspace.read<int>("/ints");
        REQUIRE(front.has_value());
        CHECK(front.value() == 0);
    }

    SUBCASE("Simple PathSpace Read Function Pointer Execution") {
        using TestFuncPtr = int (*)();
        TestFuncPtr f     = []() -> int { return 58; };
        TestFuncPtr f2    = []() -> int { return 25; };
        CHECK(pspace.insert("/f", f).nbrTasksInserted == 1);
        CHECK(pspace.insert("/f2", f2).nbrTasksInserted == 1);
        CHECK(pspace.read<int>("/f", Block{}).value() == 58);
        CHECK(pspace.read<int>("/f", Block{}).value() == 58);
        CHECK(pspace.read<int>("/f2", Block{}).value() == 25);
    }

    SUBCASE("Simple PathSpace Execution Lazy") {
        std::function<int()> f = []() -> int { return 58; };
        CHECK(pspace.insert("/f", f, In{.executionCategory = ExecutionCategory::Lazy}).nbrTasksInserted == 1);
        CHECK(pspace.read<int>("/f", Block{}).value() == 58);
    }

    SUBCASE("PathSpace Read Function Pointer Execution Blocking Simple") {
        auto const f1 = [&pspace]() -> int { return pspace.read<int>("/f2", Block{}).value() + 11; };
        int (*f2)()   = []() -> int { return 10; };

        CHECK(pspace.insert("/f1", f1).errors.size() == 0);
        CHECK(pspace.insert("/f2", f2).errors.size() == 0);

        auto const val = pspace.read<int>("/f1", Block{});
        CHECK(val.has_value() == true);
        CHECK(val.value() == 21);
    }

    SUBCASE("PathSpace Read Function Pointer Execution Blocking") {
        auto const f1 = [&pspace]() -> int {
            auto val = pspace.read<int>("/f2", Block{}).value();
            sp_log(std::format("f1 returning {} + 1 = {} from f2.", val, val + 1), "INFO");
            return val + 1;
        };
        auto const f2 = [&pspace]() -> int {
            auto val = pspace.read<int>("/f3", Block{}).value();
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

        auto const val = pspace.read<int>("/f1", Block{});
        CHECK(val.has_value() == true);
        CHECK(val.value() == 111);
    }

    SUBCASE("PathSpace Read Block") {
        pspace.insert("/i", 46);
        auto const val = pspace.read<int>("/i", Block{});
        CHECK(val.has_value());
        CHECK(val.value() == 46);
    }

    SUBCASE("PathSpace Read Block Delayed") {
        pspace.insert("/i", +[] { return 46; });
        auto const val = pspace.read<int>("/i", Block{});
        CHECK(val.has_value());
        CHECK(val.value() == 46);
    }

    SUBCASE("Read with timeout") {
        auto ret = pspace.read<int>("/timeout", Block(100ms));
        CHECK_FALSE(ret.has_value());
    }

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
        REQUIRE(val.has_value());
        CHECK(val->size() == umap.size());
        for (auto const& [key, expected] : umap) {
            auto it = val->find(key);
            REQUIRE(it != val->end());
            CHECK(it->second == doctest::Approx(expected));
        }
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
        REQUIRE(val.has_value());
        CHECK(val->size() == uset.size());
        for (auto element : uset) {
            CHECK(val->count(element) == 1);
        }
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

    SUBCASE("Read children via runtime and compile-time helpers") {
        CHECK(pspace.insert("/root/a", 1).errors.empty());
        CHECK(pspace.insert("/root/b", 2).errors.empty());

        auto kids = pspace.read<Children>("/root");
        REQUIRE(kids.has_value());
        std::vector<std::string> names = kids->names;
        std::sort(names.begin(), names.end());
        CHECK(names == std::vector<std::string>({"a", "b"}));

        auto kidsConst = pspace.read<Children>("/root");
        REQUIRE(kidsConst.has_value());
        std::vector<std::string> namesConst = kidsConst->names;
        std::sort(namesConst.begin(), namesConst.end());
        CHECK(namesConst == std::vector<std::string>({"a", "b"}));

        auto bad = pspace.read<Children>("/root/*");
        CHECK_FALSE(bad.has_value());
    }

    SUBCASE("Glob read skips incompatible children and returns first compatible value") {
        CHECK(pspace.insert("/glob_read/a", std::string("nope")).errors.empty());
        CHECK(pspace.insert("/glob_read/b", 42).errors.empty());

        auto value = pspace.read<int>("/glob_read/*");
        REQUIRE(value.has_value());
        CHECK(value.value() == 42);

        auto stillString = pspace.read<std::string>("/glob_read/a");
        REQUIRE(stillString.has_value());
        CHECK(stillString.value() == "nope");
    }

    SUBCASE("Glob read reports type mismatch when no child matches") {
        CHECK(pspace.insert("/glob_mismatch/a", std::string("alpha")).errors.empty());
        CHECK(pspace.insert("/glob_mismatch/b", std::string("beta")).errors.empty());

        auto bad = pspace.read<int>("/glob_mismatch/*");
        CHECK_FALSE(bad.has_value());
        CHECK(bad.error().code == Error::Code::InvalidType);

        auto stillThere = pspace.read<std::string>("/glob_mismatch/a");
        REQUIRE(stillThere.has_value());
        CHECK(stillThere.value() == "alpha");
    }

    SUBCASE("Span read rejects glob and indexed paths without consuming data") {
        CHECK(pspace.insert("/glob/value", 9).errors.empty());
        auto globRead = pspace.read("/glob/*", [&](std::span<const int>) {});
        CHECK_FALSE(globRead.has_value());
        CHECK(globRead.error().code == Error::Code::InvalidPath);

        auto indexedRead = pspace.read("/glob/value[0]", [&](std::span<const int>) {});
        CHECK_FALSE(indexedRead.has_value());
        CHECK(indexedRead.error().code == Error::Code::InvalidPath);

        auto stillThere = pspace.take<int>("/glob/value");
        REQUIRE(stillThere.has_value());
        CHECK(stillThere.value() == 9);
    }

    SUBCASE("Span take rejects glob and indexed paths without consuming data") {
        CHECK(pspace.insert("/glob2/value", 5).errors.empty());
        auto globTake = pspace.take("/glob2/*", [&](std::span<int>) {});
        CHECK_FALSE(globTake.has_value());
        CHECK(globTake.error().code == Error::Code::InvalidPath);

        auto indexedTake = pspace.take("/glob2/value[0]", [&](std::span<int>) {});
        CHECK_FALSE(indexedTake.has_value());
        CHECK(indexedTake.error().code == Error::Code::InvalidPath);

        auto stillThere = pspace.take<int>("/glob2/value");
        REQUIRE(stillThere.has_value());
        CHECK(stillThere.value() == 5);
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

    SUBCASE("Compile-time read and take use FixedString overloads") {
        CHECK(pspace.insert("/fixed", 7).errors.empty());
        auto readFixed = pspace.read<"/fixed", int>();
        REQUIRE(readFixed.has_value());
        CHECK(*readFixed == 7);

        CHECK(pspace.insert("/fixed", 9).errors.empty());
        auto takeFixed = pspace.take<"/fixed", int>();
        REQUIRE(takeFixed.has_value());
        CHECK(*takeFixed == 7);

        auto takeFixed2 = pspace.take<"/fixed", int>();
        REQUIRE(takeFixed2.has_value());
        CHECK(*takeFixed2 == 9);

        auto emptyTake = pspace.take<"/fixed", int>();
        CHECK_FALSE(emptyTake.has_value());
    }

    SUBCASE("Runtime FutureAny read surfaces execution futures and missing paths") {
        auto missing = pspace.read("/noexec");
        CHECK_FALSE(missing.has_value());
        CHECK(missing.error().code == Error::Code::NoObjectFound);

        auto insertRet = pspace.insert("/exec", []() -> int { return 17; }, In{.executionCategory = ExecutionCategory::Lazy});
        CHECK(insertRet.errors.empty());
        CHECK(insertRet.nbrTasksInserted == 1);

        auto futAny = pspace.read("/exec");
        REQUIRE(futAny.has_value());
        CHECK(futAny->valid());
        CHECK(futAny->type() == typeid(int));
    }

    SUBCASE("FutureAny read honors validation level for malformed paths") {
        auto invalid = pspace.read("/bad//path", OutFullValidation{});
        CHECK_FALSE(invalid.has_value());
        CHECK(invalid.error().code == Error::Code::InvalidPath);

        auto skippedValidation = pspace.read("relative/path", OutNoValidation{});
        CHECK_FALSE(skippedValidation.has_value());
        CHECK(skippedValidation.error().code == Error::Code::NoObjectFound);
    }

    SUBCASE("Compile-time FutureAny read exposes execution future") {
        auto missing = pspace.read<"/noexec">();
        CHECK_FALSE(missing.has_value());
        CHECK(missing.error().code == Error::Code::NoObjectFound);

        auto insertRet = pspace.insert("/exec", []() -> int { return 42; }, In{.executionCategory = ExecutionCategory::Lazy});
        CHECK(insertRet.errors.empty());
        CHECK(insertRet.nbrTasksInserted == 1);

        auto futAny = pspace.read<"/exec">();
        REQUIRE(futAny.has_value());
        CHECK(futAny->valid());
        CHECK(futAny->type() == typeid(int));

        // Force execution, then verify the future can copy the result.
        auto readValue = pspace.read<int>("/exec", Block{});
        REQUIRE(readValue.has_value());
        CHECK(*readValue == 42);

        int copied = 0;
        CHECK(futAny->copy_to(&copied));
        CHECK(copied == 42);
    }
}

TEST_SUITE_END();
