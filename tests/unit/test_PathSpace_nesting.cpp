#include "ext/doctest.h"
#include <pathspace/PathSpace.hpp>

using namespace SP;
using namespace std::chrono_literals;

TEST_CASE("PathSpace Nesting") {
    SUBCASE("Basic Nested PathSpace Operations") {
        PathSpace outerSpace;
        auto      innerSpace = std::make_unique<PathSpace>();

        // Add some data to inner space
        REQUIRE(innerSpace->insert("/test", 42).nbrValuesInserted == 1);
        REQUIRE(innerSpace->insert("/nested/value", "hello").nbrValuesInserted == 1);

        // Insert inner space into outer space
        REQUIRE(outerSpace.insert("/inner", std::move(innerSpace)).nbrSpacesInserted == 1);

        // Verify we can read values through the nested path
        auto result1 = outerSpace.read<int>("/inner/test", Block{});
        REQUIRE(result1.has_value());
        CHECK(result1.value() == 42);

        auto result2 = outerSpace.read<std::string>("/inner/nested/value", Block{});
        REQUIRE(result2.has_value());
        CHECK(result2.value() == "hello");
    }

    SUBCASE("Deep Nesting") {
        PathSpace level1;
        auto      level2 = std::make_unique<PathSpace>();
        auto      level3 = std::make_unique<PathSpace>();

        // Add data at each level
        REQUIRE(level3->insert("/data", 100).nbrValuesInserted == 1);
        REQUIRE(level2->insert("/l3", std::move(level3)).nbrSpacesInserted == 1);
        REQUIRE(level1.insert("/l2", std::move(level2)).nbrSpacesInserted == 1);

        // Verify deep access
        auto result = level1.read<int>("/l2/l3/data", Block{});
        REQUIRE(result.has_value());
        CHECK(result.value() == 100);
    }

    SUBCASE("Multiple Nested Spaces") {
        PathSpace root;
        auto      space1 = std::make_unique<PathSpace>();
        auto      space2 = std::make_unique<PathSpace>();

        // Add data to each space
        REQUIRE(space1->insert("/data", 1).nbrValuesInserted == 1);
        REQUIRE(space2->insert("/data", 2).nbrValuesInserted == 1);

        // Insert both spaces
        REQUIRE(root.insert("/space1", std::move(space1)).nbrSpacesInserted == 1);
        REQUIRE(root.insert("/space2", std::move(space2)).nbrSpacesInserted == 1);

        // Verify access to both spaces
        auto result1 = root.read<int>("/space1/data", Block{});
        REQUIRE(result1.has_value());
        CHECK(result1.value() == 1);

        auto result2 = root.read<int>("/space2/data", Block{});
        REQUIRE(result2.has_value());
        CHECK(result2.value() == 2);
    }

    SUBCASE("Nested Space with Functions") {
        PathSpace root;
        auto      subspace = std::make_unique<PathSpace>();

        // Add a function to the subspace
        auto func = []() -> int { return 42; };
        REQUIRE(subspace->insert("/func", func, In{.executionCategory = ExecutionCategory::Lazy}).nbrTasksInserted == 1);

        // Insert subspace
        REQUIRE(root.insert("/sub", std::move(subspace)).nbrSpacesInserted == 1);

        // Execute function through nested path
        auto result = root.read<int>("/sub/func", Block{});
        REQUIRE(result.has_value());
        CHECK(result.value() == 42);
    }
    /*
             SUBCASE("Nested Space with Blocking Operations") {
                 PathSpace root;
                 auto      subspace = std::make_unique<PathSpace>();

                 // Insert subspace first
                 REQUIRE(root.insert("/sub", std::move(subspace)).nbrSpacesInserted == 1);

                 // Start a thread that will write to the nested space after a delay
                 std::thread writer([&root]() {
                     std::this_thread::sleep_for(100ms);
                     auto result = root.insert("/sub/delayed", 42);
                     REQUIRE(result.nbrValuesInserted == 1);
                 });

                 // Try to read with blocking
                 auto result = root.read<int>("/sub/delayed", Block{200ms});
                 writer.join();

                 REQUIRE(result.has_value());
                 CHECK(result.value() == 42);
             }

             SUBCASE("Nested Space Extraction") {
                 PathSpace root;
                 auto      subspace = std::make_unique<PathSpace>();

                 // Add data to subspace
                 REQUIRE(subspace->insert("/data", 42).nbrValuesInserted == 1);
                 REQUIRE(subspace->insert("/data", 43).nbrValuesInserted == 1);

                 // Insert subspace
                 REQUIRE(root.insert("/sub", std::move(subspace)).nbrSpacesInserted == 1);

                 // Test extraction through nested path
                 auto result1 = root.take<int>("/sub/data", Block{});
                 REQUIRE(result1.has_value());
                 CHECK(result1.value() == 42);

                 auto result2 = root.take<int>("/sub/data", Block{});
                 REQUIRE(result2.has_value());
                 CHECK(result2.value() == 43);

                 // Verify no more data
                 auto result3 = root.read<int>("/sub/data");
                 CHECK(!result3.has_value());
             }

             SUBCASE("Concurrent Access to Nested Space") {
                 PathSpace root;
                 auto      subspace = std::make_unique<PathSpace>();

                 // Insert subspace
                 REQUIRE(root.insert("/sub", std::move(subspace)).nbrSpacesInserted == 1);

                 constexpr int NUM_THREADS    = 10;
                 constexpr int OPS_PER_THREAD = 100;

                 std::atomic<int> insertCount{0};
                 std::atomic<int> readCount{0};

                 // Create writer threads
                 std::vector<std::thread> writers;
                 for (int i = 0; i < NUM_THREADS; ++i) {
                     writers.emplace_back([&root, i]() {
                         for (int j = 0; j < OPS_PER_THREAD; ++j) {
                             auto result = root.insert("/sub/data", i * OPS_PER_THREAD + j);
                             if (result.nbrValuesInserted == 1) {
                                 insertCount++;
                             }
                         }
                     });
                 }

                 // Create reader threads
                 std::vector<std::thread> readers;
                 for (int i = 0; i < NUM_THREADS; ++i) {
                     readers.emplace_back([&root, &readCount]() {
                         for (int j = 0; j < OPS_PER_THREAD; ++j) {
                             auto result = root.read<int>("/sub/data", Block{10ms});
                             if (result.has_value()) {
                                 readCount++;
                             }
                         }
                     });
                 }

                 // Join all threads
                 for (auto& w : writers)
                     w.join();
                 for (auto& r : readers)
                     r.join();

                 // Verify operations
                 CHECK(insertCount > 0);
                 CHECK(readCount > 0);
                 CHECK(insertCount + readCount > 0);
             }

             SUBCASE("Nested Space Clear Operations") {
                 PathSpace root;
                 auto      subspace = std::make_unique<PathSpace>();

                 // Add data to subspace
                 REQUIRE(subspace->insert("/data1", 42).nbrValuesInserted == 1);
                 REQUIRE(subspace->insert("/data2", "test").nbrValuesInserted == 1);

                 // Insert subspace
                 REQUIRE(root.insert("/sub", std::move(subspace)).nbrSpacesInserted == 1);

                 // Clear root space
                 root.clear();

                 // Verify all data is cleared
                 auto result1 = root.read<int>("/sub/data1");
                 CHECK(!result1.has_value());

                 auto result2 = root.read<std::string>("/sub/data2");
                 CHECK(!result2.has_value());
             }

             SUBCASE("Invalid Nested Space Operations") {
                 PathSpace root;

                 // Try to insert nullptr
                 std::unique_ptr<PathSpace> nullspace = nullptr;
                 auto                       result    = root.insert("/null", std::move(nullspace));
                 CHECK(result.errors.size() > 0);

                 // Try to insert into non-existent nested space
                 auto result2 = root.insert("/nonexistent/data", 42);
                 CHECK(result2.nbrValuesInserted == 0);
             }

             SUBCASE("Nested Space Path Validation") {
                 PathSpace root;
                 auto      subspace = std::make_unique<PathSpace>();

                 // Test invalid paths
                 auto result1 = root.insert("invalid", std::move(subspace));
                 CHECK(result1.errors.size() > 0);

                 auto subspace2 = std::make_unique<PathSpace>();
                 auto result2   = root.insert("/sub//invalid", std::move(subspace2));
                 CHECK(result2.errors.size() > 0);
             }*/
}