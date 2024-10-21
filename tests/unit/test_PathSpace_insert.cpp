#include "ext/doctest.h"
#include <pathspace/PathSpace.hpp>
#include <pathspace/core/Capabilities.hpp>

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
}

TEST_CASE("PathSpace Insert Function and Execution") {
    PathSpace pspace;

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

    SUBCASE("Function Overwriting") {
        /*int (*func1)() = []() -> int { return 1; };
        int (*func2)() = []() -> int { return 2; };

        CHECK(pspace.insert(SP::GlobPathStringView{"/overwrite"}, func1).errors.size() == 0);
        CHECK(pspace.insert(SP::GlobPathStringView{"/overwrite"}, func2).errors.size() == 0);

        auto result = pspace.readBlock<int>(SP::ConcretePathStringView{"/overwrite"});
        CHECK(result.has_value());
        CHECK(result.value() == 2);*/
    }

    SUBCASE("Circular Dependency Detection") {
        // ToDo : Implement circular dependency detection
        /*auto f1 = [&pspace]() -> int { return pspace.readBlock<int>(SP::ConcretePathStringView{"/f2"}).value() + 1; };
        auto f2 = [&pspace]() -> int { return pspace.readBlock<int>(SP::ConcretePathStringView{"/f1"}).value() + 1; };

        CHECK(pspace.insert(SP::GlobPathStringView{"/f1"}, f1).errors.size() == 0);
        CHECK(pspace.insert(SP::GlobPathStringView{"/f2"}, f2).errors.size() == 0);

        auto result = pspace.readBlock<int>(SP::ConcretePathStringView{"/f1"});
        // Expecting an error or timeout due to circular dependency
        CHECK(!result.has_value());*/
    }

    SUBCASE("Exception Handling in Functions") {
        /*auto throwingFunc = []() -> int { throw std::runtime_error("Test exception"); };

        CHECK(pspace.insert(SP::GlobPathStringView{"/throwing"}, throwingFunc).errors.size() == 0);

        auto result = pspace.readBlock<int>(SP::ConcretePathStringView{"/throwing"});
        CHECK(!result.has_value());*/
        // ToDo: Check for appropriate error handling
        // The exact error checking depends on how PathSpace handles exceptions
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

    SUBCASE("Concurrent Function Execution") {
        std::atomic<int> counter(0);
        auto incrementFunc = [&counter]() -> int {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return ++counter;
        };

        for (int i = 0; i < 100; ++i) {
            CHECK(pspace.insert(SP::GlobPathStringView{"/concurrent" + std::to_string(i)}, incrementFunc).errors.size() == 0);
        }

        std::vector<std::thread> threads;
        for (int i = 0; i < 100; ++i) {
            threads.emplace_back([&pspace, i]() { pspace.readBlock<int>(SP::ConcretePathStringView{"/concurrent" + std::to_string(i)}); });
        }

        for (auto& t : threads) {
            t.join();
        }

        CHECK(counter == 100);
    }
}