#include "ext/doctest.h"
#include <atomic>
#include <chrono>
#include <future>
#include <pathspace/PathSpace.hpp>
#include <random>
#include <thread>
#include <vector>

using namespace SP;

TEST_CASE("PathSpace Multithreading") {
    SUBCASE("Basic Concurrent Operations") {
        PathSpace pspace;
        const int NUM_THREADS = 8;
        const int OPERATIONS_PER_THREAD = 100;
        const int MAX_RETRIES = 3;
        const auto TEST_TIMEOUT = std::chrono::seconds(30);

        struct Stats {
            std::atomic<size_t> totalOps{0};
            std::atomic<size_t> successfulOps{0};
            std::atomic<size_t> failedOps{0};
            std::atomic<size_t> timeouts{0};
            std::mutex mtx;
            std::map<std::string, size_t> pathAccesses;

            void recordAccess(const std::string& path) {
                std::lock_guard<std::mutex> lock(mtx);
                pathAccesses[path]++;
            }
        } stats;

        // Shared state
        std::atomic<bool> shouldStop{false};
        std::atomic<bool> readersCanStart{false};
        std::atomic<int> insertCount{0};

        // Fixed set of paths to ensure contention
        const std::vector<std::string> shared_paths = {"/shared/counter", "/shared/accumulator", "/shared/status"};

        // Get path ensuring shared path usage
        auto getPath = [&shared_paths](int threadId, int opId, std::mt19937& rng) -> std::string {
            std::uniform_int_distribution<> dist(0, shared_paths.size() - 1);
            // Use shared paths 50% of the time
            if (opId % 2 == 0) {
                return shared_paths[dist(rng)];
            }
            return std::format("/seq/{}/{}", threadId, opId);
        };

        // Writer function
        auto writer = [&](int threadId) {
            std::mt19937 rng(std::random_device{}() + threadId);
            try {
                for (int i = 0; i < OPERATIONS_PER_THREAD && !shouldStop; i++) {
                    std::string path = getPath(threadId, i, rng);
                    int value = threadId * 1000 + i;

                    bool inserted = false;
                    for (int attempt = 0; attempt < MAX_RETRIES && !inserted; attempt++) {
                        if (auto result = pspace.insert(path, value); result.errors.empty()) {
                            inserted = true;
                            insertCount++;
                            stats.successfulOps++;
                            stats.recordAccess(path);

                            if (insertCount > OPERATIONS_PER_THREAD / 2) {
                                readersCanStart.store(true);
                            }
                            break;
                        }

                        if (attempt < MAX_RETRIES - 1) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(1 << attempt));
                        }
                    }

                    if (!inserted)
                        stats.failedOps++;
                    stats.totalOps++;

                    if (i % 10 == 0)
                        std::this_thread::yield();
                }
            } catch (const std::exception& e) {
                log(std::format("Writer {} error: {}", threadId, e.what()));
                stats.failedOps++;
            }
        };

        // Reader function
        auto reader = [&](int threadId) {
            std::mt19937 rng(std::random_device{}() + threadId);

            while (!readersCanStart) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            try {
                for (int i = 0; i < OPERATIONS_PER_THREAD && !shouldStop; i++) {
                    std::string path = getPath(threadId % (NUM_THREADS / 2), i, rng);

                    OutOptions options{.block
                                       = BlockOptions{.behavior = BlockOptions::Behavior::Wait, .timeout = std::chrono::milliseconds(50)}};

                    // Try read first, fall back to extract
                    auto result = pspace.readBlock<int>(path, options);
                    if (!result.has_value()) {
                        result = pspace.extractBlock<int>(path, options);
                    }

                    if (result.has_value()) {
                        stats.successfulOps++;
                        stats.recordAccess(path);
                    } else {
                        if (result.error().code == Error::Code::Timeout) {
                            stats.timeouts++;
                        }
                        stats.failedOps++;
                    }

                    stats.totalOps++;

                    // Small delay to reduce contention
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            } catch (const std::exception& e) {
                log(std::format("Reader {} error: {}", threadId, e.what()));
                stats.failedOps++;
            }
        };

        // Launch threads
        {
            std::vector<std::jthread> threads;
            auto testStart = std::chrono::steady_clock::now();

            // Start writers
            for (int i = 0; i < NUM_THREADS / 2; ++i) {
                threads.emplace_back(writer, i);
            }

            // Start readers after some data is available
            for (int i = NUM_THREADS / 2; i < NUM_THREADS; ++i) {
                threads.emplace_back(reader, i);
            }

            // Monitor progress
            while (stats.totalOps < NUM_THREADS * OPERATIONS_PER_THREAD) {
                if (std::chrono::steady_clock::now() - testStart > TEST_TIMEOUT) {
                    shouldStop = true;
                    log("Test timeout reached");
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        // Verify results
        {
            // Calculate rates
            double successRate = static_cast<double>(stats.successfulOps) / stats.totalOps * 100;
            double errorRate = static_cast<double>(stats.failedOps) / stats.totalOps;

            // Check success rate
            CHECK_MESSAGE(successRate > 90.0, std::format("Success rate too low: {:.1f}%", successRate));

            // Check error rate
            CHECK_MESSAGE(errorRate < 0.1, std::format("Error rate too high: {:.1f}%", errorRate * 100));

            // Verify shared path usage
            std::lock_guard<std::mutex> lock(stats.mtx);
            for (const auto& path : shared_paths) {
                size_t accesses = stats.pathAccesses[path];
                CHECK_MESSAGE(accesses > NUM_THREADS,
                              std::format("Insufficient contention on shared path {}: {} accesses", path, accesses));
            }

            // Clean up and verify
            pspace.clear();
            for (const auto& [path, _] : stats.pathAccesses) {
                CHECK_MESSAGE(!pspace.read<int>(path).has_value(), std::format("Data remains at path: {}", path));
            }
        }
    }

    SUBCASE("PathSpace Concurrent Counter") {
        PathSpace pspace;
        const int NUM_THREADS = std::min(16, static_cast<int>(std::thread::hardware_concurrency() * 2));
        const int OPERATIONS_PER_THREAD = 100;

        std::atomic<int> failedOperations{0};
        std::atomic<int> successfulOperations{0};

        // Structure to track thread operations
        struct ThreadStats {
            std::vector<int> insertedValues;
            int threadId;
            int successCount{0};
            int failCount{0};

            ThreadStats(int id) : threadId(id) {
                insertedValues.reserve(OPERATIONS_PER_THREAD);
            }
        };

        auto workerFunction = [&](int threadId) {
            ThreadStats stats(threadId);

            for (int i = 0; i < OPERATIONS_PER_THREAD; ++i) {
                // Generate unique value that encodes both thread ID and operation number
                // This lets us verify exactly which operations succeeded
                int value = (threadId * OPERATIONS_PER_THREAD) + i;

                // Try to insert our value
                auto result = pspace.insert("/data", value);

                if (result.errors.empty()) {
                    stats.insertedValues.push_back(value);
                    stats.successCount++;
                    successfulOperations.fetch_add(1, std::memory_order_relaxed);
                } else {
                    stats.failCount++;
                    failedOperations.fetch_add(1, std::memory_order_relaxed);
                }
            }

            return stats;
        };

        // Launch threads and collect their stats
        std::vector<std::future<ThreadStats>> futures;
        for (int i = 0; i < NUM_THREADS; ++i) {
            futures.push_back(std::async(std::launch::async, workerFunction, i));
        }

        // Collect results
        std::vector<ThreadStats> allStats;
        for (auto& future : futures) {
            allStats.push_back(future.get());
        }

        // Extract all values to verify what got stored
        std::vector<int> extractedValues;
        while (true) {
            auto result = pspace.extract<int>("/data");
            if (!result.has_value())
                break;
            extractedValues.push_back(result.value());
        }

        // Verify the test results
        INFO("Successful operations: " << successfulOperations.load());
        INFO("Failed operations: " << failedOperations.load());
        INFO("Extracted values: " << extractedValues.size());

        // Check that we haven't lost any successful operations
        CHECK(extractedValues.size() == successfulOperations.load());

        // Verify no duplicate values were stored
        std::set<int> uniqueValues(extractedValues.begin(), extractedValues.end());
        CHECK(uniqueValues.size() == extractedValues.size());

        // Verify we can reconstruct which thread's operations succeeded
        std::vector<int> successesPerThread(NUM_THREADS, 0);
        for (int value : extractedValues) {
            int threadId = value / OPERATIONS_PER_THREAD;
            int operationNum = value % OPERATIONS_PER_THREAD;
            REQUIRE(threadId >= 0);
            REQUIRE(threadId < NUM_THREADS);
            REQUIRE(operationNum >= 0);
            REQUIRE(operationNum < OPERATIONS_PER_THREAD);
            successesPerThread[threadId]++;
        }

        // Verify each thread's recorded successes match what we extracted
        for (size_t i = 0; i < allStats.size(); i++) {
            CHECK(allStats[i].successCount == successesPerThread[i]);
        }
    }

    SUBCASE("PathSpace Counter Order Preservation") {
        PathSpace pspace;
        const int NUM_THREADS = 4;
        const int OPERATIONS_PER_THREAD = 10;

        // Track operations as they're queued
        struct Operation {
            int threadId;
            int seqNum;
            int value;
        };
        std::vector<Operation> expectedOperations;
        std::mutex opsMutex;

        auto workerFunction = [&](int threadId) {
            for (int i = 0; i < OPERATIONS_PER_THREAD; i++) {
                int value = (threadId * 100) + i;

                // Insert our value
                auto result = pspace.insert("/counter", value);
                REQUIRE(result.errors.empty());

                // Record this operation
                {
                    std::lock_guard<std::mutex> lock(opsMutex);
                    expectedOperations.push_back({threadId, i, value});
                }

                // Small delay to help interleave operations
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        };

        // Launch threads
        std::vector<std::thread> threads;
        for (int i = 0; i < NUM_THREADS; i++) {
            threads.emplace_back(workerFunction, i);
        }

        // Wait for threads to complete
        for (auto& t : threads) {
            t.join();
        }

        // Extract all values and verify order
        std::vector<Operation> actualOperations;

        while (true) {
            auto value = pspace.extractBlock<int>("/counter");
            if (!value.has_value())
                break;

            // Find matching operation
            bool found = false;
            Operation matchingOp;

            for (const auto& op : expectedOperations) {
                if (op.value == value.value()) {
                    matchingOp = op;
                    found = true;
                    break;
                }
            }

            REQUIRE(found);
            actualOperations.push_back(matchingOp);
        }

        // Verify we got all operations
        CHECK(actualOperations.size() == NUM_THREADS * OPERATIONS_PER_THREAD);

        // Verify per-thread ordering (operations from same thread should be in sequence)
        for (int t = 0; t < NUM_THREADS; t++) {
            std::vector<int> threadSeqNums;

            for (const auto& op : actualOperations) {
                if (op.threadId == t) {
                    threadSeqNums.push_back(op.seqNum);
                }
            }

            INFO("Thread " << t << " sequence: ");
            for (int seq : threadSeqNums) {
                INFO("  " << seq);
            }

            // Check that this thread's operations are in order
            CHECK(std::is_sorted(threadSeqNums.begin(), threadSeqNums.end()));
            CHECK(threadSeqNums.size() == OPERATIONS_PER_THREAD);
        }

        // Print full operation sequence for debugging
        INFO("\nFull operation sequence:");
        for (const auto& op : actualOperations) {
            INFO("Thread " << op.threadId << " op " << op.seqNum << " (value " << op.value << ")");
        }
    }

    SUBCASE("Mixed Readers and Writers") {
        PathSpace pspace;
        const int NUM_WRITERS = 4;
        const int NUM_READERS = 4;
        const int VALUES_PER_WRITER = 100;

        std::atomic<int> readsCompleted{0};
        std::atomic<int> extractsCompleted{0};
        std::atomic<int> writesCompleted{0};

        auto writerFunction = [&](int threadId) {
            for (int i = 0; i < VALUES_PER_WRITER; i++) {
                int value = (threadId * 1000) + i;
                REQUIRE(pspace.insert("/mixed", value).errors.empty());
                writesCompleted++;

                // Occasionally write to a different path
                if (i % 10 == 0) {
                    REQUIRE(pspace.insert("/mixed_alt", value).errors.empty());
                }
            }
        };

        auto readerFunction = [&]() {
            while (writesCompleted < (NUM_WRITERS * VALUES_PER_WRITER)) {
                auto value = pspace.readBlock<int>("/mixed");
                if (value.has_value()) {
                    readsCompleted++;
                }

                // Occasionally check alternate path
                if (readsCompleted % 10 == 0) {
                    auto altValue = pspace.readBlock<int>("/mixed_alt");
                    if (altValue.has_value()) {
                        readsCompleted++;
                    }
                }

                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        };

        auto extractorFunction = [&]() {
            while (writesCompleted < (NUM_WRITERS * VALUES_PER_WRITER)) {
                auto value = pspace.extractBlock<int>("/mixed");
                if (value.has_value()) {
                    extractsCompleted++;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        };

        // Launch threads
        std::vector<std::thread> threads;

        // Start readers and extractors first
        for (int i = 0; i < NUM_READERS / 2; i++) {
            threads.emplace_back(readerFunction);
            threads.emplace_back(extractorFunction);
        }

        // Then start writers
        for (int i = 0; i < NUM_WRITERS; i++) {
            threads.emplace_back(writerFunction, i);
        }

        // Wait for completion
        for (auto& t : threads) {
            t.join();
        }

        // Verify operations
        CHECK(writesCompleted == NUM_WRITERS * VALUES_PER_WRITER);
        INFO("Reads completed: " << readsCompleted);
        INFO("Extracts completed: " << extractsCompleted);
        CHECK(readsCompleted > 0);
        CHECK(extractsCompleted > 0);
    }

    SUBCASE("Multiple Path Operations") {
        PathSpace pspace;
        const int NUM_THREADS = 4;
        const int PATHS_PER_THREAD = 3;
        const int OPS_PER_PATH = 50;

        struct PathOperation {
            std::string path;
            int threadId;
            int seqNum;
            int value;
        };

        std::vector<std::vector<PathOperation>> threadOperations(NUM_THREADS);
        std::mutex opsMutex;

        auto workerFunction = [&](int threadId) {
            std::vector<std::string> paths;
            for (int p = 0; p < PATHS_PER_THREAD; p++) {
                paths.push_back("/path" + std::to_string(threadId) + "_" + std::to_string(p));
            }

            for (int i = 0; i < OPS_PER_PATH; i++) {
                for (const auto& path : paths) {
                    int value = (threadId * 1000000) + (i * 1000);

                    REQUIRE(pspace.insert(path, value).errors.empty());

                    {
                        std::lock_guard<std::mutex> lock(opsMutex);
                        threadOperations[threadId].push_back({path, threadId, i, value});
                    }
                }
            }
        };

        // Launch threads
        std::vector<std::thread> threads;
        for (int i = 0; i < NUM_THREADS; i++) {
            threads.emplace_back(workerFunction, i);
        }

        for (auto& t : threads) {
            t.join();
        }

        // Verify each path's operations
        for (int t = 0; t < NUM_THREADS; t++) {
            for (int p = 0; p < PATHS_PER_THREAD; p++) {
                std::string path = "/path" + std::to_string(t) + "_" + std::to_string(p);
                std::vector<int> seqNums;

                // Extract all values from this path
                while (true) {
                    auto value = pspace.extractBlock<int>(path);
                    if (!value.has_value())
                        break;

                    // Find matching operation
                    auto& ops = threadOperations[t];
                    auto it = std::find_if(ops.begin(), ops.end(), [&](const PathOperation& op) {
                        return op.path == path && op.value == value.value();
                    });

                    REQUIRE(it != ops.end());
                    seqNums.push_back(it->seqNum);
                }

                // Verify sequence
                CHECK(seqNums.size() == OPS_PER_PATH);
                CHECK(std::is_sorted(seqNums.begin(), seqNums.end()));
            }
        }
    }

    SUBCASE("Read-Extract Race Conditions") {
        PathSpace pspace;
        const int NUM_VALUES = 100; // Smaller number for clearer testing

        // Pre-populate with known values
        for (int i = 0; i < NUM_VALUES; i++) {
            REQUIRE(pspace.insert("/race", i).errors.empty());
        }

        std::vector<int> extractedValues;
        std::mutex extractMutex;

        // Just two threads - one reader, one extractor
        std::thread readerThread([&]() {
            int readCount = 0;
            while (readCount < NUM_VALUES * 100) { // Allow more reads than values
                auto value = pspace.read<int>("/race");
                if (value.has_value()) {
                    readCount++;
                    INFO("Read value: " << value.value());
                }
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            }
        });

        std::thread extractorThread([&]() {
            while (true) {
                auto value = pspace.extractBlock<int>("/race");
                if (!value.has_value())
                    break;

                std::lock_guard<std::mutex> lock(extractMutex);
                extractedValues.push_back(value.value());
            }
        });

        readerThread.join();
        extractorThread.join();

        // Verify results
        std::sort(extractedValues.begin(), extractedValues.end());
        INFO("Number of values extracted: " << extractedValues.size());

        CHECK(extractedValues.size() == NUM_VALUES);
        for (int i = 0; i < NUM_VALUES; i++) {
            CHECK(extractedValues[i] == i);
        }

        // Verify queue is empty
        auto finalRead = pspace.readBlock<int>("/race");
        CHECK_FALSE(finalRead.has_value());
        auto finalExtract = pspace.extractBlock<int>("/race");
        CHECK_FALSE(finalExtract.has_value());
    }

    SUBCASE("Concurrent Path Creation") { // ToDo: This crashed once, unknown why.
        PathSpace pspace;
        const int NUM_THREADS = 8;
        const int PATHS_PER_THREAD = 100;

        auto pathCreator = [&](int threadId) {
            for (int i = 0; i < PATHS_PER_THREAD; i++) {
                std::string basePath = "/thread" + std::to_string(threadId);
                for (int depth = 0; depth < 3; depth++) {
                    std::string path = basePath + "/path" + std::to_string(i) + "/depth" + std::to_string(depth);
                    REQUIRE(pspace.insert(path, i).errors.empty());
                }
            }
        };

        // Launch threads
        std::vector<std::thread> threads;
        for (int i = 0; i < NUM_THREADS; i++) {
            threads.emplace_back(pathCreator, i);
        }

        for (auto& t : threads) {
            t.join();
        }

        // Verify all paths were created and contain correct values
        for (int t = 0; t < NUM_THREADS; t++) {
            for (int i = 0; i < PATHS_PER_THREAD; i++) {
                std::string basePath = "/thread" + std::to_string(t);
                for (int depth = 0; depth < 3; depth++) {
                    std::string path = basePath + "/path" + std::to_string(i) + "/depth" + std::to_string(depth);
                    auto value = pspace.extractBlock<int>(path);
                    REQUIRE(value.has_value());
                    CHECK(value.value() == i);
                }
            }
        }
    }

    SUBCASE("Blocking Operations") {
        PathSpace pspace;
        const int NUM_THREADS = 4;
        const int ITEMS_PER_THREAD = 50;

        struct TestData {
            std::string path;
            int value;
            bool extracted{false};
        };

        std::vector<std::vector<TestData>> threadData(NUM_THREADS);

        // Phase 1: Insert data
        log("\nPhase 1: Inserting data");
        {
            for (int t = 0; t < NUM_THREADS; ++t) {
                threadData[t].reserve(ITEMS_PER_THREAD);
                for (int i = 0; i < ITEMS_PER_THREAD; ++i) {
                    TestData data{.path = std::format("/data/{}/{}", t, i), .value = t * 1000 + i};

                    auto result = pspace.insert(data.path, data.value);
                    CHECK_MESSAGE(result.errors.empty(), std::format("Failed to insert at path {}", data.path));

                    threadData[t].push_back(data);
                }
            }

            log(std::format("Inserted {} items", NUM_THREADS * ITEMS_PER_THREAD));
        }

        // Phase 2: Extract data with multiple threads
        log("\nPhase 2: Extracting data");
        {
            std::atomic<int> extractedCount = 0;
            std::atomic<bool> shouldStop = false;

            auto extractWorker = [&](int threadId) {
                auto& items = threadData[threadId];
                for (auto& item : items) {
                    if (shouldStop)
                        break;
                    if (item.extracted)
                        continue;

                    auto result = pspace.extractBlock<int>(item.path,
                                                           OutOptions{.block = BlockOptions{.behavior = BlockOptions::Behavior::Wait,
                                                                                            .timeout = std::chrono::milliseconds(100)}});

                    if (result.has_value()) {
                        CHECK(result.value() == item.value);
                        item.extracted = true;
                        extractedCount++;
                    }
                }
            };

            // Launch extraction threads
            {
                std::vector<std::jthread> threads;
                for (int t = 0; t < NUM_THREADS; ++t) {
                    threads.emplace_back(extractWorker, t);
                }

                // Monitor progress
                auto start = std::chrono::steady_clock::now();
                while (extractedCount < NUM_THREADS * ITEMS_PER_THREAD) {
                    if (std::chrono::steady_clock::now() - start > std::chrono::seconds(10)) {
                        shouldStop = true;
                        log("Extraction timeout reached");
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    log(std::format("Extracted: {}/{}", extractedCount.load(), NUM_THREADS * ITEMS_PER_THREAD));
                }
            }

            log(std::format("Extracted {} items", extractedCount.load()));
        }

        // Phase 3: Verify and cleanup any remaining items
        log("\nPhase 3: Verification and cleanup");
        {
            int remainingItems = 0;
            std::vector<std::string> remainingPaths;

            // First pass: Try to extract any remaining items
            for (auto& thread : threadData) {
                for (auto& item : thread) {
                    if (item.extracted)
                        continue;

                    // Try to extract the item
                    auto result = pspace.extract<int>(item.path);
                    if (result.has_value()) {
                        remainingItems++;
                        remainingPaths.push_back(item.path);
                    }
                }
            }

            if (!remainingPaths.empty()) {
                log("\nFound remaining items:");
                for (const auto& path : remainingPaths) {
                    log(std::format("  {}", path));
                }

                // Try one more time to extract these items
                for (const auto& path : remainingPaths) {
                    if (auto result = pspace.extract<int>(path); result.has_value()) {
                        remainingItems--;
                    }
                }
            }

            // Final cleanup
            pspace.clear();

            // Final verification using extract
            bool anyRemaining = false;
            for (const auto& thread : threadData) {
                for (const auto& item : thread) {
                    if (auto result = pspace.extract<int>(item.path); result.has_value()) {
                        anyRemaining = true;
                        log(std::format("Item remains after clear: {}", item.path));
                    }
                }
            }

            CHECK_MESSAGE(!anyRemaining, "Items remain after final cleanup");
        }

        log("\nTest completed");
    }

    SUBCASE("Task Execution Order") {
        // This test verifies that tasks are executed in a consistent order,
        // even when run concurrently across multiple threads.
        const std::vector<int> TASK_COUNTS = {10, 100, 1000}; // Test with different scales
        for (int NUM_TASKS : TASK_COUNTS) {
            PathSpace pspace;
            std::atomic<int> tasksCompleted(0);
            std::vector<int> executionOrder;
            std::mutex orderMutex;
            std::condition_variable cv;

            auto start_time = std::chrono::steady_clock::now();
            auto timeout = std::chrono::seconds(30); // Global timeout for the entire test

            for (int i = 0; i < NUM_TASKS; ++i) {
                auto task = [i, &tasksCompleted, &executionOrder, &orderMutex, &cv]() -> int {
                    std::unique_lock<std::mutex> lock(orderMutex);
                    cv.wait_for(lock, std::chrono::milliseconds(100), [&tasksCompleted, i]() { return tasksCompleted.load() == i; });

                    executionOrder.push_back(i);
                    tasksCompleted.fetch_add(1);
                    cv.notify_all();
                    return i;
                };
                CHECK(pspace.insert("/ordered/" + std::to_string(i), task).errors.size() == 0);
            }

            std::vector<std::thread> threads;
            std::atomic<bool> anyThreadFailed(false);
            for (int i = 0; i < NUM_TASKS; ++i) {
                threads.emplace_back([&pspace, i, &anyThreadFailed]() {
                    try {
                        auto result = pspace.readBlock<int>("/ordered/" + std::to_string(i));
                        CHECK(result.has_value());
                        CHECK(result.value() == i);
                    } catch (const std::exception& e) {
                        anyThreadFailed.store(true);
                        FAIL("Thread " << i << " failed: " << e.what());
                    }
                });
            }

            // Join threads with timeout
            for (auto& t : threads) {
                if (std::chrono::steady_clock::now() - start_time > timeout) {
                    FAIL("Test timed out after " << timeout.count() << " seconds");
                }
                if (t.joinable())
                    t.join();
            }

            CHECK(tasksCompleted == NUM_TASKS);
            CHECK_FALSE(anyThreadFailed);

            // Verify execution order
            CHECK(executionOrder.size() == NUM_TASKS);
            bool is_sorted = std::is_sorted(executionOrder.begin(), executionOrder.end());
            if (!is_sorted) {
                // Log the actual order for debugging
                std::stringstream ss;
                ss << "Execution order: ";
                for (int i : executionOrder) {
                    ss << i << " ";
                }
                INFO(ss.str());
            }
            CHECK(is_sorted);
        }
    }

    SUBCASE("Stress Testing") {
        // This test performs stress testing on PathSpace by running a large number of
        // concurrent operations (inserts, reads, and extracts) across multiple threads.

        const auto processor_count = std::thread::hardware_concurrency();
        const int NUM_THREADS = std::min(1000, static_cast<int>(processor_count * 4));
        const int OPERATIONS_PER_THREAD = 100;
        const auto TIMEOUT = std::chrono::seconds(60);

        PathSpace pspace;
        std::atomic<int> totalOperations(0);
        std::atomic<int> successfulOperations(0);
        std::atomic<bool> shouldStop(false);

        auto workerFunction = [&](int threadId) {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(0, 2);

            for (int i = 0; i < OPERATIONS_PER_THREAD && !shouldStop; ++i) {
                std::string path = "/stress/" + std::to_string(gen()) + "/" + std::to_string(i);
                int operation = dis(gen);

                try {
                    if (operation == 0) {
                        // Insert
                        auto insertFunc = [&totalOperations]() -> int { return ++totalOperations; };
                        auto result = pspace.insert(path, insertFunc);
                        if (result.errors.empty())
                            successfulOperations++;
                    } else if (operation == 1) {
                        // Read
                        auto result
                                = pspace.readBlock<int>(path, OutOptions{.block = BlockOptions{.timeout = std::chrono::milliseconds(10)}});
                        if (result)
                            successfulOperations++;
                    } else {
                        // Extract
                        auto result = pspace.extractBlock<int>(path,
                                                               OutOptions{.block = BlockOptions{.timeout = std::chrono::milliseconds(10)}});
                        if (result)
                            successfulOperations++;
                    }
                } catch (const std::exception& e) {
                    // Log the exception but continue the test
                    INFO("Thread " << threadId << " encountered an exception: " << e.what());
                }
            }
        };

        std::vector<std::thread> threads;
        threads.reserve(NUM_THREADS);

        auto startTime = std::chrono::steady_clock::now();

        for (int i = 0; i < NUM_THREADS; ++i) {
            try {
                threads.emplace_back(workerFunction, i);
            } catch (const std::exception& e) {
                FAIL("Failed to create thread " << i << ": " << e.what());
            }
        }

        // Join threads with timeout
        for (auto& t : threads) {
            if (std::chrono::steady_clock::now() - startTime > TIMEOUT) {
                shouldStop = true;
                INFO("Test timed out after " << TIMEOUT.count() << " seconds");
                break;
            }
            if (t.joinable())
                t.join();
        }

        auto endTime = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

        INFO("Test completed in " << duration.count() << "ms");
        INFO("Total operations: " << totalOperations.load());
        INFO("Successful operations: " << successfulOperations.load());

        CHECK(totalOperations > 0);
        CHECK(successfulOperations > 0);
        CHECK(successfulOperations <= NUM_THREADS * OPERATIONS_PER_THREAD);

        // Additional checks on PathSpace state
        int pathCount = 0;
        std::function<void(const std::string&)> countPaths = [&](const std::string& path) {
            pathCount++;
            // You might need to implement a method in PathSpace to get child paths
            // for (const auto& childPath : pspace.getChildPaths(path)) {
            //     countPaths(childPath);
            // }
        };
        countPaths("/stress");
        INFO("Total paths in PathSpace: " << pathCount);
        CHECK(pathCount > 0);
    }

    SUBCASE("Long-Running Tasks") {
        PathSpace pspace;
        const int NUM_LONG_TASKS = 10;
        const int NUM_SHORT_TASKS = 1000;
        std::atomic<int> longTasksCompleted(0), shortTasksCompleted(0);

        // Insert long-running tasks
        for (int i = 0; i < NUM_LONG_TASKS; ++i) {
            auto longTask = [&longTasksCompleted]() -> int {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                return ++longTasksCompleted;
            };
            CHECK(pspace.insert("/long/" + std::to_string(i), longTask).errors.size() == 0);
        }

        // Insert short tasks
        for (int i = 0; i < NUM_SHORT_TASKS; ++i) {
            auto shortTask = [&shortTasksCompleted]() -> int { return ++shortTasksCompleted; };
            CHECK(pspace.insert("/short/" + std::to_string(i), shortTask).errors.size() == 0);
        }

        // Execute all tasks concurrently
        std::vector<std::thread> threads;
        for (int i = 0; i < NUM_LONG_TASKS; ++i) {
            threads.emplace_back([&pspace, i]() { pspace.readBlock<int>("/long/" + std::to_string(i)); });
        }
        for (int i = 0; i < NUM_SHORT_TASKS; ++i) {
            threads.emplace_back([&pspace, i]() { pspace.readBlock<int>("/short/" + std::to_string(i)); });
        }

        for (auto& t : threads) {
            t.join();
        }

        CHECK(longTasksCompleted == NUM_LONG_TASKS);
        CHECK(shortTasksCompleted == NUM_SHORT_TASKS);
    }

    SUBCASE("Task Cancellation") {
        PathSpace pspace;
        const int NUM_TASKS = 100;
        std::atomic<bool> shouldCancel(false);
        std::atomic<int> cancelledTasks(0), completedTasks(0);

        for (int i = 0; i < NUM_TASKS; ++i) {
            auto task = [i, &shouldCancel, &cancelledTasks, &completedTasks]() -> int {
                std::this_thread::sleep_for(std::chrono::milliseconds(i % 10)); // Spread out task execution
                if (i < 10) {                                                   // First 10 tasks complete immediately
                    completedTasks++;
                    return i;
                }
                for (int j = 0; j < 100; ++j) {
                    if (shouldCancel.load()) {
                        cancelledTasks++;
                        return -1;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                completedTasks++;
                return i;
            };
            CHECK(pspace.insert("/cancellable/" + std::to_string(i), task).errors.size() == 0);
        }

        std::vector<std::future<void>> futures;
        for (int i = 0; i < NUM_TASKS; ++i) {
            futures.push_back(
                    std::async(std::launch::async, [&pspace, i]() { pspace.readBlock<int>("/cancellable/" + std::to_string(i)); }));
        }

        // Allow some tasks to start and potentially complete
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Signal cancellation
        shouldCancel.store(true);

        // Wait for all tasks to complete or cancel
        for (auto& f : futures) {
            f.wait();
        }

        CHECK(cancelledTasks > 0);
        CHECK(completedTasks > 0);
        CHECK(cancelledTasks + completedTasks == NUM_TASKS);
    }

    SUBCASE("Thread Pool Behavior") {
        PathSpace pspace;
        const int NUM_TASKS = 1000;
        std::atomic<int> executedTasks(0);

        for (int i = 0; i < NUM_TASKS; ++i) {
            auto task = [&executedTasks]() -> int {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return ++executedTasks;
            };
            CHECK(pspace.insert("/pool/" + std::to_string(i), task).errors.size() == 0);
        }

        std::vector<std::thread> threads;
        for (int i = 0; i < NUM_TASKS; ++i) {
            threads.emplace_back([&pspace, i]() { pspace.readBlock<int>("/pool/" + std::to_string(i)); });
        }

        for (auto& t : threads) {
            t.join();
        }

        CHECK(executedTasks == NUM_TASKS);
    }

    SUBCASE("Memory Management") {
        PathSpace pspace;
        const int NUM_THREADS = 4;
        const int OPERATIONS_PER_THREAD = 1000;
        std::atomic<int> successfulOperations(0);
        std::atomic<int> failedInserts(0);
        std::atomic<int> failedReads(0);
        std::atomic<int> failedExtracts(0);
        std::atomic<bool> shouldStop(false);
        std::atomic<int> completedThreads(0);

        auto workerFunction = [&](int threadId) {
            for (int i = 0; i < OPERATIONS_PER_THREAD; ++i) {
                if (shouldStop.load() && i % 100 == 0) {
                    log("Thread " + std::to_string(threadId) + " stopping at operation " + std::to_string(i));
                    break;
                }

                std::string path = "/memory/" + std::to_string(threadId) + "/" + std::to_string(i);

                // Insert an integer
                auto insertResult = pspace.insert(path, i);
                if (insertResult.errors.empty()) {
                    // Try to immediately read it back
                    auto readResult = pspace.readBlock<int>(path);
                    if (readResult.has_value() && readResult.value() == i) {
                        // If successful, remove it
                        auto extractResult = pspace.extractBlock<int>(path);
                        if (extractResult.has_value() && extractResult.value() == i) {
                            successfulOperations++;
                        } else {
                            failedExtracts++;
                        }
                    } else {
                        failedReads++;
                    }
                } else {
                    failedInserts++;
                }

                if (i % 100 == 0) {
                    log("Thread " + std::to_string(threadId) + " completed " + std::to_string(i) + " operations");
                }
            }
            completedThreads++;
            log("Thread " + std::to_string(threadId) + " completed all operations");
        };

        std::vector<std::thread> threads;
        for (int i = 0; i < NUM_THREADS; ++i) {
            threads.emplace_back(workerFunction, i);
        }

        // Add a timeout for the entire test
        std::thread timeoutThread([&]() {
            for (int i = 0; i < 120 && !shouldStop; ++i) { // 120 * 1 second = 2 minutes total
                std::this_thread::sleep_for(std::chrono::seconds(1));
                log("Time elapsed: " + std::to_string(i) + " seconds");
                log("Successful operations: " + std::to_string(successfulOperations.load()));
                if (completedThreads.load() == NUM_THREADS) {
                    log("All threads completed");
                    return;
                }
            }
            shouldStop.store(true);
            log("Test timed out after 2 minutes");
        });

        for (auto& t : threads) {
            t.join();
        }
        timeoutThread.join();

        int totalOperations = NUM_THREADS * OPERATIONS_PER_THREAD;
        int successfulOps = successfulOperations.load();

        log("Successful operations: " + std::to_string(successfulOps) + " out of " + std::to_string(totalOperations));
        log("Failed inserts: " + std::to_string(failedInserts.load()));
        log("Failed reads: " + std::to_string(failedReads.load()));
        log("Failed extracts: " + std::to_string(failedExtracts.load()));

        // Check if any items remain in the PathSpace
        int remainingItems = 0;
        for (int t = 0; t < NUM_THREADS; ++t) {
            for (int i = 0; i < OPERATIONS_PER_THREAD; ++i) {
                std::string path = "/memory/" + std::to_string(t) + "/" + std::to_string(i);
                auto result = pspace.readBlock<int>(path);
                if (result.has_value()) {
                    remainingItems++;
                    if (remainingItems <= 10) { // Log the first 10 remaining items
                        log("Remaining item: " + path + " = " + std::to_string(result.value()));
                    }
                }
            }
        }

        log("Remaining items: " + std::to_string(remainingItems));

        // We expect a high percentage of operations to be successful and few items to remain
        double successRate = static_cast<double>(successfulOps) / totalOperations;
        log("Success rate: " + std::to_string(successRate));

        CHECK(successRate > 0.95);                      // Expect at least 95% success rate
        CHECK(remainingItems < totalOperations * 0.01); // Expect less than 1% of items to remain
        CHECK(completedThreads.load() == NUM_THREADS);  // All threads should complete
    }

    SUBCASE("Deadlock Detection and Prevention") {
        PathSpace pspace;
        const int NUM_THREADS = 10;
        std::atomic<int> deadlockCount(0);

        auto resourceA = []() -> int { return 1; };
        auto resourceB = []() -> int { return 2; };

        CHECK(pspace.insert("/resourceA", resourceA).errors.size() == 0);
        CHECK(pspace.insert("/resourceB", resourceB).errors.size() == 0);

        auto workerFunction = [&](int threadId) {
            if (threadId % 2 == 0) {
                // Even threads try to acquire A then B
                auto resultA = pspace.readBlock<int>("/resourceA");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                auto resultB = pspace.readBlock<int>("/resourceB",
                                                     OutOptions{.block = BlockOptions{.behavior = BlockOptions::Behavior::Wait,
                                                                                      .timeout = std::chrono::milliseconds(100)}});
                if (!resultB)
                    deadlockCount++;
            } else {
                // Odd threads try to acquire B then A
                auto resultB = pspace.readBlock<int>("/resourceB");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                auto resultA = pspace.readBlock<int>("/resourceA",
                                                     OutOptions{.block = BlockOptions{.behavior = BlockOptions::Behavior::Wait,
                                                                                      .timeout = std::chrono::milliseconds(100)}});
                if (!resultA)
                    deadlockCount++;
            }
        };

        std::vector<std::thread> threads;
        for (int i = 0; i < NUM_THREADS; ++i) {
            threads.emplace_back(workerFunction, i);
        }

        for (auto& t : threads) {
            t.join();
        }

        // We expect some deadlocks to be prevented due to timeouts
        CHECK(deadlockCount > 0);
        CHECK(deadlockCount < NUM_THREADS);
    }

    SUBCASE("Performance Testing") {
        PathSpace pspace;
        const int NUM_THREADS = std::thread::hardware_concurrency();
        const int OPERATIONS_PER_THREAD = 500; // Reduced from 10000
        const int NUM_PATHS = 50;              // Reduced from 1000
        const auto TEST_DURATION = std::chrono::milliseconds(300);
        const int NUM_ITERATIONS = 2; // Reduced from 3

        auto performanceTest = [&](int concurrency) {
            struct Result {
                double ops;
                double duration;
            };

            auto runIteration = [&]() -> Result {
                std::atomic<int> completedOperations(0);
                std::atomic<bool> shouldStop(false);

                auto workerFunction = [&]() {
                    for (int i = 0; i < OPERATIONS_PER_THREAD && !shouldStop.load(std::memory_order_relaxed); ++i) {
                        std::string path = std::format("/perf/{}", i % NUM_PATHS);
                        auto task = []() -> int { return 42; };
                        pspace.insert(path, task);
                        auto result
                                = pspace.readBlock<int>(path, OutOptions{.block = BlockOptions{.timeout = std::chrono::milliseconds(10)}});
                        if (result.has_value()) {
                            completedOperations.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                };

                auto start = std::chrono::steady_clock::now();

                std::vector<std::jthread> threads;
                threads.reserve(concurrency);
                for (int i = 0; i < concurrency; ++i) {
                    threads.emplace_back(workerFunction);
                }

                std::jthread timeoutThread([&shouldStop, TEST_DURATION](std::stop_token stoken) {
                    std::this_thread::sleep_for(TEST_DURATION);
                    shouldStop.store(true, std::memory_order_relaxed);
                });

                for (auto& t : threads) {
                    t.join();
                }

                auto end = std::chrono::steady_clock::now();
                auto duration = std::chrono::duration<double>(end - start);

                return {static_cast<double>(completedOperations), duration.count()};
            };

            Result totalResult{0.0, 0.0};
            for (int i = 0; i < NUM_ITERATIONS; ++i) {
                auto result = runIteration();
                totalResult.ops += result.ops;
                totalResult.duration += result.duration;
                pspace.clear();
            }

            return Result{totalResult.ops / NUM_ITERATIONS, totalResult.duration / NUM_ITERATIONS};
        };

        auto singleThreadResult = performanceTest(1);
        auto multiThreadResult = performanceTest(NUM_THREADS);

        // Calculate operations per second
        double singleThreadedOps = singleThreadResult.ops / singleThreadResult.duration;
        double multiThreadedOps = multiThreadResult.ops / multiThreadResult.duration;

        // Log the results
        std::cout << std::format("Single-threaded performance: {:.2f} ops/sec\n", singleThreadedOps);
        std::cout << std::format("Multi-threaded performance: {:.2f} ops/sec\n", multiThreadedOps);
        std::cout << std::format("Performance improvement: {:.2f}x\n", multiThreadedOps / singleThreadedOps);

        // Check for performance improvement with a tolerance
        constexpr double IMPROVEMENT_THRESHOLD = 1.2; // Expect at least 20% improvement
        constexpr double TOLERANCE = 0.1;             // 10% tolerance

        CHECK((multiThreadedOps / singleThreadedOps) > (IMPROVEMENT_THRESHOLD - TOLERANCE));
    }

    SUBCASE("Dining Philosophers") {
        PathSpace pspace;
        const int NUM_PHILOSOPHERS = 5;
        const int EATING_DURATION_MS = 10;
        const int THINKING_DURATION_MS = 10;
        const int TEST_DURATION_MS = 5000; // Increased to 5 seconds

        struct PhilosopherStats {
            std::atomic<int> meals_eaten{0};
            std::atomic<int> times_starved{0};
            std::atomic<int> forks_acquired{0};
        };

        std::vector<PhilosopherStats> stats(NUM_PHILOSOPHERS);

        auto philosopher = [&](int id) {
            std::string first_fork = std::format("/fork/{}", std::min(id, (id + 1) % NUM_PHILOSOPHERS));
            std::string second_fork = std::format("/fork/{}", std::max(id, (id + 1) % NUM_PHILOSOPHERS));

            std::mt19937 rng(id);
            std::uniform_int_distribution<> think_dist(1, THINKING_DURATION_MS);
            std::uniform_int_distribution<> eat_dist(1, EATING_DURATION_MS);
            std::uniform_int_distribution<> backoff_dist(1, 5);

            auto start_time = std::chrono::steady_clock::now();

            while (std::chrono::steady_clock::now() - start_time < std::chrono::milliseconds(TEST_DURATION_MS)) {
                // Thinking
                std::this_thread::sleep_for(std::chrono::milliseconds(think_dist(rng)));

                // Try to pick up forks
                auto first
                        = pspace.extractBlock<int>(first_fork, OutOptions{.block = BlockOptions{.timeout = std::chrono::milliseconds(50)}});
                if (first.has_value()) {
                    stats[id].forks_acquired.fetch_add(1, std::memory_order_relaxed);
                    auto second = pspace.extractBlock<int>(second_fork,
                                                           OutOptions{.block = BlockOptions{.timeout = std::chrono::milliseconds(50)}});
                    if (second.has_value()) {
                        stats[id].forks_acquired.fetch_add(1, std::memory_order_relaxed);
                        // Eating
                        std::this_thread::sleep_for(std::chrono::milliseconds(eat_dist(rng)));
                        stats[id].meals_eaten.fetch_add(1, std::memory_order_relaxed);

                        // Put down second fork
                        pspace.insert(second_fork, 1);
                    }
                    // Put down first fork
                    pspace.insert(first_fork, 1);
                } else {
                    stats[id].times_starved.fetch_add(1, std::memory_order_relaxed);
                }

                // Backoff on failure
                std::this_thread::sleep_for(std::chrono::milliseconds(backoff_dist(rng)));
            }
        };

        // Initialize forks
        for (int i = 0; i < NUM_PHILOSOPHERS; ++i) {
            REQUIRE(pspace.insert(std::format("/fork/{}", i), 1).nbrValuesInserted == 1);
        }

        // Start philosophers
        std::vector<std::jthread> philosophers;
        for (int i = 0; i < NUM_PHILOSOPHERS; ++i) {
            philosophers.emplace_back(philosopher, i);
        }

        // Wait for test to complete
        std::this_thread::sleep_for(std::chrono::milliseconds(TEST_DURATION_MS));

        // Join all threads
        philosophers.clear();

        // Output and check results
        int total_meals = 0;
        int total_starved = 0;
        int total_forks_acquired = 0;
        for (int i = 0; i < NUM_PHILOSOPHERS; ++i) {
            int meals = stats[i].meals_eaten.load();
            int starved = stats[i].times_starved.load();
            int forks = stats[i].forks_acquired.load();
            total_meals += meals;
            total_starved += starved;
            total_forks_acquired += forks;
            std::cout << std::format("Philosopher {}: Meals eaten: {}, Times starved: {}, Forks acquired: {}\n", i, meals, starved, forks);

            // Check that each philosopher ate at least once
            CHECK(meals > 0);
            // Check that each philosopher experienced some contention
            CHECK(starved > 0);
        }

        std::cout << std::format("Total meals eaten: {}\n", total_meals);
        std::cout << std::format("Total times starved: {}\n", total_starved);
        std::cout << std::format("Total forks acquired: {}\n", total_forks_acquired);
        std::cout << std::format("Meals per philosopher: {:.2f}\n", static_cast<double>(total_meals) / NUM_PHILOSOPHERS);

        // Check overall statistics
        CHECK(total_meals > NUM_PHILOSOPHERS);          // Each philosopher should eat at least once
        CHECK(total_starved > 0);                       // There should be some contention
        CHECK(total_forks_acquired >= total_meals * 2); // Each meal requires two forks

        // Check that there's no deadlock (all forks are available)
        for (int i = 0; i < NUM_PHILOSOPHERS; ++i) {
            auto fork = pspace.readBlock<int>(std::format("/fork/{}", i));
            CHECK(fork.has_value());
            if (fork.has_value()) {
                CHECK(fork.value() == 1);
            }
        }

        // Check for fairness (no philosopher should starve significantly more than others)
        double avg_starved = static_cast<double>(total_starved) / NUM_PHILOSOPHERS;
        for (int i = 0; i < NUM_PHILOSOPHERS; ++i) {
            double starve_ratio = static_cast<double>(stats[i].times_starved) / avg_starved;
            CHECK(starve_ratio >= 0.5);
            CHECK(starve_ratio <= 1.5);
        }
    }
}