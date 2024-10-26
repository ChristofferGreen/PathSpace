#include "ext/doctest.h"
#include "path/ConcretePath.hpp"
#include "pathspace/PathSpace.hpp"
#include "pathspace/taskpool/TaskPool.hpp"

#ifdef __GNUG__
#include <valgrind/helgrind.h>
#else
// Define dummy macros that take the same parameters as the real ones
#define VALGRIND_HG_DISABLE_CHECKING(_qzz_start, _qzz_len) ((void)0)
#define VALGRIND_HG_ENABLE_CHECKING(_qzz_start, _qzz_len) ((void)0)
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <random> // Add this for random number generation
#include <thread>

using namespace SP;
using namespace std::chrono_literals;

// Reusable synchronization primitive for tests
class TestSync {
public:
    void wait() {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this] { return completed; });
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

TEST_CASE("Task TaskPool Suite") {
    static int dummy;
    VALGRIND_HG_DISABLE_CHECKING(&dummy, sizeof(dummy));
    SUBCASE("Basic task execution") {
        TaskPool pool(2);
        std::atomic<int> counter{0};
        std::atomic<bool> done{false};

        {
            std::shared_ptr<Task> task = std::make_shared<Task>();
            task->function = [&counter, &done](Task const&, void*, bool) {
                counter++;
                done = true;
            };

            pool.addTask(task);
            while (!done)
                std::this_thread::yield();
            task.reset();
        }

        CHECK(counter == 1);
    }
    VALGRIND_HG_ENABLE_CHECKING(&dummy, sizeof(dummy));

    SUBCASE("Multiple tasks execution") {
        // Manager focused on handling multiple sequential tasks
        class TaskLifetimeManager {
            struct Impl {
                std::shared_ptr<Task> task;
                std::mutex mutex;
                std::condition_variable cv;
                bool completed = false;
            };
            std::shared_ptr<Impl> impl = std::make_shared<Impl>();

        public:
            void createTask(std::function<void(Task const&, void*, bool)> fn) {
                std::lock_guard<std::mutex> lock(impl->mutex);
                impl->completed = false; // Reset for new task
                impl->task = std::make_shared<Task>();
                auto weakImpl = std::weak_ptr<Impl>(impl);

                impl->task->function = [weakImpl, fn = std::move(fn)](Task const& t, void* v, bool b) {
                    if (auto implPtr = weakImpl.lock()) {
                        fn(t, v, b);
                        std::lock_guard<std::mutex> lock(implPtr->mutex);
                        implPtr->completed = true;
                        implPtr->cv.notify_one();
                    }
                };
            }

            void waitForCompletion() {
                std::unique_lock<std::mutex> lock(impl->mutex);
                impl->cv.wait(lock, [this] { return impl->completed; });
                impl->task.reset();
            }

            std::weak_ptr<Task> getWeakPtr() const {
                std::lock_guard<std::mutex> lock(impl->mutex);
                return impl->task;
            }
        };

        TaskPool pool(4);
        std::atomic<int> counter{0};
        TestSync sync;

        const int NUM_TASKS = 100;
        for (int i = 0; i < NUM_TASKS; ++i) {
            TaskLifetimeManager manager;
            manager.createTask([&counter, &sync](Task const&, void*, bool) {
                counter++;
                sync.notify();
            });
            pool.addTask(manager.getWeakPtr());
            sync.wait();
            manager.waitForCompletion();
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
            // Manager focused on task cancellation during shutdown
            class TaskLifetimeManager {
                struct Impl {
                    std::shared_ptr<Task> task;
                    std::mutex mutex;
                };
                std::shared_ptr<Impl> impl = std::make_shared<Impl>();

            public:
                void createTask(std::function<void(Task const&, void*, bool)> fn) {
                    std::lock_guard<std::mutex> lock(impl->mutex);
                    impl->task = std::make_shared<Task>();
                    auto weakImpl = std::weak_ptr<Impl>(impl);

                    impl->task->function = [weakImpl, fn = std::move(fn)](Task const& t, void* v, bool b) {
                        if (auto implPtr = weakImpl.lock()) {
                            fn(t, v, b);
                        }
                    };
                }

                std::weak_ptr<Task> getWeakPtr() const {
                    std::lock_guard<std::mutex> lock(impl->mutex);
                    return impl->task;
                }
            };

            TaskPool pool(2);
            std::atomic<int> counter{0};
            std::vector<TaskLifetimeManager> managers(10);

            // Add some long-running tasks
            for (int i = 0; i < 10; ++i) {
                managers[i].createTask([&counter](Task const&, void*, bool) {
                    std::this_thread::sleep_for(100ms);
                    counter++;
                });
                pool.addTask(managers[i].getWeakPtr());
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

    SUBCASE("Task lifetime and weak_ptr behavior") {
        // Manager focused on weak_ptr lifetime verification
        class TaskLifetimeManager {
            struct Impl {
                std::shared_ptr<Task> task;
                std::mutex mutex;
                std::condition_variable cv;
                bool completed = false;
            };
            std::shared_ptr<Impl> impl = std::make_shared<Impl>();

        public:
            void createTask(std::function<void(Task const&, void*, bool)> fn) {
                std::lock_guard<std::mutex> lock(impl->mutex);
                impl->task = std::make_shared<Task>();
                auto weakImpl = std::weak_ptr<Impl>(impl);

                impl->task->function = [weakImpl, fn = std::move(fn)](Task const& t, void* v, bool b) {
                    if (auto implPtr = weakImpl.lock()) {
                        fn(t, v, b);
                        std::lock_guard<std::mutex> lock(implPtr->mutex);
                        implPtr->completed = true;
                        implPtr->cv.notify_one();
                    }
                };
            }

            void waitForCompletion() {
                std::unique_lock<std::mutex> lock(impl->mutex);
                impl->cv.wait(lock, [this] { return impl->completed; });
                impl->task.reset();
            }

            std::weak_ptr<Task> getWeakPtr() const {
                std::lock_guard<std::mutex> lock(impl->mutex);
                return impl->task;
            }
        };

        TaskPool pool(2);
        std::atomic<bool> taskExecuted{false};
        std::weak_ptr<Task> weakTask;
        TestSync sync;

        {
            TaskLifetimeManager manager;
            manager.createTask([&taskExecuted, &sync](Task const&, void*, bool) {
                taskExecuted = true;
                sync.notify();
            });

            weakTask = manager.getWeakPtr();
            pool.addTask(manager.getWeakPtr());
            sync.wait();
            manager.waitForCompletion();
        }

        CHECK(taskExecuted);
        CHECK(weakTask.expired());
    }

    SUBCASE("Task cancellation through task destruction") {
        // Manager focused on task cancellation behavior
        class TaskLifetimeManager {
            struct Impl {
                std::shared_ptr<Task> task;
                std::mutex mutex;
            };
            std::shared_ptr<Impl> impl = std::make_shared<Impl>();

        public:
            void createTask(std::function<void(Task const&, void*, bool)> fn) {
                std::lock_guard<std::mutex> lock(impl->mutex);
                impl->task = std::make_shared<Task>();
                auto weakImpl = std::weak_ptr<Impl>(impl);

                impl->task->function = [weakImpl, fn = std::move(fn)](Task const& t, void* v, bool b) {
                    if (auto implPtr = weakImpl.lock()) {
                        fn(t, v, b);
                    }
                };
            }

            std::weak_ptr<Task> getWeakPtr() const {
                std::lock_guard<std::mutex> lock(impl->mutex);
                return impl->task;
            }
        };

        TaskPool pool(2);
        std::atomic<bool> taskExecuted{false};

        {
            TaskLifetimeManager manager;
            manager.createTask([&taskExecuted](Task const&, void*, bool) {
                std::this_thread::sleep_for(500ms);
                taskExecuted = true;
            });
            pool.addTask(manager.getWeakPtr());
        } // Task destroyed immediately

        std::this_thread::sleep_for(100ms);
        CHECK_FALSE(taskExecuted);
    }

    SUBCASE("Stress test with rapid task addition and shutdown") {
        // Manager focused on rapid task creation/destruction
        class TaskLifetimeManager {
            struct Impl {
                std::shared_ptr<Task> task;
                std::mutex mutex;
            };
            std::shared_ptr<Impl> impl = std::make_shared<Impl>();

        public:
            void createTask(std::function<void(Task const&, void*, bool)> fn) {
                std::lock_guard<std::mutex> lock(impl->mutex);
                impl->task = std::make_shared<Task>();
                auto weakImpl = std::weak_ptr<Impl>(impl);

                impl->task->function = [weakImpl, fn = std::move(fn)](Task const& t, void* v, bool b) {
                    if (auto implPtr = weakImpl.lock()) {
                        fn(t, v, b);
                    }
                };
            }

            std::weak_ptr<Task> getWeakPtr() const {
                std::lock_guard<std::mutex> lock(impl->mutex);
                return impl->task;
            }
        };

        const int NUM_ITERATIONS = 10;
        for (int i = 0; i < NUM_ITERATIONS; ++i) {
            TaskPool pool(4);
            std::atomic<int> counter{0};
            std::vector<TaskLifetimeManager> managers;
            managers.reserve(1000);

            // Rapidly add tasks while shutting down
            std::thread task_adder([&]() {
                for (int j = 0; j < 1000; ++j) {
                    managers.emplace_back();
                    managers.back().createTask([&counter](Task const&, void*, bool) { counter++; });
                    pool.addTask(managers.back().getWeakPtr());
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
        // Manager focused on exception handling
        class TaskLifetimeManager {
            struct Impl {
                std::shared_ptr<Task> task;
                std::mutex mutex;
                std::condition_variable cv;
                bool completed = false;
            };
            std::shared_ptr<Impl> impl = std::make_shared<Impl>();

        public:
            void createTask(std::function<void(Task const&, void*, bool)> fn) {
                std::lock_guard<std::mutex> lock(impl->mutex);
                impl->task = std::make_shared<Task>();
                auto weakImpl = std::weak_ptr<Impl>(impl);

                impl->task->function = [weakImpl, fn = std::move(fn)](Task const& t, void* v, bool b) {
                    if (auto implPtr = weakImpl.lock()) {
                        try {
                            fn(t, v, b);
                        } catch (...) {
                            std::lock_guard<std::mutex> lock(implPtr->mutex);
                            implPtr->completed = true;
                            implPtr->cv.notify_one();
                            throw;
                        }
                        std::lock_guard<std::mutex> lock(implPtr->mutex);
                        implPtr->completed = true;
                        implPtr->cv.notify_one();
                    }
                };
            }

            void waitForCompletion() {
                std::unique_lock<std::mutex> lock(impl->mutex);
                impl->cv.wait(lock, [this] { return impl->completed; });
                impl->task.reset();
            }

            std::weak_ptr<Task> getWeakPtr() const {
                std::lock_guard<std::mutex> lock(impl->mutex);
                return impl->task;
            }
        };

        TaskPool pool(2);
        std::atomic<bool> exceptionCaught{false};
        TestSync sync;

        {
            TaskLifetimeManager manager;
            manager.createTask([&exceptionCaught, &sync](Task const&, void*, bool) {
                try {
                    throw std::runtime_error("Test exception");
                } catch (...) {
                    exceptionCaught = true;
                }
                sync.notify();
            });

            pool.addTask(manager.getWeakPtr());
            sync.wait();
            manager.waitForCompletion();
        }

        CHECK(exceptionCaught);
    }

    SUBCASE("Complex task interactions") {
        SUBCASE("Task chain execution") {
            // Manager focused on clean task completion signaling
            class TaskLifetimeManager {
                struct Impl {
                    std::shared_ptr<Task> task;
                    std::mutex mutex;
                    std::condition_variable cv;
                    bool completed = false;
                };
                std::shared_ptr<Impl> impl = std::make_shared<Impl>();

            public:
                void createTask(std::function<void(Task const&, void*, bool)> fn) {
                    std::lock_guard<std::mutex> lock(impl->mutex);
                    impl->completed = false;
                    impl->task = std::make_shared<Task>();
                    auto weakImpl = std::weak_ptr<Impl>(impl);

                    impl->task->function = [weakImpl, fn = std::move(fn)](Task const& t, void* v, bool b) {
                        if (auto implPtr = weakImpl.lock()) {
                            fn(t, v, b);
                            std::lock_guard<std::mutex> lock(implPtr->mutex);
                            implPtr->completed = true;
                            implPtr->cv.notify_one();
                        }
                    };
                }

                void waitForCompletion() {
                    std::unique_lock<std::mutex> lock(impl->mutex);
                    impl->cv.wait(lock, [this] { return impl->completed; });
                    impl->task.reset();
                }

                std::weak_ptr<Task> getWeakPtr() const {
                    std::lock_guard<std::mutex> lock(impl->mutex);
                    return impl->task;
                }
            };

            TaskPool pool(2);
            std::atomic<int> counter{0};
            TestSync sync;
            const int CHAIN_LENGTH = 5;

            std::vector<TaskLifetimeManager> managers(CHAIN_LENGTH);

            // Set up task chain
            for (int i = 0; i < CHAIN_LENGTH; ++i) {
                managers[i].createTask([&counter, &sync, i, last = (i == CHAIN_LENGTH - 1)](Task const&, void*, bool) {
                    counter++;
                    if (last) {
                        sync.notify();
                    }
                });
            }

            // Submit all tasks in order
            for (auto& manager : managers) {
                pool.addTask(manager.getWeakPtr());
            }

            sync.wait();
            for (auto& manager : managers) {
                manager.waitForCompletion();
            }

            CHECK(counter == CHAIN_LENGTH);
        }

        SUBCASE("Parallel task groups") {
            // Manager focused on parallel task execution
            class TaskLifetimeManager {
                struct Impl {
                    std::shared_ptr<Task> task;
                    std::mutex mutex;
                    std::condition_variable cv;
                    bool completed = false;
                };
                std::shared_ptr<Impl> impl = std::make_shared<Impl>();

            public:
                void createTask(std::function<void(Task const&, void*, bool)> fn) {
                    std::lock_guard<std::mutex> lock(impl->mutex);
                    impl->task = std::make_shared<Task>();
                    auto weakImpl = std::weak_ptr<Impl>(impl);

                    impl->task->function = [weakImpl, fn = std::move(fn)](Task const& t, void* v, bool b) {
                        if (auto implPtr = weakImpl.lock()) {
                            fn(t, v, b);
                            std::lock_guard<std::mutex> lock(implPtr->mutex);
                            implPtr->completed = true;
                            implPtr->cv.notify_one();
                        }
                    };
                }

                void waitForCompletion() {
                    std::unique_lock<std::mutex> lock(impl->mutex);
                    impl->cv.wait(lock, [this] { return impl->completed; });
                    impl->task.reset();
                }

                std::weak_ptr<Task> getWeakPtr() const {
                    std::lock_guard<std::mutex> lock(impl->mutex);
                    return impl->task;
                }
            };

            TaskPool pool(4);
            std::atomic<int> counter{0};
            TestSync sync;
            const int GROUPS = 3;
            const int TASKS_PER_GROUP = 10;
            const int TOTAL_TASKS = GROUPS * TASKS_PER_GROUP;
            std::atomic<int> completions{0};

            std::vector<TaskLifetimeManager> managers(TOTAL_TASKS);

            // Create all tasks with random work
            for (int i = 0; i < TOTAL_TASKS; ++i) {
                managers[i].createTask([&counter, &completions, &sync, TOTAL_TASKS](Task const&, void*, bool) {
                    thread_local std::random_device rd;
                    thread_local std::mt19937 gen(rd());
                    thread_local std::uniform_int_distribution<> workDist(1, 100);

                    std::this_thread::sleep_for(std::chrono::microseconds(workDist(gen)));
                    counter++;

                    if (completions.fetch_add(1) + 1 == TOTAL_TASKS) {
                        sync.notify();
                    }
                });
            }

            // Submit all tasks
            for (auto& manager : managers) {
                pool.addTask(manager.getWeakPtr());
            }

            sync.wait();
            for (auto& manager : managers) {
                manager.waitForCompletion();
            }

            CHECK(counter == TOTAL_TASKS);
            CHECK(completions == TOTAL_TASKS);
        }
    }

    SUBCASE("Memory usage under load") {
        // Manager focused on memory stability
        class TaskLifetimeManager {
            struct Impl {
                std::shared_ptr<Task> task;
                std::mutex mutex;
                std::condition_variable cv;
                bool completed = false;
            };
            std::shared_ptr<Impl> impl = std::make_shared<Impl>();

        public:
            void createTask(std::function<void(Task const&, void*, bool)> fn) {
                std::lock_guard<std::mutex> lock(impl->mutex);
                impl->task = std::make_shared<Task>();
                auto weakImpl = std::weak_ptr<Impl>(impl);

                impl->task->function = [weakImpl, fn = std::move(fn)](Task const& t, void* v, bool b) {
                    if (auto implPtr = weakImpl.lock()) {
                        fn(t, v, b);
                        std::lock_guard<std::mutex> lock(implPtr->mutex);
                        implPtr->completed = true;
                        implPtr->cv.notify_one();
                    }
                };
            }

            void waitForCompletion() {
                std::unique_lock<std::mutex> lock(impl->mutex);
                impl->cv.wait(lock, [this] { return impl->completed; });
                impl->task.reset();
            }

            std::weak_ptr<Task> getWeakPtr() const {
                std::lock_guard<std::mutex> lock(impl->mutex);
                return impl->task;
            }
        };

        const int THREAD_COUNT = 4;
        const int TASKS_PER_BATCH = 100;
        const int NUM_BATCHES = 50;
        const int TOTAL_TASKS = TASKS_PER_BATCH * NUM_BATCHES;

        TaskPool pool(THREAD_COUNT);
        std::atomic<int> queued{0};
        std::atomic<int> started{0};
        std::atomic<int> completed{0};
        TestSync completion_sync;

        std::vector<TaskLifetimeManager> managers(TOTAL_TASKS);

        // Create and queue tasks
        for (int batch = 0; batch < NUM_BATCHES; ++batch) {
            for (int i = 0; i < TASKS_PER_BATCH; ++i) {
                const int taskIdx = batch * TASKS_PER_BATCH + i;
                managers[taskIdx].createTask([&started, &completed, &completion_sync, TOTAL_TASKS](Task const&, void*, bool) {
                    started++;
                    if (completed.fetch_add(1) + 1 == TOTAL_TASKS) {
                        completion_sync.notify();
                    }
                });
                pool.addTask(managers[taskIdx].getWeakPtr());
                queued++;
            }
        }

        completion_sync.wait();
        for (auto& manager : managers) {
            manager.waitForCompletion();
        }

        CHECK(completed == TOTAL_TASKS);
    }

    SUBCASE("Mixed task durations") {
        // Manager focused on handling variable duration tasks
        class TaskLifetimeManager {
            struct Impl {
                std::shared_ptr<Task> task;
                std::mutex mutex;
                std::condition_variable cv;
                bool completed = false;
            };
            std::shared_ptr<Impl> impl = std::make_shared<Impl>();

        public:
            void createTask(std::function<void(Task const&, void*, bool)> fn) {
                std::lock_guard<std::mutex> lock(impl->mutex);
                impl->completed = false;
                impl->task = std::make_shared<Task>();
                auto weakImpl = std::weak_ptr<Impl>(impl);

                impl->task->function = [weakImpl, fn = std::move(fn)](Task const& t, void* v, bool b) {
                    if (auto implPtr = weakImpl.lock()) {
                        fn(t, v, b);
                        std::lock_guard<std::mutex> lock(implPtr->mutex);
                        implPtr->completed = true;
                        implPtr->cv.notify_one();
                    }
                };
            }

            void waitForCompletion() {
                std::unique_lock<std::mutex> lock(impl->mutex);
                impl->cv.wait(lock, [this] { return impl->completed; });
                impl->task.reset();
            }

            std::weak_ptr<Task> getWeakPtr() const {
                std::lock_guard<std::mutex> lock(impl->mutex);
                return impl->task;
            }
        };

        TaskPool pool(4);
        std::atomic<int> counter{0};
        TestSync sync;
        const int TOTAL_TASKS = 100;

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> durationDist(1, 100);

        for (int i = 0; i < TOTAL_TASKS; ++i) {
            TaskLifetimeManager manager;
            manager.createTask([&counter, &sync, duration = durationDist(gen)](Task const&, void*, bool) {
                std::this_thread::sleep_for(std::chrono::milliseconds(duration));
                counter++;
                sync.notify();
            });

            pool.addTask(manager.getWeakPtr());
            sync.wait();
            manager.waitForCompletion();
            sync.reset();
        }

        CHECK(counter == TOTAL_TASKS);
    }
}