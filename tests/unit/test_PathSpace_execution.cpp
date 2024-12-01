#include "ext/doctest.h"
#include <chrono>
#include <pathspace/PathSpace.hpp>
#include <thread>

using namespace SP;
using namespace std::chrono_literals;

TEST_CASE("PathSpace Execution") {
    PathSpace pspace;

    SUBCASE("Function Types") {
        SUBCASE("Function Pointer") {
            int (*f)() = +[]() -> int { return 65; };
            pspace.insert("/test", f);
            auto result = pspace.read<int>("/test", Block{});
            CHECK(result.has_value());
            CHECK(result.value() == 65);
        }

        SUBCASE("Function Lambda") {
            auto f = []() -> int { return 65; };
            CHECK(pspace.insert("/test", f).nbrTasksInserted == 1);
            auto result = pspace.read<int>("/test", Block{});
            REQUIRE(result.has_value());
            CHECK(result.value() == 65);
        }

        SUBCASE("Direct Lambda") {
            CHECK(pspace.insert("/test", []() -> int { return 65; }).nbrTasksInserted == 1);
            auto result = pspace.read<int>("/test", Block{});
            REQUIRE(result.has_value());
            CHECK(result.value() == 65);
        }
    }

    SUBCASE("Execution Categories") {
        SUBCASE("Immediate Execution") {
            pspace.insert("/test", []() -> int { return 42; }, In{.executionCategory = ExecutionCategory::Immediate});
            auto result = pspace.read<int>("/test", Block{});
            REQUIRE(result.has_value());
            CHECK(result.value() == 42);
        }

        SUBCASE("Lazy Execution") {
            pspace.insert("/test", []() -> int { return 42; }, In{.executionCategory = ExecutionCategory::Lazy});
            auto result = pspace.read<int>("/test", Block{});
            REQUIRE(result.has_value());
            CHECK(result.value() == 42);
        }
    }

    SUBCASE("Timeout Behavior") {
        SUBCASE("Successful Completion Before Timeout") {
            pspace.insert(
                    "/test",
                    []() -> int {
                        std::this_thread::sleep_for(50ms);
                        return 42;
                    },
                    In{.executionCategory = ExecutionCategory::Lazy});
            auto result = pspace.read<int>("/test", Block{200ms});
            REQUIRE(result.has_value());
            CHECK(result.value() == 42);
        }

        SUBCASE("Timeout Before Completion") {
            pspace.insert(
                    "/test",
                    []() -> int {
                        std::this_thread::sleep_for(200ms);
                        return 42;
                    },
                    In{.executionCategory = ExecutionCategory::Lazy});

            auto result = pspace.read<int>("/test", Block(50ms));
            CHECK(!result.has_value());
            CHECK(result.error().code == Error::Code::Timeout);
        }
    }

    SUBCASE("Multiple Operations") {
        SUBCASE("Read Then Extract") {
            pspace.insert("/test", []() -> int { return 42; });

            auto read_result = pspace.read<int>("/test", Block{});
            CHECK(read_result.has_value());
            CHECK(read_result.value() == 42);

            auto extract_result = pspace.take<int>("/test", Block{});
            CHECK(extract_result.has_value());
            CHECK(extract_result.value() == 42);

            auto final_read = pspace.read<int>("/test");
            CHECK(!final_read.has_value());
        }

        SUBCASE("Concurrent Tasks") {
            pspace.insert(
                    "/test1",
                    []() -> int {
                        std::this_thread::sleep_for(50ms);
                        return 1;
                    },
                    In{.executionCategory = ExecutionCategory::Lazy});

            pspace.insert(
                    "/test2",
                    []() -> int {
                        std::this_thread::sleep_for(50ms);
                        return 2;
                    },
                    In{.executionCategory = ExecutionCategory::Lazy});

            auto result1 = pspace.read<int>("/test1", Block{});
            auto result2 = pspace.read<int>("/test2", Block{});

            CHECK(result1.has_value());
            CHECK(result2.has_value());
            CHECK(result1.value() == 1);
            CHECK(result2.value() == 2);
        }
    }

    SUBCASE("Block Behavior Types") {
        SUBCASE("Wait For Execution") {
            pspace.insert("/test", []() -> int { return 42; }, In{.executionCategory = ExecutionCategory::Lazy});

            auto result = pspace.read<int>("/test", Block{});
            CHECK(result.has_value());
            CHECK(result.value() == 42);
        }

        SUBCASE("Wait For Existence") {
            std::thread inserter([&pspace]() {
                std::this_thread::sleep_for(50ms);
                pspace.insert("/test", 42);
            });

            auto result = pspace.read<int>("/test", Block{});

            inserter.join();
            CHECK(result.has_value());
            CHECK(result.value() == 42);
        }
    }
}