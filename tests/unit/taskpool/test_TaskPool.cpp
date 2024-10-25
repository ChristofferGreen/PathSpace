#include "ext/doctest.h"
#include "path/ConcretePath.hpp"
#include "pathspace/PathSpace.hpp"
#include "pathspace/taskpool/TaskPool.hpp"

using namespace SP;

#include <atomic>
#include <chrono>
#include <future>
#include <random>
#include <thread>

using namespace SP;
using namespace std::chrono_literals;

class TestSync {
public:
    void wait() {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this] { return completed; });
    }

    bool waitFor(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mtx);
        return cv.wait_for(lock, timeout, [this] { return completed; });
    }

    void notify() {
        std::lock_guard<std::mutex> lock(mtx);
        completed = true;
        cv.notify_all();
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mtx);
        completed = false;
    }

private:
    std::mutex mtx;
    std::condition_variable cv;
    bool completed = false;
};

// Task generation helper
class TaskGenerator {
public:
    static auto createVariableDurationTask(TestSync& sync, std::atomic<int>& counter) {
        auto task = std::make_shared<Task>();
        task->function = [&sync, &counter](Task const&, void*, bool) {
            // Create local random number generator
            thread_local std::random_device rd;
            thread_local std::mt19937 gen(rd());
            thread_local std::uniform_int_distribution<> workDist(1, 100);

            std::this_thread::sleep_for(std::chrono::microseconds(workDist(gen)));
            counter++;
            sync.notify();
        };
        return task;
    }
};

TEST_CASE("Task TaskPool Suite") {
    SUBCASE("Basic task execution") {
        TaskPool pool(2);
        std::atomic<int> counter{0};
        TestSync sync;

        auto task = std::make_shared<Task>();
        task->function = [&counter, &sync](Task const&, void*, bool) {
            counter++;
            sync.notify();
        };

        pool.addTask(task);
        sync.wait();
        CHECK(counter == 1);
    }

    SUBCASE("Multiple tasks execution") {
        TaskPool pool(4);
        std::atomic<int> counter{0};
        TestSync sync;

        const int NUM_TASKS = 100;
        for (int i = 0; i < NUM_TASKS; ++i) {
            auto task = std::make_shared<Task>();
            task->function = [&counter, &sync](Task const&, void*, bool) {
                counter++;
                sync.notify();
            };
            pool.addTask(task);
            sync.wait();
            sync.reset();
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
            TaskPool pool(2);
            std::atomic<int> counter{0};

            // Add some long-running tasks
            for (int i = 0; i < 10; ++i) {
                auto task = std::make_shared<Task>();
                task->function = [&counter](Task const&, void*, bool) {
                    std::this_thread::sleep_for(100ms);
                    counter++;
                };
                pool.addTask(task);
            }

            // Immediate shutdown
            pool.shutdown();

            // Check that not all tasks completed
            CHECK(counter < 10);
            CHECK(pool.size() == 0);
        }

        SUBCASE("Double shutdown safety") {
            TaskPool pool(2);
            pool.shutdown();
            // Should not crash or throw
            pool.shutdown();
            CHECK(pool.size() == 0);
        }
    }

    SUBCASE("Task lifetime and weak_ptr behavior") {
        TaskPool pool(2);
        std::atomic<bool> taskExecuted{false};
        std::weak_ptr<Task> weakTask;
        TestSync sync;

        // Create and queue the task while ensuring it lives long enough to be queued
        {
            auto task = std::make_shared<Task>();
            weakTask = task;
            task->function = [&taskExecuted, &sync](Task const&, void*, bool) {
                taskExecuted = true;
                sync.notify();
            };

            pool.addTask(task);
            // Give time for the task to be queued before destroying the shared_ptr
            std::this_thread::sleep_for(10ms);
        }

        // Wait for completion or timeout
        auto start = std::chrono::steady_clock::now();
        while (!taskExecuted && std::chrono::steady_clock::now() - start < std::chrono::seconds(1)) {
            if (sync.waitFor(std::chrono::milliseconds(100))) {
                break;
            }
        }

        CHECK(taskExecuted);
        CHECK(weakTask.expired());
    }

    SUBCASE("Task cancellation through task destruction") {
        TaskPool pool(2);
        std::atomic<bool> taskExecuted{false};

        auto task = std::make_shared<Task>();
        task->function = [&taskExecuted](Task const&, void*, bool) {
            std::this_thread::sleep_for(500ms);
            taskExecuted = true;
        };

        pool.addTask(task);
        task.reset(); // Destroy the task before it executes

        std::this_thread::sleep_for(100ms);
        CHECK_FALSE(taskExecuted); // Task should not execute as shared_ptr was destroyed
    }

    SUBCASE("Stress test with rapid task addition and shutdown") {
        const int NUM_ITERATIONS = 10;
        for (int i = 0; i < NUM_ITERATIONS; ++i) {
            TaskPool pool(4);
            std::atomic<int> counter{0};

            // Rapidly add tasks while shutting down
            std::thread task_adder([&]() {
                for (int j = 0; j < 1000; ++j) {
                    auto task = std::make_shared<Task>();
                    task->function = [&counter](Task const&, void*, bool) { counter++; };
                    pool.addTask(task);
                }
            });

            std::this_thread::sleep_for(1ms);
            pool.shutdown();

            if (task_adder.joinable()) {
                task_adder.join();
            }

            CHECK(pool.size() == 0);
        }
    }

    SUBCASE("Task exception handling") {
        TaskPool pool(2);
        std::atomic<bool> exceptionCaught{false};
        TestSync sync;

        auto task = std::make_shared<Task>();
        task->function = [&exceptionCaught, &sync](Task const&, void*, bool) {
            try {
                throw std::runtime_error("Test exception");
            } catch (...) {
                exceptionCaught = true;
            }
            sync.notify();
        };

        pool.addTask(task);
        sync.wait();

        CHECK(exceptionCaught);
    }

    SUBCASE("Complex task interactions") {
        SUBCASE("Task chain execution") {
            TaskPool pool(2);
            std::atomic<int> counter{0};
            TestSync sync;
            const int CHAIN_LENGTH = 5;

            // Create all tasks first
            std::vector<std::shared_ptr<Task>> tasks;
            tasks.reserve(CHAIN_LENGTH);

            // First create all the task objects
            for (int i = 0; i < CHAIN_LENGTH; ++i) {
                tasks.push_back(std::make_shared<Task>());
            }

            // Then set up their functions with proper dependencies
            for (int i = 0; i < CHAIN_LENGTH; ++i) {
                tasks[i]->function = [&counter, &sync, i, last = (i == CHAIN_LENGTH - 1)](Task const&, void*, bool) {
                    counter++;
                    if (last) {
                        sync.notify();
                    }
                };
            }

            // Submit all tasks in order
            for (auto& task : tasks) {
                pool.addTask(task);
            }

            // Wait for completion
            sync.wait();
            CHECK(counter == CHAIN_LENGTH);
        }

        SUBCASE("Parallel task groups") {
            TaskPool pool(4);
            std::atomic<int> counter{0};
            TestSync sync;
            const int GROUPS = 3;
            const int TASKS_PER_GROUP = 10;
            const int TOTAL_TASKS = GROUPS * TASKS_PER_GROUP;
            std::atomic<int> completions{0};

            // Create all tasks first
            std::vector<std::shared_ptr<Task>> tasks;
            tasks.reserve(TOTAL_TASKS);

            for (int g = 0; g < GROUPS; ++g) {
                for (int t = 0; t < TASKS_PER_GROUP; ++t) {
                    auto task = std::make_shared<Task>();
                    task->function = [&counter, &completions, &sync, TOTAL_TASKS](Task const&, void*, bool) {
                        // Simulate variable work
                        thread_local std::random_device rd;
                        thread_local std::mt19937 gen(rd());
                        thread_local std::uniform_int_distribution<> workDist(1, 100);

                        std::this_thread::sleep_for(std::chrono::microseconds(workDist(gen)));
                        counter++;

                        // If this was the last task to complete, notify
                        if (completions.fetch_add(1) + 1 == TOTAL_TASKS) {
                            sync.notify();
                        }
                    };
                    tasks.push_back(task);
                }
            }

            // Submit all tasks
            for (auto& task : tasks) {
                pool.addTask(task);
            }

            // Wait for all tasks to complete
            sync.wait();

            CHECK(counter == TOTAL_TASKS);
            CHECK(completions == TOTAL_TASKS);
        }
    }

    SUBCASE("Memory usage under load") {
        const int THREAD_COUNT = 4;
        const int TASKS_PER_BATCH = 100;
        const int NUM_BATCHES = 50;
        const int TOTAL_TASKS = TASKS_PER_BATCH * NUM_BATCHES;

        TaskPool pool(THREAD_COUNT);
        std::atomic<int> queued{0};
        std::atomic<int> started{0};
        std::atomic<int> completed{0};
        TestSync completion_sync;

        sp_log("Starting memory usage test", "TEST", "SETUP");

        // Store task objects to keep them alive
        std::vector<std::shared_ptr<Task>> tasks;
        tasks.reserve(TOTAL_TASKS);

        // Create and queue tasks
        for (int batch = 0; batch < NUM_BATCHES; ++batch) {
            for (int i = 0; i < TASKS_PER_BATCH; ++i) {
                auto task = std::make_shared<Task>();
                task->function = [&started, &completed, &completion_sync, TOTAL_TASKS](Task const&, void*, bool) {
                    started++;
                    if (completed.fetch_add(1) + 1 == TOTAL_TASKS) {
                        sp_log("All tasks completed, notifying", "TEST", "COMPLETION");
                        completion_sync.notify();
                    }
                };
                tasks.push_back(task);
                pool.addTask(task);
                queued++;
            }
            sp_log("Queued batch " + std::to_string(batch + 1) + "/" + std::to_string(NUM_BATCHES), "TEST", "PROGRESS");
        }

        sp_log("Queued " + std::to_string(queued.load()) + " tasks", "TEST", "PROGRESS");

        // Wait with timeout
        auto start = std::chrono::steady_clock::now();
        bool completed_successfully = false;
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
            sp_log("Status - Queued: " + std::to_string(queued.load()) + ", Started: " + std::to_string(started.load())
                           + ", Completed: " + std::to_string(completed.load()),
                   "TEST",
                   "STATUS");

            if (completed.load() == TOTAL_TASKS) {
                completed_successfully = true;
                break;
            }
            std::this_thread::sleep_for(100ms);
        }

        sp_log("Final Status - Queued: " + std::to_string(queued.load()) + ", Started: " + std::to_string(started.load())
                       + ", Completed: " + std::to_string(completed.load()),
               "TEST",
               "FINAL");

        CHECK(completed_successfully);
        CHECK(completed == TOTAL_TASKS);

        tasks.clear();
        sp_log("Test cleanup complete", "TEST", "CLEANUP");
    }

    SUBCASE("Mixed task durations") {
        TaskPool pool(4);
        std::atomic<int> counter{0};
        TestSync sync;
        const int TOTAL_TASKS = 100;

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> durationDist(1, 100);

        for (int i = 0; i < TOTAL_TASKS; ++i) {
            auto task = std::make_shared<Task>();
            task->function = [&counter, &sync, duration = durationDist(gen)](Task const&, void*, bool) {
                std::this_thread::sleep_for(std::chrono::milliseconds(duration));
                counter++;
                sync.notify();
            };
            pool.addTask(task);
            sync.wait();
            sync.reset();
        }

        CHECK(counter == TOTAL_TASKS);
    }
}