#include "PathSpace.hpp"
#include "ext/doctest.h"
#include "path/validation.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace SP;
using namespace std::chrono_literals;

struct TestPathHierarchy {
    PathSpace& space;

    TestPathHierarchy(PathSpace& sp)
        : space(sp) {
        // Setup standard test hierarchy
        space.insert("/root/branch1/leaf1", 1);
        space.insert("/root/branch1/leaf2", 2);
        space.insert("/root/branch2/leaf1", 3);
    }

    template <typename T>
    void verifyPath(const std::string& path, const T& expected) {
        auto result = space.read<T>(path);
        REQUIRE(result.has_value());
        CHECK(result.value() == expected);
    }
};

TEST_CASE("Path Validation") {
    SUBCASE("Basic Path Validation") {
        // Valid paths
        CHECK_FALSE(Path<std::string>("/").validate().has_value());
        CHECK_FALSE(Path<std::string>("/root").validate().has_value());
        CHECK_FALSE(Path<std::string>("/root/path").validate().has_value());
        CHECK_FALSE(Path<std::string>("/a/b/c").validate().has_value());

        // Invalid paths
        {
            auto error = Path<std::string>("").validate();
            REQUIRE(error.has_value());
            CHECK(error->code == Error::Code::InvalidPath);
            CHECK(error->message->find("Empty path") != std::string::npos);
        }
        {
            auto error = Path<std::string>("invalid").validate();
            REQUIRE(error.has_value());
            CHECK(error->code == Error::Code::InvalidPath);
            CHECK(error->message->find("start with '/'") != std::string::npos);
        }
        {
            auto error = Path<std::string>("/path/").validate();
            REQUIRE(error.has_value());
            CHECK(error->code == Error::Code::InvalidPath);
            CHECK(error->message->find("ends with slash") != std::string::npos);
        }
        {
            auto error = Path<std::string>("./path").validate();
            REQUIRE(error.has_value());
            CHECK(error->code == Error::Code::InvalidPath);
            CHECK(error->message->find("start with '/'") != std::string::npos);
        }
    }

    SUBCASE("Component Validation") {
        // Invalid components
        {
            auto error = Path<std::string>("//").validate(ValidationLevel::Full);
            REQUIRE(error.has_value());
            CHECK(error->message->find("Path ends with slash") != std::string::npos);
        }
        {
            auto error = Path<std::string>("/path//other").validate(ValidationLevel::Full);
            REQUIRE(error.has_value());
            CHECK(error->message->find("Empty path component") != std::string::npos);
        }
        {
            auto error = Path<std::string>("/path/.").validate(ValidationLevel::Full);
            REQUIRE(error.has_value());
            CHECK(error->message->find("Relative paths not allowed") != std::string::npos);
        }
        {
            auto error = Path<std::string>("/path/..").validate(ValidationLevel::Full);
            REQUIRE(error.has_value());
            CHECK(error->message->find("Relative paths not allowed") != std::string::npos);
        }
    }

    SUBCASE("Glob Pattern Validation") {
        // Valid patterns
        CHECK_FALSE(Path<std::string>("/path/*").validate().has_value());
        CHECK_FALSE(Path<std::string>("/*/path").validate().has_value());
        CHECK_FALSE(Path<std::string>("/path/?/other").validate().has_value());
        CHECK_FALSE(Path<std::string>("/path/[abc]").validate().has_value());
        CHECK_FALSE(Path<std::string>("/path/[a-z]").validate().has_value());
        CHECK_FALSE(Path<std::string>("/path/[!a-z]").validate().has_value());
        CHECK_FALSE(Path<std::string>("/path/[0-9]/*").validate().has_value());

        // Invalid patterns
        {
            auto error = Path<std::string>("/path/[").validate(ValidationLevel::Full);
            REQUIRE(error.has_value());
            CHECK(error->message->find("Unclosed bracket") != std::string::npos);
        }
        {
            auto error = Path<std::string>("/path/]").validate(ValidationLevel::Full);
            REQUIRE(error.has_value());
            CHECK(error->message->find("Unmatched closing bracket") != std::string::npos);
        }
        {
            auto error = Path<std::string>("/path/[a-]").validate(ValidationLevel::Full);
            REQUIRE(error.has_value());
            CHECK(error->message->find("Invalid character range") != std::string::npos);
        }
        {
            auto error = Path<std::string>("/path/[-a]").validate(ValidationLevel::Full);
            REQUIRE(error.has_value());
            CHECK(error->message->find("Invalid character range") != std::string::npos);
        }
        {
            auto error = Path<std::string>("/path/[z-a]").validate(ValidationLevel::Full);
            REQUIRE(error.has_value());
            CHECK(error->message->find("Invalid character range") != std::string::npos);
        }
    }

    SUBCASE("Escape Sequence Validation") {
        // Valid escapes
        CHECK_FALSE(Path<std::string>("/path/\\*").validate().has_value());
        CHECK_FALSE(Path<std::string>("/path/\\?").validate().has_value());
        CHECK_FALSE(Path<std::string>("/path/\\[").validate().has_value());
        CHECK_FALSE(Path<std::string>("/path/\\]").validate().has_value());
        CHECK_FALSE(Path<std::string>("/path/\\\\").validate().has_value());
    }

    SUBCASE("Complex Pattern Combinations") {
        // Multiple patterns
        CHECK_FALSE(Path<std::string>("/path/[a-z]/[0-9]/*").validate().has_value());
        CHECK_FALSE(Path<std::string>("/*/[a-z]/?/[0-9]").validate().has_value());

        // Escaped patterns in brackets
        CHECK(Path<std::string>("/path/[\\[-\\]]").validate(ValidationLevel::Full).has_value());
        CHECK_FALSE(Path<std::string>("/path/[\\*\\?]").validate(ValidationLevel::Full).has_value());

        // Complex combinations
        CHECK_FALSE(Path<std::string>("/[a-z]*/[0-9]?/*").validate(ValidationLevel::Full).has_value());
        CHECK_FALSE(Path<std::string>("/path/[!a-z][0-9]/*").validate(ValidationLevel::Full).has_value());

        // Invalid combinations
        {
            auto error = Path<std::string>("/path/[[a-z]]").validate(ValidationLevel::Full);
            REQUIRE(error.has_value());
            CHECK(error->message->find("Nested brackets") != std::string::npos);
        }
    }

    SUBCASE("Edge Cases") {
        // Maximum nesting
        {
            std::string deep_path;
            for (int i = 0; i < 100; ++i) {
                deep_path += "/valid";
            }
            CHECK_FALSE(Path<std::string>(deep_path).validate().has_value());
        }

        // Long component names
        {
            std::string long_name = "/path/";
            long_name.append(1000, 'a');
            CHECK_FALSE(Path<std::string>(long_name).validate().has_value());
        }

        // Complex pattern combinations
        CHECK_FALSE(Path<std::string>("/[!a-z][0-9]\\*/?/[a-zA-Z0-9]/\\[escaped\\]").validate().has_value());
    }

    /*SUBCASE("Compile Time Validation") {
        static constexpr auto valid1 = "/valid/path";
        static constexpr auto valid2 = "/valid/[a-z]/*";
        static constexpr auto valid3 = "/valid/\\[escaped\\]";

        static_assert(!Path<std::string>(valid1).validate().has_value());
        static_assert(!Path<std::string>(valid2).validate().has_value());
        static_assert(!Path<std::string>(valid3).validate().has_value());*/

    // These would cause compile errors if uncommented
    /*
    static_assert(!Path<std::string>("invalid").validate().has_value());
    static_assert(!Path<std::string>("/invalid/[/").validate().has_value());
    static_assert(!Path<std::string>("/invalid/path/").validate().has_value());
    */
    //}
}

TEST_CASE("PathSpace Integration") {
    PathSpace pspace;

    SUBCASE("Insert Validation") {
        // Valid inserts
        CHECK(pspace.insert("/valid/path", 42).errors.empty());
        CHECK(pspace.insert("/test/[a-z]/*", 42).errors.empty());

        // Invalid inserts
        {
            auto ret = pspace.insert("invalid", 42);
            CHECK_FALSE(ret.errors.empty());
            CHECK(ret.errors[0].code == Error::Code::InvalidPath);
        }
    }

    SUBCASE("Read Validation") {
        pspace.insert("/test", 42);

        // Valid paths
        CHECK(pspace.read<int>("/test").has_value());

        // Invalid paths
        auto bad_read = pspace.read<int>("invalid");
        REQUIRE_FALSE(bad_read.has_value());
        CHECK(bad_read.error().code == Error::Code::InvalidPath);
    }

    SUBCASE("Extract Validation") {
        pspace.insert("/test", 42);

        // Valid paths
        CHECK(pspace.extract<int>("/test").has_value());

        // Invalid paths
        auto bad_extract = pspace.extract<int>("invalid");
        REQUIRE_FALSE(bad_extract.has_value());
        CHECK(bad_extract.error().code == Error::Code::InvalidPath);
    }

    SUBCASE("Deep Nested Path Operations") {
        PathSpace pspace;

        // Multi-level insertion
        REQUIRE(pspace.insert("/org/dept/team/project/task1", 42).nbrValuesInserted == 1);
        REQUIRE(pspace.insert("/org/dept/team/project/task2", 43).nbrValuesInserted == 1);

        // Test partial path reads
        auto task1 = pspace.read<int>("/org/dept/team/project/task1");
        auto task2 = pspace.read<int>("/org/dept/team/project/task2");
        REQUIRE(task1.has_value());
        REQUIRE(task2.has_value());
        CHECK(task1.value() == 42);
        CHECK(task2.value() == 43);

        // Test glob across multiple levels
        CHECK(pspace.insert("/org/*/team/*/task*", 100).nbrValuesInserted == 2);
        auto modifiedTask1 = pspace.extract<int>("/org/dept/team/project/task1");
        auto modifiedTask2 = pspace.extract<int>("/org/dept/team/project/task2");
        REQUIRE(modifiedTask1.has_value());
        REQUIRE(modifiedTask2.has_value());
        CHECK(modifiedTask1.value() == 42);
        CHECK(modifiedTask2.value() == 43);
        modifiedTask1 = pspace.extract<int>("/org/dept/team/project/task1");
        modifiedTask2 = pspace.extract<int>("/org/dept/team/project/task2");
        REQUIRE(modifiedTask1.has_value());
        REQUIRE(modifiedTask2.has_value());
        CHECK(modifiedTask1.value() == 100);
        CHECK(modifiedTask2.value() == 100);
    }

    SUBCASE("Complex Nested Path Extraction") {
        PathSpace pspace;

        // Setup nested structure with different types
        pspace.insert("/data/sensors/temp/1", 23.5f);
        pspace.insert("/data/sensors/temp/2", 24.1f);
        pspace.insert("/data/sensors/humid/1", 85);
        pspace.insert("/data/sensors/humid/2", 87);

        // Extract humidity readings
        auto humid1 = pspace.extract<int>("/data/sensors/humid/1");
        auto humid2 = pspace.extract<int>("/data/sensors/humid/2");
        REQUIRE(humid1.has_value());
        REQUIRE(humid2.has_value());
        CHECK(humid1.value() == 85);
        CHECK(humid2.value() == 87);

        // Verify temperatures remain while humidity is gone
        CHECK(!pspace.read<int>("/data/sensors/humid/1").has_value());
        CHECK(pspace.read<float>("/data/sensors/temp/1").has_value());
    }

    SUBCASE("Mixed Data Type Operations") {
        PathSpace pspace;

        struct SensorData {
            float       temperature;
            int         humidity;
            std::string status;

            bool operator==(const SensorData& other) const {
                return temperature == other.temperature && humidity == other.humidity && status == other.status;
            }
        };

        // Insert different types at same path
        pspace.insert("/mixed/data", 42);
        pspace.insert("/mixed/data", "status");
        pspace.insert("/mixed/data", SensorData{23.5f, 85, "ok"});

        // Verify type-specific extraction order
        auto intResult    = pspace.extract<int>("/mixed/data");
        auto strResult    = pspace.extract<std::string>("/mixed/data");
        auto sensorResult = pspace.extract<SensorData>("/mixed/data");

        REQUIRE(intResult.has_value());
        REQUIRE(strResult.has_value());
        REQUIRE(sensorResult.has_value());

        CHECK(intResult.value() == 42);
        CHECK(strResult.value() == "status");
        CHECK(sensorResult.value() == SensorData{23.5f, 85, "ok"});

        // Verify path is now empty
        CHECK(!pspace.read<int>("/mixed/data").has_value());
    }

    SUBCASE("Advanced Glob Pattern Operations") {
        PathSpace pspace;

        // Setup hierarchical data
        pspace.insert("/2023/01/01/temp", 20.0f);
        pspace.insert("/2023/01/02/temp", 21.0f);
        pspace.insert("/2023/02/01/temp", 22.0f);
        pspace.insert("/2024/01/01/temp", 23.0f);

        // Test complex glob patterns
        CHECK(pspace.insert("/202?/0[1-2]/*/temp", 25.0f).nbrValuesInserted == 4);

        // Verify all temperatures were updated
        auto temp1 = pspace.read<float>("/2023/01/01/temp");
        auto temp2 = pspace.read<float>("/2023/01/02/temp");
        auto temp3 = pspace.read<float>("/2023/02/01/temp");
        auto temp4 = pspace.read<float>("/2024/01/01/temp");

        REQUIRE(temp1.has_value());
        REQUIRE(temp2.has_value());
        REQUIRE(temp3.has_value());
        REQUIRE(temp4.has_value());

        CHECK(temp1.value() == 20.0f);
        CHECK(temp2.value() == 21.0f);
        CHECK(temp3.value() == 22.0f);
        CHECK(temp4.value() == 23.0f);
    }

    SUBCASE("Complex Function Execution Chains") {
        PathSpace pspace;

        // Setup dependent computations
        auto computeBase = []() -> int { return 10; };
        auto multiply    = [&pspace]() -> int {
            return pspace.readBlock<int>("/data/base", Block{}).value() * 2;
        };
        auto addOffset = [&pspace]() -> int {
            return pspace.readBlock<int>("/data/multiplied", Block{}).value() + 5;
        };

        pspace.insert("/data/base", computeBase);
        pspace.insert("/data/multiplied", multiply);
        pspace.insert("/data/final", addOffset);

        // Verify execution chain
        auto result = pspace.readBlock<int>("/data/final", Block{});
        REQUIRE(result.has_value());
        CHECK(result.value() == 25); // (10 * 2) + 5

        // Test multiple reads give same result
        auto result2 = pspace.readBlock<int>("/data/final", Block{});
        REQUIRE(result2.has_value());
        CHECK(result2.value() == 25);
    }

    SUBCASE("Complex Concurrent Operations") {
        PathSpace        pspace;
        const int        NUM_THREADS = 4;
        std::atomic<int> successCount{0};
        std::atomic<int> failureCount{0};

        {
            std::vector<std::jthread> threads;
            for (int i = 0; i < NUM_THREADS; ++i) {
                threads.emplace_back([&, i]() {
                    try {
                        // Insert operations
                        pspace.insert("/data/" + std::to_string(i) + "/value", i);
                        pspace.insert("/data/" + std::to_string(i) + "/status", "active");

                        // Read operations
                        auto value = pspace.readBlock<int>("/data/" + std::to_string(i) + "/value", Block{});
                        if (value)
                            successCount++;

                        // Extract operations
                        auto status = pspace.extract<std::string>("/data/" + std::to_string(i) + "/status");
                        if (status)
                            successCount++;

                        // Glob operations
                        pspace.insert("/data/*/value", 100);
                    } catch (const std::exception& e) {
                        failureCount++;
                    }
                });
            }
        }

        // Verify results
        CHECK(successCount > 0);
        CHECK(failureCount == 0);

        // Check final state
        bool allUpdated = true;
        for (int i = 0; i < NUM_THREADS; ++i) {
            auto value = pspace.extract<int>("/data/" + std::to_string(i) + "/value");
            if (!value.has_value() || value.value() != i) {
                allUpdated = false;
                break;
            }
            value = pspace.read<int>("/data/" + std::to_string(i) + "/value");
            if (!value.has_value() || value.value() != 100) {
                allUpdated = false;
                break;
            }
        }
        CHECK(allUpdated);
    }

    SUBCASE("Timeout and Blocking Behavior") {
        PathSpace pspace;

        // Setup delayed computations
        auto slowTask = []() -> int {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            return 42;
        };

        auto fastTask = []() -> int {
            return 10;
        };

        // Test mixed timeout scenarios
        pspace.insert("/tasks/slow", slowTask);
        pspace.insert("/tasks/fast", fastTask);

        auto fastResult = pspace.readBlock<int>("/tasks/fast",
                                                Block(100ms));
        auto slowResult = pspace.readBlock<int>("/tasks/slow",
                                                Block(100ms));

        CHECK(fastResult.has_value());
        CHECK_FALSE(slowResult.has_value());
        CHECK(slowResult.error().code == Error::Code::Timeout);

        // Verify slow task eventually completes
        auto slowResultWait = pspace.readBlock<int>("/tasks/slow",
                                                    Block(1000ms));
        REQUIRE(slowResultWait.has_value());
        CHECK(slowResultWait.value() == 42);
    }

    SUBCASE("Edge Cases and Error Handling") {
        PathSpace pspace;

        // Test invalid type combinations
        pspace.insert("/data", 42);
        auto wrongType = pspace.read<std::string>("/data");
        CHECK(!wrongType.has_value());
        CHECK(wrongType.error().code == Error::Code::InvalidType);

        // Test malformed paths
        auto result = pspace.insert("invalid_path", 42);
        CHECK(!result.errors.empty());
        CHECK(result.errors[0].code == Error::Code::InvalidPath);

        // Test empty containers
        std::vector<int> empty;
        pspace.insert("/empty", empty);
        auto readEmpty = pspace.read<std::vector<int>>("/empty");
        REQUIRE(readEmpty.has_value());
        CHECK(readEmpty.value().empty());

        // Test nested empty paths
        CHECK(pspace.insert("/a//b///c", 42).nbrValuesInserted == 1);
        auto nestedEmptyResult = pspace.read<int>("/a/b/c");
        REQUIRE(nestedEmptyResult.has_value());
        CHECK(nestedEmptyResult.value() == 42);
    }

    SUBCASE("Resource Cleanup") {
        PathSpace pspace;

        // Insert some data
        pspace.insert("/test/cleanup/1", "data1");
        pspace.insert("/test/cleanup/2", "data2");

        // Extract and verify cleanup
        auto data1 = pspace.extract<std::string>("/test/cleanup/1");
        REQUIRE(data1.has_value());
        CHECK(data1.value() == "data1");
        CHECK(!pspace.read<std::string>("/test/cleanup/1").has_value());

        // Clear all data
        pspace.clear();
        CHECK(!pspace.read<std::string>("/test/cleanup/2").has_value());

        // Verify new insertions work after clear
        CHECK(pspace.insert("/test/new", "data3").nbrValuesInserted == 1);
    }

    SUBCASE("Simple Type Hierarchies") {
        PathSpace pspace;
        struct SimpleData {
            int         value;
            std::string name;

            bool operator==(const SimpleData& other) const = default;
        };

        struct NestedData {
            SimpleData         simple;
            std::vector<float> measurements;

            bool operator==(const NestedData& other) const = default;
        };

        SimpleData simple{42, "test"};
        NestedData nested{
                .simple       = {100, "nested"},
                .measurements = {1.0f, 2.0f, 3.0f}};

        pspace.insert("/data/mixed", simple);
        pspace.insert("/data/mixed", nested);

        auto mixedSimple = pspace.extract<SimpleData>("/data/mixed");
        REQUIRE(mixedSimple.has_value());
        INFO("Original simple.value: " << simple.value << ", simple.name: " << simple.name);
        INFO("Extracted simple.value: " << mixedSimple.value().value << ", simple.name: " << mixedSimple.value().name);
        CHECK(mixedSimple.value() == simple);

        auto mixedNested = pspace.extract<NestedData>("/data/mixed");
        REQUIRE(mixedNested.has_value());
        INFO("Original nested.simple.value: " << nested.simple.value
                                              << ", nested.simple.name: " << nested.simple.name);
        INFO("Extracted nested.simple.value: " << mixedNested.value().simple.value
                                               << ", nested.simple.name: " << mixedNested.value().simple.name);
        INFO("Original measurements: ");
        for (float f : nested.measurements)
            INFO(f << " ");
        INFO("Extracted measurements: ");
        for (float f : mixedNested.value().measurements)
            INFO(f << " ");
        CHECK(mixedNested.value() == nested);
    }

    SUBCASE("Complex Type Hierarchies") {
        PathSpace pspace;

        struct SimpleData {
            int         value;
            std::string name;

            bool operator==(const SimpleData& other) const = default;
        };

        struct NestedData {
            SimpleData         simple;
            std::vector<float> measurements;

            bool operator==(const NestedData& other) const = default;
        };

        // Test with simple data first
        SimpleData simple{42, "test"};
        REQUIRE(pspace.insert("/data/simple", simple).nbrValuesInserted == 1);

        auto simpleResult = pspace.read<SimpleData>("/data/simple");
        REQUIRE(simpleResult.has_value());
        CHECK(simpleResult.value() == simple);

        // Test with nested data
        NestedData nested{
                .simple       = {100, "nested"},
                .measurements = {1.0f, 2.0f, 3.0f}};

        REQUIRE(pspace.insert("/data/nested", nested).nbrValuesInserted == 1);

        auto nestedResult = pspace.read<NestedData>("/data/nested");
        REQUIRE(nestedResult.has_value());
        CHECK(nestedResult.value() == nested);

        // Test mixed operations
        NestedData nested2{
                .simple       = {200, "mixed"},
                .measurements = {4.0f, 5.0f, 6.0f}};

        pspace.insert("/data/mixed", simple);
        pspace.insert("/data/mixed", nested2);

        auto mixedSimple = pspace.extract<SimpleData>("/data/mixed");
        REQUIRE(mixedSimple.has_value());
        CHECK(mixedSimple.value() == simple);

        auto mixedNested = pspace.extract<NestedData>("/data/mixed");
        REQUIRE(mixedNested.has_value());
        auto mv = mixedNested.value();
        CHECK(mv == nested2);
    }
}