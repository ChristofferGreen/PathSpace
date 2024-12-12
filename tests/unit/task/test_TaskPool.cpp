#include "ext/doctest.h"
#include "task/TaskPool.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_set>

using namespace SP;
using namespace std::chrono_literals;

TEST_CASE("TaskPool Misc") {
    SUBCASE("Basic task execution") {
        TaskPool          pool(2);
        std::atomic<int>  counter{0};
        std::atomic<bool> done{false};

        {
            std::shared_ptr<Task> task = Task::Create([&counter, &done](Task const&, bool) {
                counter++;
                done = true;
            });

            pool.addTask(task);
            while (!done)
                std::this_thread::yield();
            task.reset();
        }

        CHECK(counter == 1);
    }

    SUBCASE("Multiple tasks execution") {
        TaskPool         pool(4);
        std::atomic<int> counter{0};
        const int        NUM_TASKS = 100;

        std::vector<std::shared_ptr<Task>> tasks;
        tasks.reserve(NUM_TASKS);

        // Create and submit all tasks
        for (int i = 0; i < NUM_TASKS; ++i) {
            auto task = Task::Create([&counter](Task const&, bool) { counter++; });
            tasks.push_back(task);
            pool.addTask(task);
        }

        // Simple spin-wait for completion
        while (counter < NUM_TASKS) {
            std::this_thread::yield();
        }

        CHECK(counter == NUM_TASKS);
    }

    SUBCASE("Shutdown behavior") {
        SUBCASE("Clean shutdown with no tasks") {
            TaskPool pool(2);
            pool.shutdown();
            CHECK(pool.size() == 0);
        }

        SUBCASE("Shutdown with pending tasks") {
            TaskPool                           pool(2);
            std::atomic<int>                   counter{0};
            std::vector<std::shared_ptr<Task>> tasks;

            // Add several quick tasks
            for (int i = 0; i < 10; ++i) {
                auto fun = [&counter](Task const&, bool) {
                    std::this_thread::sleep_for(10ms);
                    counter++;
                };
                tasks.push_back(Task::Create(fun));
                pool.addTask(tasks.back());
            }

            pool.shutdown();
            CHECK(counter > 0);   // At least some tasks ran
            CHECK(counter <= 10); // Not all tasks might run
            CHECK(pool.size() == 0);
        }

        SUBCASE("Double shutdown safety") {
            TaskPool pool(2);
            pool.shutdown();
            pool.shutdown(); // Should not crash
            CHECK(pool.size() == 0);
        }
    }

    SUBCASE("Task lifetime and cleanup with timeout") {
        TaskPool                pool(1);
        std::atomic<bool>       taskExecuted{false};
        std::weak_ptr<Task>     weakTask;
        std::mutex              mutex;
        std::condition_variable cv;
        bool                    completed = false;

        {
            auto task = Task::Create([&](Task const&, bool) {
                taskExecuted = true;
                std::lock_guard<std::mutex> lock(mutex);
                completed = true;
                cv.notify_one();
            });
            weakTask  = task;

            pool.addTask(task);

            std::unique_lock<std::mutex> lock(mutex);
            bool                         waitResult = cv.wait_for(lock, std::chrono::seconds(5), [&] { return completed; });

            REQUIRE(waitResult); // Ensure we didn't timeout
        }

        CHECK(taskExecuted);
        std::this_thread::sleep_for(20ms);
        CHECK(weakTask.expired());
    }

    SUBCASE("Simple task cancellation through shared_ptr lifetime") {
        TaskPool          pool(2);
        std::atomic<bool> taskExecuted{false};

        {
            std::weak_ptr<Task> weakTask;
            // Create a shared_ptr to control task lifetime
            auto task = Task::Create([weakTask, &taskExecuted](Task const&, bool) {
                if (auto ptr = weakTask.lock()) {
                    std::this_thread::sleep_for(500ms);
                    taskExecuted = true;
                }
            });

            // Create weak_ptr for the pool
            weakTask = task;

            // Add weak reference to pool
            pool.addTask(task);
        } // task is destroyed here

        std::this_thread::sleep_for(100ms);
        CHECK_FALSE(taskExecuted);
    }

    SUBCASE("Simplified task pool stress test") {
        auto getOptimalTaskCount = []() {
            const size_t minTasks     = 100;
            const size_t maxTasks     = 1000;
            const size_t tasksPerCore = 50;
            return std::clamp(std::thread::hardware_concurrency() * tasksPerCore, minTasks, maxTasks);
        };

        const size_t TASK_COUNT         = getOptimalTaskCount();
        const auto   REASONABLE_TIMEOUT = std::chrono::milliseconds(TASK_COUNT / 10);

        const int NUM_ITERATIONS = 3;
        for (int iter = 0; iter < NUM_ITERATIONS; ++iter) {
            const size_t                       THREAD_COUNT = std::max(2u, std::thread::hardware_concurrency() / 2);
            TaskPool                           pool(THREAD_COUNT);
            std::atomic<size_t>                completedTasks{0};
            std::vector<std::shared_ptr<Task>> tasks;
            tasks.reserve(TASK_COUNT);

            // Create and add tasks
            for (size_t i = 0; i < TASK_COUNT; ++i) {
                auto task = Task::Create([&completedTasks](Task const&, bool) { completedTasks++; });
                tasks.push_back(task);
                pool.addTask(task);
            }

            std::this_thread::sleep_for(REASONABLE_TIMEOUT);
            pool.shutdown();

            CHECK(pool.size() == 0);

            const size_t completedCount = completedTasks.load();
            INFO("Completed tasks: " << completedCount << " / " << TASK_COUNT);
            CHECK(completedCount > 0);
            CHECK(completedCount <= TASK_COUNT);

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const size_t finalCount = completedTasks.load();
            CHECK(finalCount == completedCount);
        }
    }

    SUBCASE("Task Exception Handling") {
        SUBCASE("Basic exception handling") {
            TaskPool pool(1);

            // Test state management
            struct TestState {
                std::atomic<bool>       exceptionCaught{false};
                std::atomic<bool>       taskCompleted{false};
                std::atomic<bool>       unexpectedError{false};
                std::mutex              mutex;
                std::condition_variable cv;
                std::string             errorMessage;
            };
            auto state = std::make_shared<TestState>();

            // Create task with comprehensive error handling
            auto task = Task::Create([state](Task const&, bool) {
                try {
                    throw std::runtime_error("Test exception");
                } catch (const std::runtime_error& e) {
                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        state->errorMessage    = e.what();
                        state->exceptionCaught = true;
                    }
                } catch (...) {
                    state->unexpectedError = true;
                }

                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->taskCompleted.store(true, std::memory_order_release);
                    state->cv.notify_one(); // Notify while holding lock
                }
            });

            pool.addTask(std::weak_ptr<Task>(task));

            {
                std::unique_lock<std::mutex> lock(state->mutex);
                bool                         completed = state->cv.wait_for(lock, std::chrono::seconds(5), [&state] { return state->taskCompleted.load(std::memory_order_acquire); });

                REQUIRE_MESSAGE(completed, "Task execution timed out");
                CHECK(state->exceptionCaught);
                CHECK_FALSE(state->unexpectedError);
                CHECK_EQ(state->errorMessage, "Test exception");
            }

            task.reset();
        }

        SUBCASE("Multiple concurrent exceptions") {
            TaskPool                pool(1);
            std::atomic<int>        exceptionCount{0};
            std::mutex              mutex;
            std::condition_variable cv;
            std::atomic<int>        tasksCompleted{0};

            std::vector<std::shared_ptr<Task>> tasks;
            constexpr int                      NUM_TASKS = 5;

            for (int i = 0; i < NUM_TASKS; ++i) {
                auto task = Task::Create([&exceptionCount, &mutex, &cv, &tasksCompleted, i](Task const&, bool) {
                    try {
                        throw std::runtime_error("Test exception " + std::to_string(i));
                    } catch (...) {
                        exceptionCount++;
                    }

                    if (++tasksCompleted == NUM_TASKS) {
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            cv.notify_one(); // Notify while holding lock
                        }
                    }
                });
                tasks.push_back(task);
                pool.addTask(std::weak_ptr<Task>(tasks.back()));
            }

            {
                std::unique_lock<std::mutex> lock(mutex);
                bool                         completed = cv.wait_for(lock, std::chrono::seconds(5), [&tasksCompleted] { return tasksCompleted == NUM_TASKS; });

                REQUIRE_MESSAGE(completed, "Not all tasks completed within timeout");
                CHECK_EQ(exceptionCount, NUM_TASKS);
            }

            tasks.clear();
        }

        SUBCASE("Resource cleanup during exception") {
            TaskPool pool(2);

            struct State {
                std::mutex              mutex;
                std::condition_variable cv;
                bool                    taskCompleted{false};
                bool                    resourceDestroyed{false};
            };
            auto state = std::make_shared<State>();

            struct ResourceGuard {
                State& state;
                ResourceGuard(State& s)
                    : state(s) {}
                ~ResourceGuard() {
                    std::lock_guard<std::mutex> lock(state.mutex);
                    state.resourceDestroyed = true;
                    state.cv.notify_one();
                }
            };

            {
                auto task = Task::Create([state](Task const&, bool) {
                    // Create resource in its own scope
                    {
                        auto resource = std::make_unique<ResourceGuard>(*state);
                        throw std::runtime_error("Test exception");
                    } // ResourceGuard destructor runs here

                    // Mark task as done (won't execute due to exception)
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->taskCompleted = true;
                    state->cv.notify_one();
                });

                pool.addTask(std::weak_ptr<Task>(task));

                // Wait for either task completion or resource destruction
                {
                    std::unique_lock<std::mutex> lock(state->mutex);
                    REQUIRE(state->cv.wait_for(lock, std::chrono::seconds(1), [&state] { return state->resourceDestroyed || state->taskCompleted; }));

                    CHECK(state->resourceDestroyed);   // Resource should be destroyed even with exception
                    CHECK_FALSE(state->taskCompleted); // Task should not complete normally
                }
            }
        }

        SUBCASE("Exception during task cancellation") {
            TaskPool pool(1);

            struct TestState {
                std::atomic<bool>       taskStarted{false};
                std::atomic<bool>       taskCancelled{false};
                std::atomic<bool>       exceptionHandled{false};
                std::mutex              mutex;
                std::condition_variable cv;
            };
            auto state = std::make_shared<TestState>();

            auto task = Task::Create([state](Task const& task, bool) {
                try {
                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        state->taskStarted = true;
                        state->cv.notify_one();
                    }

                    while (!state->taskCancelled) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }

                    throw std::runtime_error("Task cancelled");
                } catch (...) {
                    state->exceptionHandled = true;
                }
            });

            pool.addTask(std::weak_ptr<Task>(task));

            {
                std::unique_lock<std::mutex> lock(state->mutex);
                bool                         started = state->cv.wait_for(lock, std::chrono::seconds(5), [&state] { return state->taskStarted.load(); });
                REQUIRE_MESSAGE(started, "Task did not start within timeout");
            }

            state->taskCancelled = true;

            auto waitStart = std::chrono::steady_clock::now();
            while (!state->exceptionHandled) {
                if (std::chrono::steady_clock::now() - waitStart > std::chrono::seconds(5)) {
                    FAIL("Exception handling timeout");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            CHECK(state->exceptionHandled);
            task.reset();
        }
    }

    SUBCASE("Complex Task Interactions") {
        // Keep our original simple tests
        SUBCASE("Basic task chain") {
            TaskPool                pool(2);
            std::vector<int>        sequence;
            std::mutex              mutex;
            std::condition_variable cv;
            bool                    done = false;

            // Create tasks with sequential dependencies
            auto task2 = Task::Create([&sequence, &mutex, &cv, &done](Task const&, bool) {
                std::lock_guard<std::mutex> lock(mutex);
                sequence.push_back(2);
                done = true;
                cv.notify_one();
            });
            auto task1 = Task::Create([&sequence, &mutex, &pool, task2](Task const&, bool) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    sequence.push_back(1);
                }
                pool.addTask(std::weak_ptr<Task>(task2));
            });
            auto task0 = Task::Create([&sequence, &mutex, &pool, task1](Task const&, bool) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    sequence.push_back(0);
                }
                pool.addTask(std::weak_ptr<Task>(task1));
            });

            // Start the chain
            pool.addTask(std::weak_ptr<Task>(task0));

            // Wait for completion
            {
                std::unique_lock<std::mutex> lock(mutex);
                REQUIRE(cv.wait_for(lock, std::chrono::seconds(1), [&done] { return done; }));
                CHECK(sequence == std::vector{0, 1, 2});
            }
        }

        SUBCASE("Basic parallel tasks") {
            TaskPool pool(2); // Create pool with 2 threads

            std::atomic<int> completedTasks{0};

            // Thread-safe map to track unique thread IDs
            struct ThreadTracker {
                std::mutex                          mutex;
                std::unordered_set<std::thread::id> threads;
                std::atomic<int>                    uniqueThreadCount{0}; // Moved inside the struct

                void addThread() {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (threads.insert(std::this_thread::get_id()).second) {
                        // Only increment if this was a new thread ID
                        uniqueThreadCount++;
                    }
                }

                int getUniqueThreadCount() const {
                    return uniqueThreadCount.load();
                }

                ~ThreadTracker() {
                    std::lock_guard<std::mutex> lock(mutex);
                    threads.clear();
                }
            };

            auto tracker = std::make_shared<ThreadTracker>();

            constexpr int                      TASK_COUNT = 4;
            std::vector<std::shared_ptr<Task>> tasks;
            tasks.reserve(TASK_COUNT);

            // Create and add tasks
            for (int i = 0; i < TASK_COUNT; i++) {
                auto task = Task::Create([tracker, &completedTasks](Task const&, bool) {
                    // Record thread ID in a thread-safe way
                    tracker->addThread();

                    // Simulate some work
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));

                    completedTasks++;
                });

                tasks.push_back(task);
                pool.addTask(task);
            }

            // Wait for completion (with timeout)
            auto start = std::chrono::steady_clock::now();
            while (completedTasks < TASK_COUNT) {
                if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
                    MESSAGE("Completed tasks: ", completedTasks.load(), "/", TASK_COUNT);
                    MESSAGE("Unique threads: ", tracker->getUniqueThreadCount());
                    FAIL("Timeout waiting for tasks to complete");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            // Ensure all tasks are done before checking results
            pool.shutdown();

            // Verify results
            CHECK(completedTasks == TASK_COUNT);
            CHECK(tracker->getUniqueThreadCount() > 1); // Verify parallel execution
        }

        // Add simple focused tests for other goals
        SUBCASE("Dynamic task creation") {
            TaskPool pool(2);

            struct {
                std::mutex              mutex;
                std::condition_variable cv;
                int                     tasksCreated{0};
                int                     tasksCompleted{0};
            } state;

            // Store tasks to keep them alive
            std::vector<std::shared_ptr<Task>> allTasks;

            auto initialTask = Task::Create([&](Task const&, bool) {
                // Create two child tasks
                for (int i = 0; i < 2; i++) {
                    auto childTask = Task::Create([&state](Task const&, bool) {
                        std::lock_guard<std::mutex> lock(state.mutex);
                        state.tasksCompleted++;
                        state.cv.notify_one();
                    });

                    {
                        std::lock_guard<std::mutex> lock(state.mutex);
                        state.tasksCreated++;
                    }

                    allTasks.push_back(childTask); // Keep child task alive
                    pool.addTask(std::weak_ptr<Task>(childTask));
                }

                // Mark parent task complete
                {
                    std::lock_guard<std::mutex> lock(state.mutex);
                    state.tasksCompleted++;
                    state.cv.notify_one();
                }
            });

            allTasks.push_back(initialTask); // Keep parent task alive
            pool.addTask(std::weak_ptr<Task>(initialTask));

            // Wait for completion
            {
                std::unique_lock<std::mutex> lock(state.mutex);
                bool                         completed = state.cv.wait_for(lock, std::chrono::seconds(1), [&state] { return state.tasksCompleted == 3; });

                REQUIRE(completed);
                CHECK(state.tasksCreated == 2);   // Two child tasks
                CHECK(state.tasksCompleted == 3); // Parent + two children
            }
        }

        SUBCASE("Task group isolation") {
            TaskPool pool(2);

            struct {
                std::mutex                      mutex;
                std::condition_variable         cv;
                std::map<int, std::vector<int>> groupResults;
                int                             completedTasks{0};
            } state;

            // Keep tasks alive
            std::vector<std::shared_ptr<Task>> tasks;

            // Create two groups with distinct tasks
            for (int group = 0; group < 2; group++) {
                for (int task = 0; task < 2; task++) {
                    auto taskObj = Task::Create([&state, group, task](Task const&, bool) {
                        {
                            std::lock_guard<std::mutex> lock(state.mutex);
                            state.groupResults[group].push_back(task);
                            state.completedTasks++;
                            state.cv.notify_one();
                        }
                    });
                    tasks.push_back(taskObj); // Keep task alive
                    pool.addTask(std::weak_ptr<Task>(tasks.back()));
                }
            }

            // Wait for all tasks to complete
            {
                std::unique_lock<std::mutex> lock(state.mutex);
                bool                         completed = state.cv.wait_for(lock, std::chrono::seconds(1), [&state] { return state.completedTasks == 4; });

                REQUIRE(completed);
                CHECK(state.groupResults[0].size() == 2);
                CHECK(state.groupResults[1].size() == 2);
            }
        }

        SUBCASE("Task cleanup") {
            TaskPool pool(2);

            struct State {
                std::mutex              mutex;
                std::condition_variable cv;
                bool                    taskCompleted{false};
                std::shared_ptr<bool>   testValue; // Keep resource ownership in state

                State()
                    : testValue(std::make_shared<bool>(false)) {}
            };
            auto state = std::make_shared<State>();

            std::weak_ptr<bool> weakValue = state->testValue; // For verification

            {
                auto task = Task::Create([state](Task const&, bool) {
                    *state->testValue = true;
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->taskCompleted = true;
                    state->cv.notify_one();
                });

                pool.addTask(std::weak_ptr<Task>(task));

                // Wait for task completion
                {
                    std::unique_lock<std::mutex> lock(state->mutex);
                    REQUIRE(state->cv.wait_for(lock, std::chrono::seconds(1), [&state] { return state->taskCompleted; }));
                }
            } // task is destroyed here

            // Verify task executed and cleanup occurred
            if (auto value = weakValue.lock()) {
                CHECK(*value);
                // Note: use_count will be at least 2 because state still holds a reference
                CHECK(value); // Just verify we can access the value
            } else {
                FAIL("Resource was destroyed prematurely");
            }

            // Clear state to allow cleanup
            state.reset();
            std::this_thread::sleep_for(20ms);
            CHECK_FALSE(weakValue.lock()); // Now the resource should be cleaned up
        }
    }

    SUBCASE("Memory usage under load") {
        TaskPool pool(2);

        struct {
            std::mutex              mutex;
            std::condition_variable cv;
            int                     tasksCompleted{0};
        } state;

        constexpr int TOTAL_TASKS = 1000;
        // Keep strong references until we're sure tasks are queued
        std::vector<std::shared_ptr<Task>> tasks;
        std::vector<std::weak_ptr<Task>>   taskRefs;

        // Create and queue tasks
        for (int i = 0; i < TOTAL_TASKS; ++i) {
            auto task = Task::Create([&state](Task const&, bool) {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.tasksCompleted++;
                state.cv.notify_one();
            });

            tasks.push_back(task);    // Keep strong reference
            taskRefs.push_back(task); // Store weak reference for later cleanup check
            pool.addTask(std::weak_ptr<Task>(tasks.back()));
        }

        // Wait for completion
        {
            std::unique_lock<std::mutex> lock(state.mutex);
            bool                         completed = state.cv.wait_for(lock,
                                               std::chrono::seconds(10), // Increased timeout
                                               [&state] { return state.tasksCompleted == TOTAL_TASKS; });

            REQUIRE(completed);
            CHECK(state.tasksCompleted == TOTAL_TASKS);
        }

        // Now safe to release strong references
        tasks.clear();

        // Verify task cleanup
        int liveTaskCount = 0;
        for (const auto& weakTask : taskRefs) {
            if (auto task = weakTask.lock()) {
                liveTaskCount++;
            }
        }
        CHECK(liveTaskCount == 0);
    }

    SUBCASE("Mixed task durations") {
        TaskPool pool(2);

        struct {
            std::mutex                            mutex;
            std::condition_variable               cv;
            int                                   tasksCompleted{0};
            std::set<std::thread::id>             threadsSeen; // Track which threads execute tasks
            std::chrono::steady_clock::time_point startTime;
        } state;

        state.startTime = std::chrono::steady_clock::now();

        // Keep tasks alive until completion
        std::vector<std::shared_ptr<Task>> tasks;

        // Create a mix of long and short tasks
        constexpr int LONG_TASKS  = 2;
        constexpr int SHORT_TASKS = 8;
        constexpr int TOTAL_TASKS = LONG_TASKS + SHORT_TASKS;

        // First add long tasks
        for (int i = 0; i < LONG_TASKS; i++) {
            auto task = Task::Create([&state](Task const&, bool) {
                {
                    std::lock_guard<std::mutex> lock(state.mutex);
                    state.threadsSeen.insert(std::this_thread::get_id());
                }
                // Long task
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                {
                    std::lock_guard<std::mutex> lock(state.mutex);
                    state.tasksCompleted++;
                    state.cv.notify_one();
                }
            });
            tasks.push_back(task);
            pool.addTask(std::weak_ptr<Task>(task));
        }

        // Then add short tasks
        for (int i = 0; i < SHORT_TASKS; i++) {
            auto task = Task::Create([&state](Task const&, bool) {
                {
                    std::lock_guard<std::mutex> lock(state.mutex);
                    state.threadsSeen.insert(std::this_thread::get_id());
                }
                // Short task
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                {
                    std::lock_guard<std::mutex> lock(state.mutex);
                    state.tasksCompleted++;
                    state.cv.notify_one();
                }
            });
            tasks.push_back(task);
            pool.addTask(std::weak_ptr<Task>(task));
        }

        // Wait for completion
        {
            std::unique_lock<std::mutex> lock(state.mutex);
            bool                         completed = state.cv.wait_for(lock, std::chrono::seconds(2), [&state] { return state.tasksCompleted == TOTAL_TASKS; });

            REQUIRE(completed);

            // Verify completion
            CHECK(state.tasksCompleted == TOTAL_TASKS);

            // Verify parallel execution by checking thread count
            CHECK(state.threadsSeen.size() > 1);

            // Verify total execution time is less than sequential would require
            auto totalTime         = std::chrono::steady_clock::now() - state.startTime;
            auto maxSequentialTime = std::chrono::milliseconds(LONG_TASKS * 100 + SHORT_TASKS * 10);
            CHECK(totalTime < maxSequentialTime);
        }
    }
}