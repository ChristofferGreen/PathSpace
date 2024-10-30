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
            auto result = pspace.readBlock<int>("/test");
            CHECK(result.has_value());
            CHECK(result.value() == 65);
        }

        SUBCASE("Function Lambda") {
            auto f = []() -> int { return 65; };
            pspace.insert("/test", f);
            auto result = pspace.readBlock<int>("/test");
            CHECK(result.has_value());
            CHECK(result.value() == 65);
        }

        SUBCASE("Direct Lambda") {
            pspace.insert("/test", []() -> int { return 65; });
            auto result = pspace.readBlock<int>("/test");
            CHECK(result.has_value());
            CHECK(result.value() == 65);
        }
    }

    SUBCASE("Execution Categories") {
        SUBCASE("Immediate Execution") {
            pspace.insert(
                    "/test",
                    []() -> int { return 42; },
                    InOptions{.execution = ExecutionOptions{.category = ExecutionOptions::Category::Immediate}});
            auto result = pspace.readBlock<int>("/test");
            CHECK(result.has_value());
            CHECK(result.value() == 42);
        }

        SUBCASE("Lazy Execution") {
            pspace.insert(
                    "/test",
                    []() -> int { return 42; },
                    InOptions{.execution = ExecutionOptions{.category = ExecutionOptions::Category::Lazy}});
            auto result = pspace.readBlock<int>("/test");
            CHECK(result.has_value());
            CHECK(result.value() == 42);
        }
    }

    SUBCASE("Execution Location") {
        SUBCASE("Any Location") {
            pspace.insert(
                    "/test",
                    []() -> int { return 42; },
                    InOptions{.execution = ExecutionOptions{.location = ExecutionOptions::Location::Any}});
            auto result = pspace.readBlock<int>("/test");
            CHECK(result.has_value());
            CHECK(result.value() == 42);
        }

        SUBCASE("Main Thread") {
            pspace.insert(
                    "/test",
                    []() -> int { return 42; },
                    InOptions{.execution = ExecutionOptions{.location = ExecutionOptions::Location::MainThread}});
            auto result = pspace.readBlock<int>("/test");
            CHECK(result.has_value());
            CHECK(result.value() == 42);
        }
    }

    SUBCASE("Execution Priority") {
        SUBCASE("Low Priority") {
            pspace.insert(
                    "/test",
                    []() -> int { return 42; },
                    InOptions{.execution = ExecutionOptions{.priority = ExecutionOptions::Priority::Low}});
            auto result = pspace.readBlock<int>("/test");
            CHECK(result.has_value());
            CHECK(result.value() == 42);
        }

        SUBCASE("Middle Priority") {
            pspace.insert(
                    "/test",
                    []() -> int { return 42; },
                    InOptions{.execution = ExecutionOptions{.priority = ExecutionOptions::Priority::Middle}});
            auto result = pspace.readBlock<int>("/test");
            CHECK(result.has_value());
            CHECK(result.value() == 42);
        }

        SUBCASE("High Priority") {
            pspace.insert(
                    "/test",
                    []() -> int { return 42; },
                    InOptions{.execution = ExecutionOptions{.priority = ExecutionOptions::Priority::High}});
            auto result = pspace.readBlock<int>("/test");
            CHECK(result.has_value());
            CHECK(result.value() == 42);
        }
    }

    SUBCASE("Update Intervals") {
        SUBCASE("Single Update") {
            int counter = 0;
            pspace.insert(
                    "/test",
                    [&counter]() -> int { return ++counter; },
                    InOptions{.execution = ExecutionOptions{.updateInterval = std::nullopt, .maxNbrExecutions = 1}});

            auto result1 = pspace.readBlock<int>("/test");
            auto result2 = pspace.readBlock<int>("/test");
            CHECK(result1.has_value());
            CHECK(result2.has_value());
            CHECK(result1.value() == 1);
            CHECK(result2.value() == 1); // Same value, not updated
        }

        /*SUBCASE("Periodic Update") {
            int counter = 0;
            pspace.insert(
                    "/test",
                    [&counter]() -> int { return ++counter; },
                    InOptions{.execution = ExecutionOptions{.updateInterval = 100ms, .maxNbrExecutions = 3}});

            auto result1 = pspace.readBlock<int>("/test");
            std::this_thread::sleep_for(150ms);
            auto result2 = pspace.readBlock<int>("/test");
            CHECK(result1.value() != result2.value());
        }*/
    }

    SUBCASE("Timeout Behavior") {
        SUBCASE("Successful Completion Before Timeout") {
            pspace.insert(
                    "/test",
                    []() -> int {
                        std::this_thread::sleep_for(50ms);
                        return 42;
                    },
                    InOptions{.execution = ExecutionOptions{.category = ExecutionOptions::Category::Lazy}});

            auto result
                    = pspace.readBlock<int>("/test",
                                            OutOptions{.block = BlockOptions{.behavior = BlockOptions::Behavior::Wait, .timeout = 200ms}});
            CHECK(result.has_value());
            CHECK(result.value() == 42);
        }

        SUBCASE("Timeout Before Completion") {
            pspace.insert(
                    "/test",
                    []() -> int {
                        std::this_thread::sleep_for(200ms);
                        return 42;
                    },
                    InOptions{.execution = ExecutionOptions{.category = ExecutionOptions::Category::Lazy}});

            auto result
                    = pspace.readBlock<int>("/test",
                                            OutOptions{.block = BlockOptions{.behavior = BlockOptions::Behavior::Wait, .timeout = 50ms}});
            CHECK(!result.has_value());
            CHECK(result.error().code == Error::Code::Timeout);
        }
    }

    /*SUBCASE("Result Caching") {
        SUBCASE("Cache Enabled") {
            int counter = 0;
            pspace.insert(
                    "/test",
                    [&counter]() -> int { return ++counter; },
                    InOptions{.execution = ExecutionOptions{.cacheResult = true}});

            auto result1 = pspace.readBlock<int>("/test");
            auto result2 = pspace.readBlock<int>("/test");
            CHECK(result1.value() == result2.value());
            CHECK(counter == 1); // Function only called once
        }

        SUBCASE("Cache Disabled") {
            int counter = 0;
            pspace.insert(
                    "/test",
                    [&counter]() -> int { return ++counter; },
                    InOptions{.execution = ExecutionOptions{.cacheResult = false}});

            auto result1 = pspace.readBlock<int>("/test");
            auto result2 = pspace.readBlock<int>("/test");
            CHECK(result1.value() != result2.value());
            CHECK(counter == 2); // Function called twice
        }
    }*/

    /*SUBCASE("Error Handling") {
        SUBCASE("Function Throws") {
            pspace.insert(
                    "/test",
                    []() -> int { throw std::runtime_error("Test error"); },
                    InOptions{.execution = ExecutionOptions{.category = ExecutionOptions::Category::Lazy}});

            auto result = pspace.readBlock<int>("/test");
            CHECK(!result.has_value());
            CHECK(result.error().code == Error::Code::UnknownError);
        }

        SUBCASE("Invalid Path") {
            auto result = pspace.readBlock<int>("/nonexistent");
            CHECK(!result.has_value());
            CHECK(result.error().code == Error::Code::NoSuchPath);
        }
    }*/

    SUBCASE("Multiple Operations") {
        SUBCASE("Read Then Extract") {
            pspace.insert("/test", []() -> int { return 42; });

            auto read_result = pspace.readBlock<int>("/test");
            CHECK(read_result.has_value());
            CHECK(read_result.value() == 42);

            auto extract_result = pspace.extractBlock<int>("/test");
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
                    InOptions{.execution = ExecutionOptions{.category = ExecutionOptions::Category::Lazy}});

            pspace.insert(
                    "/test2",
                    []() -> int {
                        std::this_thread::sleep_for(50ms);
                        return 2;
                    },
                    InOptions{.execution = ExecutionOptions{.category = ExecutionOptions::Category::Lazy}});

            auto result1 = pspace.readBlock<int>("/test1");
            auto result2 = pspace.readBlock<int>("/test2");

            CHECK(result1.has_value());
            CHECK(result2.has_value());
            CHECK(result1.value() == 1);
            CHECK(result2.value() == 2);
        }
    }

    SUBCASE("Block Behavior Types") {
        /*SUBCASE("Don't Wait") {
            pspace.insert(
                    "/test",
                    []() -> int {
                        std::this_thread::sleep_for(100ms);
                        return 42;
                    },
                    InOptions{.execution = ExecutionOptions{.category = ExecutionOptions::Category::Lazy}});

            auto result = pspace.readBlock<int>("/test", OutOptions{.block = BlockOptions{.behavior = BlockOptions::Behavior::DontWait}});
            CHECK(!result.has_value());
        }*/

        SUBCASE("Wait For Execution") {
            pspace.insert(
                    "/test",
                    []() -> int { return 42; },
                    InOptions{.execution = ExecutionOptions{.category = ExecutionOptions::Category::Lazy}});

            auto result = pspace.readBlock<int>("/test",
                                                OutOptions{.block = BlockOptions{.behavior = BlockOptions::Behavior::WaitForExecution}});
            CHECK(result.has_value());
            CHECK(result.value() == 42);
        }

        SUBCASE("Wait For Existence") {
            std::thread inserter([&pspace]() {
                std::this_thread::sleep_for(50ms);
                pspace.insert("/test", 42);
            });

            auto result = pspace.readBlock<int>("/test",
                                                OutOptions{.block = BlockOptions{.behavior = BlockOptions::Behavior::WaitForExistence}});

            inserter.join();
            CHECK(result.has_value());
            CHECK(result.value() == 42);
        }
    }
}