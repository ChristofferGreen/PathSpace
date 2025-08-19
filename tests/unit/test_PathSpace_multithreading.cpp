#include <pathspace/PathSpace.hpp>
#include <pathspace/core/Error.hpp>
#include <pathspace/core/ExecutionCategory.hpp>
#include <pathspace/core/In.hpp>
#include <pathspace/core/Out.hpp>

#include "ext/doctest.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <format>
#include <future>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

using namespace SP;
using namespace std::chrono_literals;

TEST_CASE("PathSpace Multithreading") {
    SUBCASE("PathSpace Basic Concurrent Operations") {
        PathSpace  pspace;
        const int  NUM_THREADS           = 8;
        const int  OPERATIONS_PER_THREAD = 100;
        const int  MAX_RETRIES           = 3;
        const auto TEST_TIMEOUT          = std::chrono::seconds(30);

        struct Stats {
            std::atomic<size_t>           totalOps{0};
            std::atomic<size_t>           successfulOps{0};
            std::atomic<size_t>           failedOps{0};
            std::atomic<size_t>           timeouts{0};
            mutable std::mutex            mtx;
            std::map<std::string, size_t> pathAccesses;

            void recordAccess(const std::string& path) {
                std::lock_guard<std::mutex> lock(mtx);
                pathAccesses[path]++;
            }

            auto getStats() const {
                std::lock_guard<std::mutex> lock(mtx);
                return std::make_tuple(totalOps.load(std::memory_order_acquire), successfulOps.load(std::memory_order_acquire), failedOps.load(std::memory_order_acquire), timeouts.load(std::memory_order_acquire), pathAccesses);
            }
        };

        // Shared state with proper synchronization
        struct SharedState {
            Stats                   stats;
            std::atomic<bool>       shouldStop{false};
            std::atomic<bool>       readersCanStart{false};
            std::atomic<int>        insertCount{0};
            std::mutex              cvMutex;
            std::condition_variable readerStartCV;

            void signalReadersToStart() {
                {
                    std::lock_guard<std::mutex> lock(cvMutex);
                    readersCanStart.store(true, std::memory_order_release);
                }
                readerStartCV.notify_all();
            }

            bool shouldContinue() const {
                return !shouldStop.load(std::memory_order_acquire);
            }

            void stop() {
                shouldStop.store(true, std::memory_order_release);
                readerStartCV.notify_all();
            }
        } state;

        // Fixed set of paths to ensure contention
        const std::vector<std::string> shared_paths = {"/shared/counter", "/shared/accumulator", "/shared/status"};

        // Thread-safe path generation
        auto getPath = [&shared_paths](int threadId, int opId, std::mt19937& rng) -> std::string {
            std::uniform_int_distribution<> dist(0, shared_paths.size() - 1);
            // Use shared paths 50% of the time
            if (opId % 2 == 0) {
                return shared_paths[dist(rng)];
            }
            return std::format("/seq/{}/{}", threadId, opId);
        };

        // Writer function with proper synchronization
        auto writer = [&](int threadId) {
            std::mt19937 rng(std::random_device{}() + threadId);
            try {
                for (int i = 0; i < OPERATIONS_PER_THREAD && state.shouldContinue(); i++) {
                    std::string path  = getPath(threadId, i, rng);
                    int         value = threadId * 1000 + i;

                    bool inserted = false;
                    for (int attempt = 0; attempt < MAX_RETRIES && !inserted && state.shouldContinue(); attempt++) {
                        if (auto result = pspace.insert(path, value); result.errors.empty()) {
                            inserted = true;
                            state.insertCount.fetch_add(1, std::memory_order_release);
                            state.stats.successfulOps.fetch_add(1, std::memory_order_release);
                            state.stats.recordAccess(path);

                            if (state.insertCount.load(std::memory_order_acquire) > OPERATIONS_PER_THREAD / 2) {
                                state.signalReadersToStart();
                            }
                            break;
                        }

                        if (attempt < MAX_RETRIES - 1) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(1 << attempt));
                        }
                    }

                    if (!inserted) {
                        state.stats.failedOps.fetch_add(1, std::memory_order_release);
                    }
                    state.stats.totalOps.fetch_add(1, std::memory_order_release);

                    if (i % 10 == 0) {
                        std::this_thread::yield();
                    }
                }
            } catch (const std::exception& e) {
                sp_log(std::format("Writer {} error: {}", threadId, e.what()), "INFO");
                state.stats.failedOps.fetch_add(1, std::memory_order_release);
            }
        };

        // Reader function with proper synchronization
        auto reader = [&](int threadId) {
            std::mt19937 rng(std::random_device{}() + threadId);

            // Wait for writers to populate data
            {
                std::unique_lock<std::mutex> lock(state.cvMutex);
                state.readerStartCV.wait(lock, [&]() { return state.readersCanStart.load(std::memory_order_acquire) || state.shouldStop.load(std::memory_order_acquire); });
            }

            try {
                for (int i = 0; i < OPERATIONS_PER_THREAD && state.shouldContinue(); i++) {
                    std::string path = getPath(threadId % (NUM_THREADS / 2), i, rng);

                    Out options = Block(50ms);

                    // Try read first, fall back to extract
                    auto result = pspace.read<int>(path, options);
                    if (!result.has_value()) {
                        result = pspace.take<int>(path, options);
                    }

                    if (result.has_value()) {
                        state.stats.successfulOps.fetch_add(1, std::memory_order_release);
                        state.stats.recordAccess(path);
                    } else {
                        if (result.error().code == Error::Code::Timeout) {
                            state.stats.timeouts.fetch_add(1, std::memory_order_release);
                        }
                        state.stats.failedOps.fetch_add(1, std::memory_order_release);
                    }

                    state.stats.totalOps.fetch_add(1, std::memory_order_release);

                    // Small delay to reduce contention
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            } catch (const std::exception& e) {
                sp_log(std::format("Reader {} error: {}", threadId, e.what()), "INFO");
                state.stats.failedOps.fetch_add(1, std::memory_order_release);
            }
        };

        // Launch threads with proper cleanup
        {
            std::vector<std::thread> threads;
            auto                      testStart = std::chrono::steady_clock::now();

            // Start writers
            for (int i = 0; i < NUM_THREADS / 2; ++i) {
                threads.emplace_back(writer, i);
            }

            // Start readers
            for (int i = NUM_THREADS / 2; i < NUM_THREADS; ++i) {
                threads.emplace_back(reader, i);
            }

            // Monitor progress
            while (true) {
                auto [total, successful, failed, timeouts, _] = state.stats.getStats();

                if (total >= NUM_THREADS * OPERATIONS_PER_THREAD || std::chrono::steady_clock::now() - testStart > TEST_TIMEOUT) {
                    state.stop();
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            for (auto& t : threads) { if (t.joinable()) t.join(); }
        }

        // Verify results
        auto [totalOps, successfulOps, failedOps, timeouts, pathAccesses] = state.stats.getStats();

        // Calculate rates
        double successRate = static_cast<double>(successfulOps) / totalOps * 100;
        double errorRate   = static_cast<double>(failedOps) / totalOps;

        // Output statistics for debugging
        INFO("Total operations: " << totalOps);
        INFO("Successful operations: " << successfulOps);
        INFO("Failed operations: " << failedOps);
        INFO("Timeouts: " << timeouts);
        INFO("Success rate: " << successRate << "%");
        INFO("Error rate: " << errorRate * 100 << "%");

        // Verify metrics
        CHECK_MESSAGE(successRate > 90.0, std::format("Success rate too low: {:.1f}%", successRate));
        CHECK_MESSAGE(errorRate < 0.1, std::format("Error rate too high: {:.1f}%", errorRate * 100));

        // Verify shared path contention
        for (const auto& path : shared_paths) {
            size_t accesses = pathAccesses[path];
            CHECK_MESSAGE(accesses > NUM_THREADS, std::format("Insufficient contention on shared path {}: {} accesses", path, accesses));
        }

        // Verify cleanup
        pspace.clear();
        for (const auto& [path, _] : pathAccesses) {
            auto result = pspace.read<int>(path);
            CHECK_MESSAGE(!result.has_value(), std::format("Data remains at path: {}", path));
        }
    }

    SUBCASE("PathSpace Concurrent Counter") {
        PathSpace pspace;
        const int NUM_THREADS           = std::min(16, static_cast<int>(std::thread::hardware_concurrency() * 2));
        const int OPERATIONS_PER_THREAD = 100;

        std::atomic<int> failedOperations{0};
        std::atomic<int> successfulOperations{0};

        // Structure to track thread operations
        struct ThreadStats {
            std::vector<int> insertedValues;
            int              threadId;
            int              successCount{0};
            int              failCount{0};

            ThreadStats(int id)
                : threadId(id) {
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
            auto result = pspace.take<int>("/data");
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
            int threadId     = value / OPERATIONS_PER_THREAD;
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
        const int NUM_THREADS           = 4;
        const int OPERATIONS_PER_THREAD = 10;

        // Track operations as they're queued
        struct Operation {
            int threadId;
            int seqNum;
            int value;
        };
        std::vector<Operation> expectedOperations;
        std::mutex             opsMutex;

        // Track any test failures in worker threads
        std::atomic<bool> workerFailure{false};
        std::string       workerErrorMsg;
        std::mutex        errorMutex;

        auto workerFunction = [&](int threadId) {
            try {
                for (int i = 0; i < OPERATIONS_PER_THREAD; i++) {
                    int value = (threadId * 100) + i;

                    // Insert our value
                    auto result = pspace.insert("/counter", value);
                    if (!result.errors.empty()) {
                        std::lock_guard<std::mutex> lock(errorMutex);
                        workerErrorMsg = "Insert failed: " + result.errors[0].message.value_or("Unknown error");
                        workerFailure  = true;
                        return;
                    }

                    // Record this operation
                    {
                        std::lock_guard<std::mutex> lock(opsMutex);
                        expectedOperations.push_back({threadId, i, value});
                    }

                    // Small delay to help interleave operations
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(errorMutex);
                workerErrorMsg = "Worker thread exception: " + std::string(e.what());
                workerFailure  = true;
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

        // Check if any worker threads failed
        if (workerFailure) {
            FAIL(workerErrorMsg);
        }

        // Extract all values and verify order
        std::vector<Operation> actualOperations;
        int                    expectedCount  = NUM_THREADS * OPERATIONS_PER_THREAD;
        int                    extractedCount = 0;
        int                    timeoutCount   = 0;
        const int              MAX_TIMEOUTS   = 10; // Allow some timeouts before giving up

        while (extractedCount < expectedCount) {
            auto value = pspace.take<int>("/counter", Block(1000ms));

            if (!value.has_value()) {
                if (value.error().code == Error::Code::Timeout) {
                    timeoutCount++;
                    if (timeoutCount > MAX_TIMEOUTS) {
                        std::stringstream ss;
                        ss << "Too many extraction timeouts. Extracted " << extractedCount << " of " << expectedCount << " values";
                        FAIL(ss.str());
                    }
                    continue; // Try again
                }
                std::stringstream ss;
                ss << "Unexpected error during extraction: " << value.error().message.value_or("Unknown error");
                FAIL(ss.str());
            }

            timeoutCount = 0; // Reset timeout counter on successful extraction

            // Find matching operation
            bool      found = false;
            Operation matchingOp;

            for (const auto& op : expectedOperations) {
                if (op.value == value.value()) {
                    matchingOp = op;
                    found      = true;
                    break;
                }
            }

            std::stringstream ss;
            ss << "Extracted value " << value.value() << " not found in expected operations";
            REQUIRE_MESSAGE(found, ss.str());

            actualOperations.push_back(matchingOp);
            extractedCount++;
        }

        // Verify we got all operations
        {
            std::stringstream ss;
            ss << "Expected " << expectedCount << " operations, got " << actualOperations.size();
            CHECK_MESSAGE(actualOperations.size() == expectedCount, ss.str());
        }

        // Verify per-thread ordering (operations from same thread should be in sequence)
        for (int t = 0; t < NUM_THREADS; t++) {
            std::vector<int> threadSeqNums;

            for (const auto& op : actualOperations) {
                if (op.threadId == t) {
                    threadSeqNums.push_back(op.seqNum);
                }
            }

            // Print sequence for debugging if test fails
            if (!std::is_sorted(threadSeqNums.begin(), threadSeqNums.end()) || threadSeqNums.size() != OPERATIONS_PER_THREAD) {
                INFO("Thread " << t << " sequence:");
                for (int seq : threadSeqNums) {
                    INFO("  " << seq);
                }
            }

            // Check that this thread's operations are in order
            {
                std::stringstream ss;
                ss << "Operations for thread " << t << " are not in order";
                CHECK_MESSAGE(std::is_sorted(threadSeqNums.begin(), threadSeqNums.end()), ss.str());
            }
            {
                std::stringstream ss;
                ss << "Thread " << t << " has " << threadSeqNums.size() << " operations, expected " << OPERATIONS_PER_THREAD;
                CHECK_MESSAGE(threadSeqNums.size() == OPERATIONS_PER_THREAD, ss.str());
            }
        }

        // Verify no more values exist
        auto extraValue = pspace.take<int>("/counter");
        CHECK_MESSAGE(!extraValue.has_value(), "Found unexpected extra value in PathSpace after test completion");
    }

    SUBCASE("Mixed Readers and Writers") {
        sp_log("Mixed Readers and Writers: START", "TestMRW");
        PathSpace pspace;
        const int NUM_WRITERS       = 4;
        const int NUM_READERS       = 4;
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
                auto value = pspace.read<int>("/mixed", Block{});
                if (value.has_value()) {
                    readsCompleted++;
                }

                // Occasionally check alternate path
                if (readsCompleted % 10 == 0) {
                    auto altValue = pspace.read<int>("/mixed_alt", Block{});
                    if (altValue.has_value()) {
                        readsCompleted++;
                    }
                }

                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        };

        auto extractorFunction = [&]() {
            while (writesCompleted < (NUM_WRITERS * VALUES_PER_WRITER)) {
                auto value = pspace.take<int>("/mixed", Block{});
                if (value.has_value()) {
                    extractsCompleted++;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        };

        // Launch threads
        std::vector<std::thread> threads;

        // Start readers and extractors first
        sp_log("MRW: launching readers and extractors", "TestMRW");
        for (int i = 0; i < NUM_READERS / 2; i++) {
            threads.emplace_back(readerFunction);
            threads.emplace_back(extractorFunction);
        }

        // Then start writers
        sp_log("MRW: launching writers", "TestMRW");
        for (int i = 0; i < NUM_WRITERS; i++) {
            threads.emplace_back(writerFunction, i);
        }

        // Join writer threads first (writers were appended last)
        for (int i = 0; i < NUM_WRITERS; ++i) {
            auto& wt = threads[threads.size() - 1 - i];
            if (wt.joinable()) wt.join();
        }

        // Wake indefinite waiters, then join reader/extractor threads
        sp_log(std::format("MRW: before shutdownPublic, writesCompleted={}", writesCompleted.load()), "TestMRW");
        pspace.shutdownPublic();
        sp_log("MRW: after shutdownPublic", "TestMRW");

        // Join remaining reader/extractor threads
        for (size_t i = 0; i < threads.size() - NUM_WRITERS; ++i) {
            auto& t = threads[i];
            if (t.joinable()) t.join();
        }

        sp_log(std::format("MRW: final counts reads={}, extracts={}, writes={}", readsCompleted.load(), extractsCompleted.load(), writesCompleted.load()), "TestMRW");
        // Verify operations
        CHECK(writesCompleted == NUM_WRITERS * VALUES_PER_WRITER);
        INFO("Reads completed: " << readsCompleted);
        INFO("Extracts completed: " << extractsCompleted);
        CHECK(readsCompleted > 0);
        CHECK(extractsCompleted > 0);
    }

    SUBCASE("PathSpace Multiple Path Operations") {
        PathSpace pspace;
        const int NUM_THREADS      = 4;
        const int PATHS_PER_THREAD = 3;
        const int OPS_PER_PATH     = 50;

        struct PathOperation {
            std::string path;
            int         threadId;
            int         seqNum;
            int         value;
        };

        std::vector<std::vector<PathOperation>> threadOperations(NUM_THREADS);
        std::mutex                              opsMutex;

        // Track any test failures in worker threads
        std::atomic<bool> workerFailure{false};
        std::string       workerErrorMsg;
        std::mutex        errorMutex;

        auto workerFunction = [&](int threadId) {
            try {
                std::vector<std::string> paths;
                for (int p = 0; p < PATHS_PER_THREAD; p++) {
                    paths.push_back("/path" + std::to_string(threadId) + "_" + std::to_string(p));
                }

                for (int i = 0; i < OPS_PER_PATH; i++) {
                    for (const auto& path : paths) {
                        int value = (threadId * 1000000) + (i * 1000);

                        auto result = pspace.insert(path, value);
                        if (!result.errors.empty()) {
                            std::lock_guard<std::mutex> lock(errorMutex);
                            workerErrorMsg = "Insert failed: " + result.errors[0].message.value_or("Unknown error");
                            workerFailure  = true;
                            return;
                        }

                        {
                            std::lock_guard<std::mutex> lock(opsMutex);
                            threadOperations[threadId].push_back({path, threadId, i, value});
                        }
                    }
                }
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(errorMutex);
                workerErrorMsg = "Worker thread exception: " + std::string(e.what());
                workerFailure  = true;
            }
        };

        SUBCASE("Order Preservation") {
            // Launch threads
            std::vector<std::thread> threads;
            for (int i = 0; i < NUM_THREADS; i++) {
                threads.emplace_back(workerFunction, i);
            }

            for (auto& t : threads) {
                t.join();
            }

            // Check if any worker threads failed
            if (workerFailure) {
                FAIL(workerErrorMsg);
            }

            // Verify each path's operations
            for (int t = 0; t < NUM_THREADS; t++) {
                for (int p = 0; p < PATHS_PER_THREAD; p++) {
                    std::string      path = "/path" + std::to_string(t) + "_" + std::to_string(p);
                    std::vector<int> seqNums;
                    int              expectedValues = OPS_PER_PATH;
                    int              timeoutCount   = 0;
                    const int        MAX_TIMEOUTS   = 10;

                    // Extract all values from this path
                    while (seqNums.size() < expectedValues) {
                        auto value = pspace.take<int>(path, Block(1000ms));

                        if (!value.has_value()) {
                            if (value.error().code == Error::Code::Timeout) {
                                timeoutCount++;
                                if (timeoutCount > MAX_TIMEOUTS) {
                                    std::stringstream ss;
                                    ss << "Too many extraction timeouts on path " << path << ". Got " << seqNums.size() << " of " << expectedValues << " values";
                                    FAIL(ss.str());
                                }
                                continue;
                            }
                            std::stringstream ss;
                            ss << "Error extracting from path " << path << ": " << value.error().message.value_or("Unknown error");
                            FAIL(ss.str());
                        }

                        timeoutCount = 0; // Reset timeout counter on successful extraction

                        // Find matching operation
                        auto& ops = threadOperations[t];
                        auto  it  = std::find_if(ops.begin(), ops.end(), [&](const PathOperation& op) { return op.path == path && op.value == value.value(); });

                        std::stringstream ss;
                        ss << "Value " << value.value() << " not found in operations for path " << path;
                        REQUIRE_MESSAGE(it != ops.end(), ss.str());
                        seqNums.push_back(it->seqNum);
                    }

                    // Verify we got the expected number of values
                    {
                        std::stringstream ss;
                        ss << "Path " << path << " has " << seqNums.size() << " operations, expected " << expectedValues;
                        CHECK_MESSAGE(seqNums.size() == expectedValues, ss.str());
                    }

                    // Verify sequence ordering
                    {
                        std::stringstream ss;
                        ss << "Operations not in order for path " << path;
                        if (!std::is_sorted(seqNums.begin(), seqNums.end())) {
                            ss << "\nSequence: ";
                            for (int seq : seqNums) {
                                ss << seq << " ";
                            }
                        }
                        CHECK_MESSAGE(std::is_sorted(seqNums.begin(), seqNums.end()), ss.str());
                    }

                    // Verify no more values exist
                    auto extraValue = pspace.take<int>(path);
                    {
                        std::stringstream ss;
                        ss << "Found unexpected extra value in path " << path << " after test completion";
                        CHECK_MESSAGE(!extraValue.has_value(), ss.str());
                    }
                }
            }
        }
    }

    SUBCASE("PathSpace Read-Extract Race Conditions") {
        PathSpace pspace;
        const int NUM_VALUES = 100; // Smaller number for clearer testing

        // Pre-populate with known values
        for (int i = 0; i < NUM_VALUES; i++) {
            auto              result = pspace.insert("/race", i);
            std::stringstream ss;
            ss << "Failed to insert initial value " << i;
            REQUIRE_MESSAGE(result.errors.empty(), ss.str());
        }

        std::vector<int>  extractedValues;
        std::mutex        extractMutex;
        std::atomic<bool> extractionComplete{false};
        std::atomic<int>  readCount{0};

        SUBCASE("Concurrent Read-Extract Operations") {
            // Track thread errors
            std::string       readerError, extractorError;
            std::mutex        errorMutex;
            std::atomic<bool> hasError{false};

            // Reader thread - reads values continuously until extraction is complete
            std::thread readerThread([&]() {
                try {
                    const int MAX_FAILED_READS    = 10;
                    int       consecutiveFailures = 0;

                    while (readCount < NUM_VALUES * 100 && !extractionComplete) { // Allow more reads than values
                        auto value = pspace.read<int>("/race");
                        if (value.has_value()) {
                            readCount++;
                            consecutiveFailures = 0;
                        } else {
                            consecutiveFailures++;
                            if (consecutiveFailures >= MAX_FAILED_READS) {
                                if (extractionComplete)
                                    break;
                                consecutiveFailures = 0;
                            }
                            std::this_thread::sleep_for(std::chrono::microseconds(100));
                        }
                    }
                } catch (const std::exception& e) {
                    std::lock_guard<std::mutex> lock(errorMutex);
                    readerError = "Reader thread exception: ";
                    readerError += e.what();
                    hasError = true;
                }
            });

            // Extractor thread - extracts values with timeout
            std::thread extractorThread([&]() {
                try {
                    int       timeoutCount   = 0;
                    const int MAX_TIMEOUTS   = 10;
                    int       extractedCount = 0;

                    while (extractedCount < NUM_VALUES) {
                        auto value = pspace.take<int>("/race", Block(1000ms));

                        if (!value.has_value()) {
                            if (value.error().code == Error::Code::Timeout) {
                                timeoutCount++;
                                if (timeoutCount > MAX_TIMEOUTS) {
                                    std::lock_guard<std::mutex> lock(errorMutex);
                                    std::stringstream           ss;
                                    ss << "Too many extraction timeouts. Got " << extractedCount << " of " << NUM_VALUES << " values";
                                    extractorError = ss.str();
                                    hasError       = true;
                                    break;
                                }
                                continue;
                            }
                            std::lock_guard<std::mutex> lock(errorMutex);
                            extractorError = "Extraction error: ";
                            extractorError += value.error().message.value_or("Unknown error");
                            hasError = true;
                            break;
                        }

                        timeoutCount = 0; // Reset timeout counter on successful extraction
                        {
                            std::lock_guard<std::mutex> lock(extractMutex);
                            extractedValues.push_back(value.value());
                        }
                        extractedCount++;
                    }

                    extractionComplete = true;
                } catch (const std::exception& e) {
                    std::lock_guard<std::mutex> lock(errorMutex);
                    extractorError = "Extractor thread exception: ";
                    extractorError += e.what();
                    hasError = true;
                }
            });

            // Wait for threads to complete
            readerThread.join();
            extractorThread.join();

            // Check for thread errors
            if (hasError) {
                std::stringstream ss;
                if (!readerError.empty())
                    ss << readerError;
                if (!extractorError.empty())
                    ss << extractorError;
                FAIL(ss.str());
            }

            // Verify results
            {
                std::lock_guard<std::mutex> lock(extractMutex);
                std::sort(extractedValues.begin(), extractedValues.end());

                {
                    std::stringstream ss;
                    ss << "Expected " << NUM_VALUES << " values, got " << extractedValues.size();
                    REQUIRE_MESSAGE(extractedValues.size() == NUM_VALUES, ss.str());
                }

                for (int i = 0; i < NUM_VALUES; i++) {
                    std::stringstream ss;
                    ss << "Value mismatch at position " << i << ": expected " << i << ", got " << extractedValues[i];
                    REQUIRE_MESSAGE(extractedValues[i] == i, ss.str());
                }
            }

            // Verify queue is empty with timeouts
            {
                auto finalRead = pspace.read<int>("/race", Block(200ms));
                CHECK_MESSAGE(!finalRead.has_value(), "Expected no more values to read");
            }

            {
                auto finalExtract = pspace.take<int>("/race", Block(1000ms));
                CHECK_MESSAGE(!finalExtract.has_value(), "Expected no more values to extract");
            }
        }
    }

    SUBCASE("Concurrent Path Creation") {
        PathSpace pspace;
        const int NUM_THREADS      = 8;
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
                    std::string path  = basePath + "/path" + std::to_string(i) + "/depth" + std::to_string(depth);
                    auto        value = pspace.take<int>(path, Block{});
                    REQUIRE(value.has_value());
                    CHECK(value.value() == i);
                }
            }
        }
    }

    SUBCASE("Blocking Operations") {
        PathSpace pspace;
        const int NUM_THREADS      = 4;
        const int ITEMS_PER_THREAD = 50;

        struct TestData {
            std::string path;
            int         value;
            bool        extracted{false};
        };

        std::vector<std::vector<TestData>> threadData(NUM_THREADS);

        // Phase 1: Insert data
        sp_log("Phase 1: Inserting data", "INFO");
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

            sp_log(std::format("Inserted {} items", NUM_THREADS * ITEMS_PER_THREAD), "INFO");
        }

        // Phase 2: Extract data with multiple threads
        sp_log("Phase 2: Extracting data", "INFO");
        {
            std::atomic<int>  extractedCount = 0;
            std::atomic<bool> shouldStop     = false;

            auto extractWorker = [&](int threadId) {
                auto& items = threadData[threadId];
                for (auto& item : items) {
                    if (shouldStop)
                        break;
                    if (item.extracted)
                        continue;

                    auto result = pspace.take<int>(item.path, Block(100ms));

                    if (result.has_value()) {
                        CHECK(result.value() == item.value);
                        item.extracted = true;
                        extractedCount++;
                    }
                }
            };

            // Launch extraction threads
            {
                std::vector<std::thread> threads;
                for (int t = 0; t < NUM_THREADS; ++t) {
                    threads.emplace_back(extractWorker, t);
                }

                for (auto& t : threads) { t.detach(); }
                // Monitor progress
                auto start = std::chrono::steady_clock::now();
                while (extractedCount < NUM_THREADS * ITEMS_PER_THREAD) {
                    if (std::chrono::steady_clock::now() - start > std::chrono::seconds(10)) {
                        shouldStop = true;
                        sp_log("Extraction timeout reached", "INFO");
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    sp_log(std::format("Extracted: {}/{}", extractedCount.load(), NUM_THREADS * ITEMS_PER_THREAD), "INFO");
                }
            }

            sp_log(std::format("Extracted {} items", extractedCount.load()), "INFO");
        }

        // Phase 3: Verify and cleanup any remaining items
        sp_log("Phase 3: Verification and cleanup", "INFO");
        {
            int                      remainingItems = 0;
            std::vector<std::string> remainingPaths;

            // First pass: Try to extract any remaining items
            for (auto& thread : threadData) {
                for (auto& item : thread) {
                    if (item.extracted)
                        continue;

                    // Try to extract the item
                    auto result = pspace.take<int>(item.path);
                    if (result.has_value()) {
                        remainingItems++;
                        remainingPaths.push_back(item.path);
                    }
                }
            }

            if (!remainingPaths.empty()) {
                sp_log("Found remaining items:", "INFO");
                for (const auto& path : remainingPaths) {
                    sp_log(std::format("  {}", path), "INFO");
                }

                // Try one more time to extract these items
                for (const auto& path : remainingPaths) {
                    if (auto result = pspace.take<int>(path); result.has_value()) {
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
                    if (auto result = pspace.take<int>(item.path); result.has_value()) {
                        anyRemaining = true;
                        sp_log(std::format("Item remains after clear: {}", item.path), "INFO");
                    }
                }
            }

            CHECK_MESSAGE(!anyRemaining, "Items remain after final cleanup");
        }

        sp_log("Test completed", "INFO");
    }

    SUBCASE("Task Execution Order") {
        const int               NUM_TASKS = 5;
        SP::PathSpace           pspace;
        std::vector<int>        executionOrder;
        std::mutex              mutex;
        std::condition_variable cv;
        std::atomic<int>        tasksCompleted{0};

        INFO("Starting test with ", NUM_TASKS, " tasks");

        // Insert tasks
        for (int i = 0; i < NUM_TASKS; ++i) {
            auto task = [i, &mutex, &cv, &executionOrder, &tasksCompleted]() -> int {
                std::unique_lock<std::mutex> lock(mutex);

                while (tasksCompleted.load() != i) {
                    cv.wait(lock); // Simplified: no timeout needed
                }

                executionOrder.push_back(i);
                tasksCompleted.fetch_add(1);
                cv.notify_all();
                return i;
            };

            REQUIRE(pspace.insert("/task/" + std::to_string(i), task, SP::In{.executionCategory = SP::ExecutionCategory::Lazy}).errors.empty());
        }

        // Execute tasks in sequence
        INFO("Executing tasks in sequence");
        for (int i = 0; i < NUM_TASKS; ++i) {
            auto result = pspace.read<int>("/task/" + std::to_string(i), Block{});
            REQUIRE(result.has_value());
            CHECK(result.value() == i);
        }

        // Verify results
        CHECK(executionOrder.size() == NUM_TASKS);
        CHECK(std::is_sorted(executionOrder.begin(), executionOrder.end()));
    }

    // Simplified but effective stress test that maintains good coverage
    SUBCASE("Stress Testing") {
        class ConcurrentTester {
        public:
            static void runTest(PathSpace& space) {
                // Configuration - smaller numbers but still sufficient for testing
                const int  NUM_THREADS    = 4;
                const int  OPS_PER_THREAD = 100;
                const auto TIMEOUT        = std::chrono::seconds(5);

                std::atomic<int>         success_count{0};
                std::atomic<bool>        has_error{false};
                std::vector<std::thread> threads;

                // Launch worker threads
                for (int t = 0; t < NUM_THREADS; ++t) {
                    threads.emplace_back([&, t]() {
                        for (int i = 0; i < OPS_PER_THREAD && !has_error.load(); ++i) {
                            try {
                                // Use just a few paths to increase contention
                                std::string path = "/stress/" + std::to_string(i % 3);

                                // Randomly choose operation
                                switch (i % 3) {
                                    case 0: {
                                        // Insert
                                        auto result = space.insert(path, i);
                                        if (result.errors.empty()) {
                                            success_count++;
                                        }
                                        break;
                                    }
                                    case 1: {
                                        // Read with short timeout
                                        auto result = space.read<int>(path, Block(10ms));
                                        if (result)
                                            success_count++;
                                        break;
                                    }
                                    case 2: {
                                        // Extract with short timeout
                                        auto result = space.take<int>(path, Block(10ms));
                                        if (result)
                                            success_count++;
                                        break;
                                    }
                                }
                            } catch (const std::exception& e) {
                                has_error = true;
                                break;
                            }
                        }
                    });
                }

                // Wait for completion with timeout
                auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
                for (auto& thread : threads) {
                    auto now = std::chrono::steady_clock::now();
                    if (now >= deadline) {
                        has_error = true;
                    }
                    if (thread.joinable()) {
                        thread.join();
                    }
                }

                // Verify results
                CHECK_FALSE_MESSAGE(has_error, "No errors occurred during stress test");
                CHECK_MESSAGE(success_count > 0, "Some operations succeeded");

                // Verify final state
                bool paths_exist = false;
                for (int i = 0; i < 3; ++i) {
                    std::string path = "/stress/" + std::to_string(i);
                    if (space.read<int>(path).has_value()) {
                        paths_exist = true;
                        break;
                    }
                }
                CHECK_MESSAGE(paths_exist, "Some data remains in PathSpace");
            }
        };

        PathSpace space;
        ConcurrentTester::runTest(space);
    }

    class TestCounter {
    public:
        void increment() {
            std::lock_guard<std::mutex> lock(mutex);
            count++;
            cv.notify_all();
        }

        bool wait_for_count(int target, std::chrono::seconds timeout) {
            std::unique_lock<std::mutex> lock(mutex);
            return cv.wait_for(lock, timeout, [&]() { return count >= target; });
        }

        int get_count() const {
            std::lock_guard<std::mutex> lock(mutex);
            return count;
        }

        void reset() {
            std::lock_guard<std::mutex> lock(mutex);
            count = 0;
        }

    private:
        mutable std::mutex      mutex;
        std::condition_variable cv;
        int                     count = 0;
    };

    SUBCASE("Concurrent Task Execution - Task Reading") {
        PathSpace space;
        const int READERS        = 3;
        const int ITERATIONS     = 5;
        const int EXPECTED_COUNT = READERS * ITERATIONS;

        TestCounter       counter;
        std::atomic<bool> has_error{false};

        // Insert initial value
        REQUIRE(space.insert("/shared/task", 42).nbrValuesInserted == 1);

        auto reader_func = [&](int reader_id) {
            try {
                for (int j = 0; j < ITERATIONS && !has_error; j++) {
                    auto result = space.read<int>("/shared/task", Block(10ms));

                    if (result && result.value() == 42) {
                        counter.increment();
                    } else {
                        has_error = true;
                        break;
                    }
                }
            } catch (const std::exception& e) {
                has_error = true;
            }
        };

        // Launch reader threads in controlled scope
        {
            std::vector<std::thread> readers;
            readers.reserve(READERS);

            for (int i = 0; i < READERS; i++) {
                readers.emplace_back(reader_func, i);
            }

            // Wait for completion using TestCounter's built-in wait functionality
            bool completed = counter.wait_for_count(EXPECTED_COUNT, std::chrono::seconds(5));

            // Verify results
            CHECK_FALSE_MESSAGE(has_error, "No errors should occur during concurrent reads");
            CHECK_MESSAGE(completed, "All reads should complete within timeout");
            CHECK_MESSAGE(counter.get_count() == EXPECTED_COUNT, "Expected " << EXPECTED_COUNT << " reads, got " << counter.get_count());

            for (auto& t : readers) { if (t.joinable()) t.join(); }
        }

        // Cleanup
        space.clear();
    }

    SUBCASE("Concurrent Task Execution - Task Reading") {
        PathSpace space;
        const int READERS        = 3;
        const int ITERATIONS     = 5;
        const int EXPECTED_COUNT = READERS * ITERATIONS;

        TestCounter       counter;
        std::atomic<bool> has_error{false};

        // Insert initial value
        REQUIRE(space.insert("/shared/task", 42).nbrValuesInserted == 1);

        auto reader_func = [&](int reader_id) {
            try {
                for (int j = 0; j < ITERATIONS && !has_error; j++) {
                    auto result = space.read<int>("/shared/task", Block(10ms));

                    if (result && result.value() == 42) {
                        counter.increment();
                    } else {
                        has_error = true;
                        break;
                    }
                }
            } catch (const std::exception& e) {
                has_error = true;
            }
        };

        // Launch reader threads in controlled scope
        {
            std::vector<std::thread> readers;
            readers.reserve(READERS);

            for (int i = 0; i < READERS; i++) {
                readers.emplace_back(reader_func, i);
            }

            // Wait for completion using TestCounter's built-in wait functionality
            bool completed = counter.wait_for_count(EXPECTED_COUNT, std::chrono::seconds(5));

            // Verify results
            CHECK_FALSE_MESSAGE(has_error, "No errors should occur during concurrent reads");
            CHECK_MESSAGE(completed, "All reads should complete within timeout");
            CHECK_MESSAGE(counter.get_count() == EXPECTED_COUNT, "Expected " << EXPECTED_COUNT << " reads, got " << counter.get_count());

            for (auto& t : readers) { if (t.joinable()) t.join(); }
        }

        // Cleanup
        space.clear();
    }

    SUBCASE("Concurrent Task Execution - Timeout Handling") {
        PathSpace space;

        // Try to read from a path that doesn't exist yet, with timeout
        auto read_result = space.read<int>("/missing_task", Block(100ms));

        // Should timeout because no data was ever inserted
        CHECK_MESSAGE(!read_result.has_value(), "Expected timeout, but got value: ", read_result.has_value() ? std::to_string(read_result.value()) : "timeout");

        if (!read_result.has_value()) {
            CHECK_MESSAGE(read_result.error().code == Error::Code::Timeout, "Expected timeout error, got different error code: ", static_cast<int>(read_result.error().code));
        }
    }

    SUBCASE("Concurrent Task Execution - Tasks should both complete and timeout") {
        PathSpace space;

        // Insert a fast task that should complete
        auto fast_insert = space.insert(
                "/fast_task",
                []() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    return 42;
                },
                In{.executionCategory = ExecutionCategory::Lazy});
        REQUIRE_MESSAGE(fast_insert.errors.empty(), "Failed to insert fast task");

        // Read fast task - should complete
        auto fast_result = space.read<int>("/fast_task", Block(200ms));

        CHECK_MESSAGE(fast_result.has_value(), "Fast task should complete but got timeout");

        if (fast_result.has_value()) {
            CHECK_MESSAGE(fast_result.value() == 42, "Fast task returned wrong value: ", fast_result.value());
        }

        // Now try reading from a non-existent path - should timeout
        auto timeout_result = space.read<int>("/missing_task", Block(100ms));

        CHECK_MESSAGE(!timeout_result.has_value(), "Expected timeout for missing task");

        if (!timeout_result.has_value()) {
            CHECK_MESSAGE(timeout_result.error().code == Error::Code::Timeout, "Expected timeout error but got: ", static_cast<int>(timeout_result.error().code));
        }

        // Clean up
        space.clear();
    }

    SUBCASE("Concurrent Task Execution - Tasks should both complete and timeout Execution") {
        PathSpace space;

        // Insert a fast task that should complete
        auto fast_insert = space.insert(
                "/fast_task",
                []() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    return 23;
                },
                In{.executionCategory = ExecutionCategory::Lazy});
        REQUIRE_MESSAGE(fast_insert.errors.empty(), "Failed to insert fast task");
        auto slow_insert = space.insert(
                "/slow_task",
                []() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    return 24;
                },
                In{.executionCategory = ExecutionCategory::Lazy});
        REQUIRE_MESSAGE(slow_insert.errors.empty(), "Failed to insert fast task");

        // Read fast task - should complete
        auto fast_result = space.read<int>("/fast_task", Block(200ms));

        CHECK_MESSAGE(fast_result.has_value(), "Fast task should complete but got timeout");

        if (fast_result.has_value()) {
            CHECK_MESSAGE(fast_result.value() == 23, "Fast task returned wrong value: ", fast_result.value());
        }

        // Now try reading from a slow execution- should timeout
        auto timeout_result = space.read<int>("/slow_task", Block(100ms));

        CHECK_MESSAGE(!timeout_result.has_value(), std::format("Expected timeout for slow task, result has value: {}", timeout_result.value()));

        if (!timeout_result.has_value()) {
            CHECK_MESSAGE(timeout_result.error().code == Error::Code::Timeout, "Expected timeout error but got: ", static_cast<int>(timeout_result.error().code));
        }
    }

    SUBCASE("Concurrent Task Execution - Error Handling") {
        return;
        MESSAGE("Starting Error Handling test");
        PathSpace space;

        // Store a normal value first
        REQUIRE(space.insert("/error/test", 42).errors.empty());

        // Store an error-generating task
        auto error_task = []() -> int { throw std::runtime_error("Expected test error"); };

        REQUIRE(space.insert("/error/task", error_task, In{.executionCategory = ExecutionCategory::Lazy}).errors.empty());

        // First verify the good path works
        auto good_result = space.read<int>("/error/test");
        CHECK_MESSAGE(good_result.has_value(), "Good path should return value");
        CHECK_MESSAGE(good_result.value() == 42, "Good path should return correct value");

        // Then verify error handling
        auto error_result = space.read<int>("/error/task", Block(100ms));
    }

    SUBCASE("Task Cancellation with Enhanced Control") {
        PathSpace pspace;

        // Shared state
        std::atomic<int>  completed{0};
        std::atomic<bool> cancel_flag{false};

        // Create and submit short tasks that should complete
        const int SHORT_TASKS = 3;
        const int LONG_TASKS  = 3;

        // Submit short tasks that should complete quickly
        for (int i = 0; i < SHORT_TASKS; i++) {
            auto task = [i, &completed]() -> int {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                completed.fetch_add(1, std::memory_order_release);
                return i;
            };

            CHECK(pspace.insert("/short/" + std::to_string(i), task).errors.empty());
        }

        // Wait briefly for short tasks to complete
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Set cancellation flag
        cancel_flag.store(true, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        // Submit longer tasks that should check cancellation
        for (int i = 0; i < LONG_TASKS; i++) {
            auto task = [i, &completed, &cancel_flag]() -> int {
                if (cancel_flag.load(std::memory_order_acquire)) {
                    return -1;
                }

                // Simulate longer work
                std::this_thread::sleep_for(std::chrono::milliseconds(50));

                // Check cancellation again before completing
                if (!cancel_flag.load(std::memory_order_acquire)) {
                    completed.fetch_add(1, std::memory_order_release);
                    return i;
                }
                return -1;
            };

            // Use ExecutionCategory::Immediate to ensure the task runs right away
            CHECK(pspace.insert("/long/" + std::to_string(i), task).errors.empty());
        }

        // Wait briefly to ensure all tasks have been picked up
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Check results
        int final_completed = completed.load(std::memory_order_acquire);
        INFO("Tasks completed: " << final_completed);

        // Only the short tasks should have completed
        CHECK(final_completed == SHORT_TASKS);
    }

    SUBCASE("Thread Pool Behavior") {
        PathSpace        pspace;
        const int        NUM_TASKS = 1000;
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
            threads.emplace_back([&pspace, i]() { pspace.read<int>("/pool/" + std::to_string(i), Block{}); });
        }

        for (auto& t : threads) {
            t.join();
        }

        CHECK(executedTasks == NUM_TASKS);
    }

    SUBCASE("Memory Management") {
        PathSpace         pspace;
        const int         NUM_THREADS           = 4;
        const int         OPERATIONS_PER_THREAD = 1000;
        std::atomic<int>  successfulOperations(0);
        std::atomic<int>  failedInserts(0);
        std::atomic<int>  failedReads(0);
        std::atomic<int>  failedExtracts(0);
        std::atomic<bool> shouldStop(false);
        std::atomic<int>  completedThreads(0);

        // Add more detailed error tracking
        std::atomic<int> insertTimeouts(0);
        std::atomic<int> readTimeouts(0);
        std::atomic<int> extractTimeouts(0);

        auto workerFunction = [&](int threadId) {
            try {
                // Log thread start
                sp_log("Thread " + std::to_string(threadId) + " starting", "INFO");

                for (int i = 0; i < OPERATIONS_PER_THREAD && !shouldStop.load(std::memory_order_acquire); ++i) {
                    std::string path            = "/memory/" + std::to_string(threadId) + "/" + std::to_string(i);
                    bool        operationLogged = false;

                    // Insert operation
                    auto insertResult = pspace.insert(path, i);
                    if (!insertResult.errors.empty()) {
                        failedInserts.fetch_add(1, std::memory_order_relaxed);
                        if (!operationLogged) {
                            sp_log("Thread " + std::to_string(threadId) + " insert failed at " + path + " with error: " + insertResult.errors[0].message.value_or("Unknown error"), "INFO");
                            operationLogged = true;
                        }
                        continue;
                    }

                    // Read operation - non-blocking first
                    auto readResult = pspace.read<int>(path);
                    if (!readResult.has_value()) {
                        failedReads.fetch_add(1, std::memory_order_relaxed);
                        if (!operationLogged) {
                            sp_log("Thread " + std::to_string(threadId) + " read failed at " + path, "INFO");
                            operationLogged = true;
                        }
                        continue;
                    }

                    if (readResult.value() != i) {
                        failedReads.fetch_add(1, std::memory_order_relaxed);
                        if (!operationLogged) {
                            sp_log("Thread " + std::to_string(threadId) + " read value mismatch at " + path + ": expected " + std::to_string(i) + " got " + std::to_string(readResult.value()), "INFO");
                            operationLogged = true;
                        }
                        continue;
                    }

                    // Extract operation - non-blocking
                    auto extractResult = pspace.take<int>(path);
                    if (!extractResult.has_value()) {
                        failedExtracts.fetch_add(1, std::memory_order_relaxed);
                        if (!operationLogged) {
                            sp_log("Thread " + std::to_string(threadId) + " extract failed at " + path, "INFO");
                            operationLogged = true;
                        }
                        continue;
                    }

                    if (extractResult.value() == i) {
                        successfulOperations.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        failedExtracts.fetch_add(1, std::memory_order_relaxed);
                        if (!operationLogged) {
                            sp_log("Thread " + std::to_string(threadId) + " extract value mismatch at " + path + ": expected " + std::to_string(i) + " got " + std::to_string(extractResult.value()), "INFO");
                        }
                    }

                    // Log progress every 100 operations
                    if (i % 100 == 0) {
                        sp_log("Thread " + std::to_string(threadId) + " progress: " + std::to_string(i) + "/" + std::to_string(OPERATIONS_PER_THREAD) + " (successful: " + std::to_string(successfulOperations.load()) + ")", "INFO");
                    }
                }
            } catch (const std::exception& e) {
                sp_log("Thread " + std::to_string(threadId) + " exception: " + e.what(), "INFO");
            } catch (...) {
                sp_log("Thread " + std::to_string(threadId) + " unknown exception", "INFO");
            }

            completedThreads.fetch_add(1, std::memory_order_release);
            sp_log("Thread " + std::to_string(threadId) + " completed", "INFO");
        };

        // Monitor thread with more frequent updates
        std::thread monitorThread([&]() {
            const int TIMEOUT_SECONDS = 30; // Reduced timeout for faster feedback
            for (int i = 0; i < TIMEOUT_SECONDS && !shouldStop; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));

                // Log detailed status every second
                int completed  = completedThreads.load(std::memory_order_acquire);
                int successful = successfulOperations.load(std::memory_order_relaxed);
                int failed_i   = failedInserts.load(std::memory_order_relaxed);
                int failed_r   = failedReads.load(std::memory_order_relaxed);
                int failed_e   = failedExtracts.load(std::memory_order_relaxed);

                sp_log("Status at " + std::to_string(i) + "s:", "INFO");
                sp_log("- Completed threads: " + std::to_string(completed) + "/" + std::to_string(NUM_THREADS), "INFO");
                sp_log("- Successful ops: " + std::to_string(successful), "INFO");
                sp_log("- Failed inserts: " + std::to_string(failed_i), "INFO");
                sp_log("- Failed reads: " + std::to_string(failed_r), "INFO");
                sp_log("- Failed extracts: " + std::to_string(failed_e), "INFO");

                if (completed == NUM_THREADS) {
                    sp_log("All threads completed successfully", "INFO");
                    return;
                }
            }
            sp_log("Test timed out", "INFO");
            shouldStop.store(true, std::memory_order_release);
        });

        // Start worker threads
        std::vector<std::thread> threads;
        threads.reserve(NUM_THREADS);
        for (int i = 0; i < NUM_THREADS; ++i) {
            threads.emplace_back(workerFunction, i);
        }

        // Join threads
        for (auto& t : threads) {
            if (t.joinable())
                t.join();
        }
        if (monitorThread.joinable())
            monitorThread.join();

        // Calculate results
        int    totalOperations = NUM_THREADS * OPERATIONS_PER_THREAD;
        int    successfulOps   = successfulOperations.load(std::memory_order_relaxed);
        double successRate     = static_cast<double>(successfulOps) / totalOperations;

        // Log final statistics
        sp_log("Final Statistics:", "INFO");
        sp_log("Total operations attempted: " + std::to_string(totalOperations), "INFO");
        sp_log("Successful operations: " + std::to_string(successfulOps), "INFO");
        sp_log("Success rate: " + std::to_string(successRate), "INFO");
        sp_log("Failed inserts: " + std::to_string(failedInserts.load(std::memory_order_relaxed)), "INFO");
        sp_log("Failed reads: " + std::to_string(failedReads.load(std::memory_order_relaxed)), "INFO");
        sp_log("Failed extracts: " + std::to_string(failedExtracts.load(std::memory_order_relaxed)), "INFO");

        // Cleanup
        pspace.clear();

        // Simplified checks
        bool didTimeout = shouldStop.load(std::memory_order_acquire);
        REQUIRE_MESSAGE(completedThreads.load(std::memory_order_acquire) == NUM_THREADS, "Not all threads completed");

        if (!didTimeout) {
            CHECK_MESSAGE(successRate > 0.0, "Success rate is zero - no operations completed successfully");
        }
    }

    SUBCASE("Deadlock Detection and Prevention") {
        PathSpace        pspace;
        const int        NUM_THREADS = 10;
        std::atomic<int> deadlockCount(0);

        auto resourceA = []() -> int { return 1; };
        auto resourceB = []() -> int { return 2; };

        CHECK(pspace.insert("/resourceA", resourceA).errors.size() == 0);
        CHECK(pspace.insert("/resourceB", resourceB).errors.size() == 0);

        auto workerFunction = [&](int threadId) {
            if (threadId % 2 == 0) {
                // Even threads try to acquire A then B
                auto resultA = pspace.read<int>("/resourceA", Block{});
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                auto resultB = pspace.read<int>("/resourceB", Block(100ms));
                if (!resultB)
                    deadlockCount++;
            } else {
                // Odd threads try to acquire B then A
                auto resultB = pspace.read<int>("/resourceB", Block{});
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                auto resultA = pspace.read<int>("/resourceA", Block(100ms));
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
        PathSpace  pspace;
        const int  NUM_THREADS           = std::thread::hardware_concurrency();
        const int  OPERATIONS_PER_THREAD = 200;
        const int  NUM_PATHS             = 20;
        const auto TEST_DURATION         = std::chrono::milliseconds(150);
        const int  NUM_ITERATIONS        = 1;

        auto performanceTest = [&](int concurrency) {
            struct Result {
                double ops;
                double duration;
            };

            auto runIteration = [&]() -> Result {
                std::atomic<int>  completedOperations(0);
                std::atomic<bool> shouldStop(false);

                auto workerFunction = [&]() {
                    for (int i = 0; i < OPERATIONS_PER_THREAD && !shouldStop.load(std::memory_order_relaxed); ++i) {
                        std::string path = std::string("/perf/") + std::to_string(i % NUM_PATHS);
                        auto        task = []() -> int { return 42; };
                        pspace.insert(path, task);
                        auto result = pspace.read<int>(path, Block(10ms));
                        if (result.has_value()) {
                            completedOperations.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                };

                auto start = std::chrono::steady_clock::now();

                std::vector<std::thread> threads;
                threads.reserve(concurrency);
                for (int i = 0; i < concurrency; ++i) {
                    threads.emplace_back(workerFunction);
                }

                std::thread([&shouldStop, TEST_DURATION]() {
                    std::this_thread::sleep_for(TEST_DURATION);
                    shouldStop.store(true, std::memory_order_relaxed);
                }).detach();

                for (auto& t : threads) {
                    t.join();
                }

                auto end      = std::chrono::steady_clock::now();
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
        auto multiThreadResult  = performanceTest(NUM_THREADS);

        // Calculate operations per second
        double singleThreadedOps = singleThreadResult.ops / singleThreadResult.duration;
        double multiThreadedOps  = multiThreadResult.ops / multiThreadResult.duration;

        // Log the results
        sp_log(std::format("Single-threaded performance: {:.2f} ops/sec\n", singleThreadedOps), "INFO");
        sp_log(std::format("Multi-threaded performance: {:.2f} ops/sec\n", multiThreadedOps), "INFO");
        sp_log(std::format("Performance improvement: {:.2f}x\n", multiThreadedOps / singleThreadedOps), "INFO");

        // Check for performance improvement with a tolerance
        constexpr double IMPROVEMENT_THRESHOLD = 1.2; // Expect at least 20% improvement
        constexpr double TOLERANCE             = 0.1; // 10% tolerance

        CHECK((multiThreadedOps / singleThreadedOps) > (IMPROVEMENT_THRESHOLD - TOLERANCE));
    }

    SUBCASE("Dining Philosophers") {
        PathSpace pspace;
        const int NUM_PHILOSOPHERS     = 5;
        const int EATING_DURATION_MS   = 5;
        const int THINKING_DURATION_MS = 5;
        const int TEST_DURATION_MS     = 1500;

        struct PhilosopherStats {
            std::atomic<int> meals_eaten{0};
            std::atomic<int> times_starved{0};
            std::atomic<int> forks_acquired{0};
        };

        std::vector<PhilosopherStats> stats(NUM_PHILOSOPHERS);

        auto philosopher = [&](int id) {
            std::string first_fork  = std::string("/fork/") + std::to_string(std::min(id, (id + 1) % NUM_PHILOSOPHERS));
            std::string second_fork = std::string("/fork/") + std::to_string(std::max(id, (id + 1) % NUM_PHILOSOPHERS));

            std::mt19937                    rng(id);
            std::uniform_int_distribution<> think_dist(1, THINKING_DURATION_MS);
            std::uniform_int_distribution<> eat_dist(1, EATING_DURATION_MS);
            std::uniform_int_distribution<> backoff_dist(1, 5);

            auto start_time = std::chrono::steady_clock::now();

            while (std::chrono::steady_clock::now() - start_time < std::chrono::milliseconds(TEST_DURATION_MS)) {
                // Thinking
                std::this_thread::sleep_for(std::chrono::milliseconds(think_dist(rng)));

                // Try to pick up forks
                auto first = pspace.take<int>(first_fork, Block(50ms));
                if (first.has_value()) {
                    stats[id].forks_acquired.fetch_add(1, std::memory_order_relaxed);
                    auto second = pspace.take<int>(second_fork, Block(50ms));
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
        std::vector<std::thread> philosophers;
        for (int i = 0; i < NUM_PHILOSOPHERS; ++i) {
            philosophers.emplace_back(philosopher, i);
        }

        // Wait for test to complete
        std::this_thread::sleep_for(std::chrono::milliseconds(TEST_DURATION_MS));

        // Join all threads
        for (auto& t : philosophers) { if (t.joinable()) t.join(); }
        philosophers.clear();

        // Output and check results
        int total_meals          = 0;
        int total_starved        = 0;
        int total_forks_acquired = 0;
        for (int i = 0; i < NUM_PHILOSOPHERS; ++i) {
            int meals   = stats[i].meals_eaten.load();
            int starved = stats[i].times_starved.load();
            int forks   = stats[i].forks_acquired.load();
            total_meals += meals;
            total_starved += starved;
            total_forks_acquired += forks;
            sp_log(std::format("Philosopher {}: Meals eaten: {}, Times starved: {}, Forks acquired: {}\n", i, meals, starved, forks), "INFO");

            // Check that each philosopher ate at least once
            CHECK(meals > 0);
            // Check that each philosopher experienced some contention
            CHECK(starved > 0);
        }

        sp_log(std::format("Total meals eaten: {}\n", total_meals), "INFO");
        sp_log(std::format("Total times starved: {}\n", total_starved), "INFO");
        sp_log(std::format("Total forks acquired: {}\n", total_forks_acquired), "INFO");
        sp_log(std::format("Meals per philosopher: {:.2f}\n", static_cast<double>(total_meals) / NUM_PHILOSOPHERS), "INFO");

        // Check overall statistics
        CHECK(total_meals > NUM_PHILOSOPHERS);          // Each philosopher should eat at least once
        CHECK(total_starved > 0);                       // There should be some contention
        CHECK(total_forks_acquired >= total_meals * 2); // Each meal requires two forks

        // Check that there's no deadlock (all forks are available)
        for (int i = 0; i < NUM_PHILOSOPHERS; ++i) {
            auto fork = pspace.read<int>(std::format("/fork/{}", i), Block{});
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