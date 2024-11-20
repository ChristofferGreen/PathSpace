#include "ext/doctest.h"
#include <pathspace/PathSpace.hpp>

using namespace SP;

TEST_CASE("PathSpace Insert") {
    PathSpace pspace;
    SUBCASE("Simple PathSpace Construction") {
        CHECK(pspace.insert("/test", 54).nbrValuesInserted == 1);
    }

    SUBCASE("Simple PathSpace Path Into Data") {
        CHECK(pspace.insert("/test", 54).nbrValuesInserted == 1);
        auto const val = pspace.insert("/test/data", 55);
        CHECK(val.nbrValuesInserted == 0);
        // CHECK(val.errors.size() == 1);
        // CHECK(val.errors[0].code == Error::Code::InvalidPathSubcomponent);
    }

    SUBCASE("Simple PathSpace Glob Construction") {
        CHECK(pspace.insert("/test1", 1).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test2", 2).nbrValuesInserted == 1);
        CHECK(pspace.insert("/tast1", 3).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test*", 4).nbrValuesInserted == 2);
    }

    SUBCASE("Simple PathSpace Insert Compiletime Check") {
        CHECK(pspace.insert<"/test1">(1).nbrValuesInserted == 1);
        CHECK(pspace.insert<"/test2">(2).nbrValuesInserted == 1);
        CHECK(pspace.insert<"/test3">(3).nbrValuesInserted == 1);
        CHECK(pspace.insert<"/test*">(4).nbrValuesInserted == 3);
    }

    SUBCASE("Middle PathSpace Glob Construction") {
        CHECK(pspace.insert("/test1/test", 1).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test2/test", 2).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test3/test", 3).nbrValuesInserted == 1);
        CHECK(pspace.insert("/tast1", 4).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test*/moo", 5).nbrValuesInserted == 3);
    }

    SUBCASE("Multiple Levels Deep") {
        CHECK(pspace.insert("/a/b/c/d", 123).nbrValuesInserted == 1);
        CHECK(pspace.insert("/a/b/e/f", 456).nbrValuesInserted == 1);
        CHECK(pspace.insert("/a/b/e/f", 567).nbrValuesInserted == 1);
        CHECK(pspace.readBlock<int>("/a/b/c/d").value() == 123);
        CHECK(pspace.extractBlock<int>("/a/b/e/f").value() == 456);
        CHECK(pspace.readBlock<int>("/a/b/e/f").value() == 567);
    }

    SUBCASE("Simple Function Pointer Insertion and Execution") {
        int (*simpleFunc)() = []() -> int { return 42; };
        CHECK(pspace.insert("/simple", simpleFunc).errors.size() == 0);
        auto result = pspace.readBlock<int>("/simple");
        CHECK(result.has_value());
        CHECK(result.value() == 42);
    }

    SUBCASE("std::function Insertion and Execution") {
        std::function<int()> stdFunc = []() -> int { return 100; };
        CHECK(pspace.insert("/std", stdFunc).errors.size() == 0);
        auto result = pspace.readBlock<int>("/std");
        CHECK(result.has_value());
        CHECK(result.value() == 100);
    }

    SUBCASE("Nested Function Calls with Different Types") {
        auto const f1 = [&pspace]() -> double {
            auto const val = pspace.readBlock<int>("/f2").value();
            return val * 1.5;
        };
        auto const f2 = [&pspace]() -> int {
            auto const val = pspace.readBlock<std::string>("/f3").value();
            return std::stoi(val);
        };
        std::function<std::string()> const f3 = []() -> std::string { return "50"; };

        CHECK(pspace.insert("/f1", f1).errors.size() == 0);
        CHECK(pspace.insert("/f2", f2).errors.size() == 0);
        CHECK(pspace.insert("/f3", f3).errors.size() == 0);

        auto result = pspace.readBlock<double>("/f1");
        CHECK(result.has_value());
        CHECK(result.value() == 75.0);
    }

    SUBCASE("Large Number of Nested Calls") {
        const int DEPTH = 1000;
        for (int i = 0; i < DEPTH; ++i) {
            auto func = [i, &pspace]() -> int {
                if (i == 0)
                    return 1;
                return pspace.readBlock<int>(SP::ConcretePathStringView{"/func" + std::to_string(i - 1)}).value() + 1;
            };
            CHECK(pspace.insert(SP::GlobPathStringView{"/func" + std::to_string(i)}, func).errors.size() == 0);
        }

        auto result = pspace.readBlock<int>(SP::ConcretePathStringView{"/func" + std::to_string(DEPTH - 1)});
        CHECK(result.has_value());
        CHECK(result.value() == DEPTH);
    }

    SUBCASE("Sequential vs Immediate Function Execution") {
        SUBCASE("Sequential (Lazy) Execution") {
            std::atomic<int> counter(0);
            auto             incrementFunc = [&counter]() -> int { return ++counter; };

            // Insert with lazy execution - functions won't run until read
            for (int i = 0; i < 1000; ++i)
                CHECK(pspace.insert(std::format("/concurrent{}", i), incrementFunc, InOptions{.execution = ExecutionOptions{.category = ExecutionOptions::Category::Lazy}}).nbrTasksInserted == 1);

            // Reading triggers execution in sequence
            for (int i = 0; i < 1000; ++i)
                CHECK(pspace.readBlock<int>(std::format("/concurrent{}", i)) == i + 1);

            CHECK(counter == 1000);
        }

        SUBCASE("Immediate (Parallel) Execution") {
            std::atomic<int> counter(0);
            auto             incrementFunc = [&counter]() -> int { return ++counter; };

            // Insert with immediate execution - functions run right away in parallel
            for (int i = 0; i < 1000; ++i)
                CHECK(pspace.insert(std::format("/concurrent{}", i), incrementFunc, InOptions{.execution = ExecutionOptions{.category = ExecutionOptions::Category::Immediate}}).nbrTasksInserted == 1);

            // Read the results - they'll be in non-deterministic order
            std::set<int> results;
            for (int i = 0; i < 1000; ++i) {
                auto result = pspace.readBlock<int>(std::format("/concurrent{}", i));
                REQUIRE(result.has_value());
                auto value = result.value();
                CHECK(value > 0);     // Separate checks
                CHECK(value <= 1000); // instead of compound condition
                results.insert(value);
            }

            // All values should be unique
            CHECK(results.size() == 1000);
            // Counter should reach 1000
            CHECK(counter == 1000);
            // First value should be >= 1
            CHECK(*results.begin() >= 1);
            // Last value should be <= 1000
            CHECK(*results.rbegin() <= 1000);
        }
    }
}
