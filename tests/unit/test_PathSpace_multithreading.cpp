#include "ext/doctest.h"
#include <atomic>
#include <chrono>
#include <future>
#include <pathspace/PathSpace.hpp>
#include <pathspace/core/Capabilities.hpp>
#include <random>
#include <thread>
#include <vector>

using namespace SP;

TEST_CASE("PathSpace Multithreading") {
    /**
     * Tests PathSpace's thread-safety and concurrent operation capabilities.
     *
     * Test Structure:
     * - Creates 8 threads total (NUM_THREADS)
     * - Half are writers, half are readers
     * - Each thread performs 100 operations (OPERATIONS_PER_THREAD)
     * - Uses 3 shared paths that all threads can access
     *
     * Thread Behaviors:
     * 1. Writer Threads
     *    - Alternates between shared paths (50%) and thread-specific paths (50%)
     *    - Each write has a unique value (threadId * 1000 + i)
     *    - Adds random delays to increase race condition chances
     *    - Tracks success/failure of each insert
     *
     * 2. Reader Threads
     *    - Accesses shared paths (33%) and random thread paths (66%)
     *    - Randomly chooses between read and extract operations
     *    - Uses 50ms timeout to prevent deadlocks
     *    - Tracks success/failure and values read
     *
     * Verification Aspects:
     * 1. Operation Counts
     *    - Verifies total operations match expectations
     *    - Checks insert/read/extract ratios
     *
     * 2. Data Consistency
     *    - Groups operations by path
     *    - Verifies each read value matches a previous insert
     *    - Ensures readers never see invalid data
     *
     * 3. Shared Path Contention
     *    - Verifies shared paths are accessed concurrently
     *    - Checks each shared path has sufficient concurrent access
     *
     * 4. Error Analysis
     *    - Verifies only expected error types occur (Timeout, NoSuchPath)
     *    - Ensures error counts are reasonable
     *    - Checks timeout frequency
     *
     * 5. Success Rate
     *    - Ensures overall operation success rate >50%
     *
     * Race Conditions Tested:
     * - Read-Write races (concurrent access to shared paths)
     * - Extract-Write races (data removal during writes)
     * - Path contention (multiple threads accessing shared paths)
     * - Timing variations (random sleeps and timeouts)
     *
     * Thread Safety Aspects Verified:
     * - Data integrity (valid values, no duplicates, no corruption)
     * - Concurrency control (shared access, deadlock prevention)
     * - Error handling (appropriate errors, timeout behavior)
     */
    SUBCASE("Basic Concurrent Operations") {
        struct Operation {
            enum class Type {
                Insert,
                Read,
                Extract
            };
            Type type = Type::Insert; // Default values for all members
            int threadId = -1;
            int operationId = -1;
            int value = 0;
            bool success = false;
            std::string path;
            Error::Code error = Error::Code::NoSuchPath; // Changed from errorCode and given default
        };
        PathSpace pspace;
        const int NUM_THREADS = 8;
        const int OPERATIONS_PER_THREAD = 100;

        std::vector<std::string> shared_paths = {"/shared/counter", "/shared/accumulator", "/shared/status"};

        std::vector<Operation> operations;
        operations.reserve(NUM_THREADS * OPERATIONS_PER_THREAD);
        std::mutex operations_mutex;

        auto writer_worker = [&](int threadId) {
            std::mt19937 rng(threadId);
            std::uniform_int_distribution<> path_dist(0, shared_paths.size() - 1);

            for (int i = 0; i < OPERATIONS_PER_THREAD; i++) {
                std::string path = (i % 2 == 0) ? shared_paths[path_dist(rng)] : "/thread/" + std::to_string(threadId) + "/value";

                int value = threadId * 1000 + i;
                auto result = pspace.insert(path, value);

                {
                    std::lock_guard<std::mutex> lock(operations_mutex);
                    operations.push_back(Operation{.type = Operation::Type::Insert,
                                                   .threadId = threadId,
                                                   .operationId = i,
                                                   .value = value,
                                                   .success = result.errors.empty(),
                                                   .path = path,
                                                   .error = result.errors.empty() ? Error::Code::NoSuchPath : result.errors.front().code});
                }

                if (i % 5 == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        };

        auto reader_worker = [&](int threadId) {
            std::mt19937 rng(threadId);
            std::uniform_int_distribution<> path_dist(0, shared_paths.size() - 1);
            std::uniform_int_distribution<> op_dist(0, 2);

            for (int i = 0; i < OPERATIONS_PER_THREAD; i++) {
                std::string path = (i % 3 == 0) ? shared_paths[path_dist(rng)] : "/thread/" + std::to_string(i % NUM_THREADS) + "/value";

                OutOptions options{.block
                                   = BlockOptions{.behavior = BlockOptions::Behavior::Wait, .timeout = std::chrono::milliseconds(6)}};

                Operation::Type opType = (op_dist(rng) == 0) ? Operation::Type::Extract : Operation::Type::Read;

                Expected<int> result = (opType == Operation::Type::Extract) ? pspace.extractBlock<int>(path, options)
                                                                            : pspace.readBlock<int>(path, options);

                {
                    std::lock_guard<std::mutex> lock(operations_mutex);
                    operations.push_back(Operation{.type = opType,
                                                   .threadId = threadId,
                                                   .operationId = i,
                                                   .value = result.has_value() ? result.value() : -1,
                                                   .success = result.has_value(),
                                                   .path = path,
                                                   .error = result.has_value() ? Error::Code::NoSuchPath : result.error().code});
                }
            }
        };

        std::vector<std::thread> threads;
        for (int i = 0; i < NUM_THREADS; i++) {
            if (i < NUM_THREADS / 2) {
                threads.emplace_back(writer_worker, i);
            } else {
                threads.emplace_back(reader_worker, i);
            }
        }

        for (auto& t : threads) {
            t.join();
        }

        SUBCASE("Operation Counts") {
            size_t insert_count = std::count_if(operations.begin(), operations.end(), [](const Operation& op) {
                return op.type == Operation::Type::Insert;
            });
            size_t read_count = std::count_if(operations.begin(), operations.end(), [](const Operation& op) {
                return op.type == Operation::Type::Read;
            });
            size_t extract_count = std::count_if(operations.begin(), operations.end(), [](const Operation& op) {
                return op.type == Operation::Type::Extract;
            });

            CHECK(insert_count == (NUM_THREADS / 2) * OPERATIONS_PER_THREAD);
            CHECK(read_count + extract_count == (NUM_THREADS / 2) * OPERATIONS_PER_THREAD);
        }

        SUBCASE("Data Consistency") {
            std::map<std::string, std::vector<Operation>> path_operations;
            for (const auto& op : operations) {
                path_operations[op.path].push_back(op);
            }

            for (const auto& [path, ops] : path_operations) {
                for (const auto& op : ops) {
                    if (op.success && op.type != Operation::Type::Insert) {
                        bool found_insert = false;
                        for (const auto& prev_op : ops) {
                            if (prev_op.type == Operation::Type::Insert && prev_op.value == op.value) {
                                found_insert = true;
                                break;
                            }
                        }
                        CHECK(found_insert);
                    }
                }
            }
        }

        SUBCASE("Shared Path Contention") {
            for (const auto& shared_path : shared_paths) {
                auto path_ops = std::count_if(operations.begin(), operations.end(), [&](const Operation& op) {
                    return op.path == shared_path && op.success;
                });
                CHECK(path_ops > NUM_THREADS);
            }
        }

        SUBCASE("Error Analysis") {
            std::map<Error::Code, int> error_counts;
            for (const auto& op : operations) {
                if (!op.success) {
                    error_counts[op.error]++; // Changed from errorCode to error
                }
            }

            for (const auto& [code, count] : error_counts) {
                bool is_expected_error = code == Error::Code::Timeout || code == Error::Code::NoSuchPath;

                CHECK(is_expected_error);
                CHECK(count > 0);
                CHECK(count < NUM_THREADS * OPERATIONS_PER_THREAD);

                if (code == Error::Code::Timeout) {
                    CHECK(count < (NUM_THREADS * OPERATIONS_PER_THREAD) / 4);
                }
            }
        }

        SUBCASE("Operation Success Rate") {
            int total_ops = operations.size();
            int successful_ops = std::count_if(operations.begin(), operations.end(), [](const Operation& op) { return op.success; });

            double success_rate = static_cast<double>(successful_ops) / total_ops;
            CHECK(success_rate > 0.5);
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

    SUBCASE("Concurrent Path Creation") {
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

    /*SUBCASE("Blocking Operations") {
        PathSpace pspace;
        const int NUM_PRODUCERS = 10;
        const int NUM_CONSUMERS = 10;
        const int ITEMS_PER_PRODUCER = 100;
        std::atomic<int> producedCount(0), consumedCount(0);

        auto producerFunction = [&](int id) {
            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                std::string path = "/queue/" + std::to_string(id) + "/" + std::to_string(i);
                auto produceFunc = [&producedCount]() -> int { return ++producedCount; };
                CHECK(pspace.insert(path, produceFunc).errors.size() == 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        };

        auto consumerFunction = [&]() {
            while (consumedCount < NUM_PRODUCERS * ITEMS_PER_PRODUCER) {
                std::string path = "/queue/" + std::to_string(rand() % NUM_PRODUCERS) + "/" + std::to_string(rand() % ITEMS_PER_PRODUCER);
                auto result = pspace.extractBlock<int>(path,
                                                       OutOptions{.block = BlockOptions{.behavior = BlockOptions::Behavior::Wait,
                                                                                        .timeout = std::chrono::milliseconds(100)}});
                if (result)
                    consumedCount++;
            }
        };

        std::vector<std::thread> producers, consumers;
        for (int i = 0; i < NUM_PRODUCERS; ++i) {
            producers.emplace_back(producerFunction, i);
        }
        for (int i = 0; i < NUM_CONSUMERS; ++i) {
            consumers.emplace_back(consumerFunction);
        }

        for (auto& t : producers) {
            t.join();
        }
        for (auto& t : consumers) {
            t.join();
        }

        CHECK(producedCount == NUM_PRODUCERS * ITEMS_PER_PRODUCER);
        CHECK(consumedCount == NUM_PRODUCERS * ITEMS_PER_PRODUCER);
    }

    SUBCASE("Task Execution Order") {
        PathSpace pspace;
        const int NUM_TASKS = 1000;
        std::atomic<int> lastExecuted(0);

        for (int i = 0; i < NUM_TASKS; ++i) {
            auto task = [i, &lastExecuted]() -> int {
                int expected = i - 1;
                while (!lastExecuted.compare_exchange_weak(expected, i)) {
                    expected = i - 1;
                    std::this_thread::yield();
                }
                return i;
            };
            CHECK(pspace.insert("/ordered/" + std::to_string(i), task).errors.size() == 0);
        }

        std::vector<std::thread> threads;
        for (int i = 0; i < NUM_TASKS; ++i) {
            threads.emplace_back([&pspace, i]() {
                auto result = pspace.readBlock<int>("/ordered/" + std::to_string(i));
                CHECK(result.has_value());
                CHECK(result.value() == i);
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        CHECK(lastExecuted == NUM_TASKS - 1);
    }

    SUBCASE("Stress Testing") {
        PathSpace pspace;
        const int NUM_THREADS = 1000;
        const int OPERATIONS_PER_THREAD = 100;
        std::atomic<int> totalOperations(0);

        auto workerFunction = [&]() {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(0, 2);

            for (int i = 0; i < OPERATIONS_PER_THREAD; ++i) {
                std::string path = "/stress/" + std::to_string(gen()) + "/" + std::to_string(i);
                int operation = dis(gen);

                if (operation == 0) {
                    // Insert
                    auto insertFunc = [&totalOperations]() -> int { return ++totalOperations; };
                    pspace.insert(path, insertFunc);
                } else if (operation == 1) {
                    // Read
                    pspace.readBlock<int>(path);
                } else {
                    // Extract
                    pspace.extractBlock<int>(path);
                }
            }
        };

        std::vector<std::thread> threads;
        for (int i = 0; i < NUM_THREADS; ++i) {
            threads.emplace_back(workerFunction);
        }

        for (auto& t : threads) {
            t.join();
        }

        CHECK(totalOperations > 0);
        CHECK(totalOperations <= NUM_THREADS * OPERATIONS_PER_THREAD);
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

    SUBCASE("Error Handling in Multithreaded Execution") {
        PathSpace pspace;
        const int NUM_THREADS = 100;
        const int OPERATIONS_PER_THREAD = 100;
        std::atomic<int> errorCount(0);

        auto workerFunction = [&](int threadId) {
            for (int i = 0; i < OPERATIONS_PER_THREAD; ++i) {
                std::string path = "/error/" + std::to_string(threadId) + "/" + std::to_string(i);

                if (i % 3 == 0) {
                    // Insert a task that might throw an exception
                    auto errorTask = [i]() -> int {
                        if (i % 2 == 0)
                            throw std::runtime_error("Simulated error");
                        return i;
                    };
                    auto result = pspace.insert(path, errorTask);
                    if (!result.errors.empty())
                        errorCount++;
                } else {
                    // Try to read, which might encounter an error
                    auto result = pspace.readBlock<int>(path);
                    if (!result)
                        errorCount++;
                }
            }
        };

        std::vector<std::thread> threads;
        for (int i = 0; i < NUM_THREADS; ++i) {
            threads.emplace_back(workerFunction, i);
        }

        for (auto& t : threads) {
            t.join();
        }

        CHECK(errorCount > 0);
        CHECK(errorCount < NUM_THREADS * OPERATIONS_PER_THREAD);
    }

    SUBCASE("Task Cancellation") {
        PathSpace pspace;
        const int NUM_TASKS = 100;
        std::atomic<bool> shouldCancel(false);
        std::atomic<int> cancelledTasks(0), completedTasks(0);

        for (int i = 0; i < NUM_TASKS; ++i) {
            auto task = [i, &shouldCancel, &cancelledTasks, &completedTasks]() -> int {
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

        // Allow some tasks to start
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

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
        const int NUM_THREADS = 100;
        const int OPERATIONS_PER_THREAD = 1000;
        std::atomic<int> allocCount(0), deallocCount(0);

        auto workerFunction = [&](int threadId) {
            for (int i = 0; i < OPERATIONS_PER_THREAD; ++i) {
                std::string path = "/memory/" + std::to_string(threadId) + "/" + std::to_string(i);

                if (i % 2 == 0) {
                    // Allocate memory
                    auto allocTask = [&allocCount]() -> int* {
                        allocCount++;
                        return new int(42);
                    };
                    CHECK(pspace.insert(path, allocTask).errors.size() == 0);
                } else {
                    // Deallocate memory
                    auto result = pspace.extractBlock<int*>(path);
                    if (result) {
                        delete *result;
                        deallocCount++;
                    }
                }
            }
        };

        std::vector<std::thread> threads;
        for (int i = 0; i < NUM_THREADS; ++i) {
            threads.emplace_back(workerFunction, i);
        }

        for (auto& t : threads) {
            t.join();
        }

        CHECK(allocCount == deallocCount);
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
        const int NUM_THREADS = 100;
        const int OPERATIONS_PER_THREAD = 10000;

        auto performanceTest = [&](int concurrency) {
            std::atomic<int> completedOperations(0);

            auto workerFunction = [&]() {
                for (int i = 0; i < OPERATIONS_PER_THREAD; ++i) {
                    std::string path = "/perf/" + std::to_string(rand());
                    if (i % 2 == 0) {
                        auto task = []() -> int { return 42; };
                        pspace.insert(path, task);
                    } else {
                        pspace.readBlock<int>(path);
                    }
                    completedOperations++;
                }
            };

            auto start = std::chrono::high_resolution_clock::now();

            std::vector<std::thread> threads;
            for (int i = 0; i < concurrency; ++i) {
                threads.emplace_back(workerFunction);
            }

            for (auto& t : threads) {
                t.join();
            }

            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

            return std::make_pair(completedOperations.load(), duration.count());
        };

        auto singleThreadResult = performanceTest(1);
        auto multiThreadResult = performanceTest(NUM_THREADS);

        // Check that multi-threaded performance is better than single-threaded
        CHECK(multiThreadResult.second < singleThreadResult.second);

        // Calculate operations per second
        double singleThreadedOps = static_cast<double>(singleThreadResult.first) / singleThreadResult.second * 1000.0;
        double multiThreadedOps = static_cast<double>(multiThreadResult.first) / multiThreadResult.second * 1000.0;

        // Log the results
        std::cout << "Single-threaded performance: " << singleThreadedOps << " ops/sec" << std::endl;
        std::cout << "Multi-threaded performance: " << multiThreadedOps << " ops/sec" << std::endl;
        std::cout << "Performance improvement: " << (multiThreadedOps / singleThreadedOps) << "x" << std::endl;

        // We expect at least some performance improvement with multi-threading
        CHECK(multiThreadedOps > singleThreadedOps);
    }

    SUBCASE("Concurrent Capabilities Testing") {
        PathSpace pspace;
        const int NUM_THREADS = 100;
        const int OPERATIONS_PER_THREAD = 100;
        std::atomic<int> successfulOperations(0), failedOperations(0);

        // Set up capabilities
        Capabilities readCap, writeCap, fullCap;
        readCap.addCapability("/read", Capabilities::Type::READ);
        writeCap.addCapability("/write", Capabilities::Type::WRITE);
        fullCap.addCapability("/full", Capabilities::Type::READ);
        fullCap.addCapability("/full", Capabilities::Type::WRITE);

        auto workerFunction = [&](int threadId) {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(0, 2);

            for (int i = 0; i < OPERATIONS_PER_THREAD; ++i) {
                int operation = dis(gen);
                std::string path;
                Capabilities cap;

                switch (operation) {
                    case 0: // Read operation
                        path = "/read/data";
                        cap = readCap;
                        break;
                    case 1: // Write operation
                        path = "/write/data";
                        cap = writeCap;
                        break;
                    case 2: // Full access operation
                        path = "/full/data";
                        cap = fullCap;
                        break;
                }

                if (operation == 1 || operation == 2) {
                    // Insert operation
                    auto insertFunc = []() -> int { return 42; };
                    auto result = pspace.insert(path, insertFunc, InOptions{.capabilities = cap});
                    if (result.errors.empty()) {
                        successfulOperations++;
                    } else {
                        failedOperations++;
                    }
                } else {
                    // Read operation
                    auto result = pspace.readBlock<int>(path, OutOptions{.capabilities = cap});
                    if (result.has_value()) {
                        successfulOperations++;
                    } else {
                        failedOperations++;
                    }
                }
            }
        };

        std::vector<std::thread> threads;
        for (int i = 0; i < NUM_THREADS; ++i) {
            threads.emplace_back(workerFunction, i);
        }

        for (auto& t : threads) {
            t.join();
        }

        CHECK(successfulOperations > 0);
        CHECK(failedOperations > 0);
        CHECK(successfulOperations + failedOperations == NUM_THREADS * OPERATIONS_PER_THREAD);
    }*/
}