#include "core/WaitMap.hpp"

#include "third_party/doctest.h"

#include <atomic>
#include <chrono>
#include <random>
#include <thread>

using namespace SP;
using namespace std::chrono_literals;

TEST_SUITE("WaitMap") {
    TEST_CASE("Basic Operations") {
        WaitMap waitMap;

        SUBCASE("Simple Wait and Notify") {
            std::atomic<bool> notified = false;
            std::string       path_str = "/test/path";
            std::string_view  path(path_str);

            std::thread waiter([&]() {
                auto guard = waitMap.wait(path);
                guard.wait_until(std::chrono::system_clock::now() + 1s, [&]() { return notified.load(); });
            });

            std::thread notifier([&]() {
                std::this_thread::sleep_for(100ms);
                notified = true;
                waitMap.notify(path);
            });

            waiter.join();
            notifier.join();
            CHECK(notified);
        }

        SUBCASE("Multiple Waiters Single Path") {
            std::atomic<int> counter{0};
            std::string      path_str = "/test/path";
            std::string_view path(path_str);
            const int        NUM_WAITERS = 3;

            std::vector<std::thread> waiters;
            for (int i = 0; i < NUM_WAITERS; ++i) {
                waiters.emplace_back([&]() {
                    auto guard = waitMap.wait(path);
                    guard.wait_until(std::chrono::system_clock::now() + 1s, [&]() { return counter.load() > 0; });
                    counter++;
                });
            }

            std::this_thread::sleep_for(100ms);
            counter = 1;
            waitMap.notify(path);

            for (auto& t : waiters) {
                t.join();
            }

            CHECK(counter == NUM_WAITERS + 1);
        }

        SUBCASE("Wait with Timeout") {
            std::string      path_str = "/test/path";
            std::string_view path(path_str);
            bool             condition_met = false;

            auto start = std::chrono::steady_clock::now();
            auto guard = waitMap.wait(path);
            guard.wait_until(std::chrono::system_clock::now() + 100ms, [&]() { return condition_met; });
            auto duration = std::chrono::steady_clock::now() - start;

            CHECK(duration >= 100ms);
            CHECK_FALSE(condition_met);
        }

        SUBCASE("Clear Operation") {
            std::atomic<bool> notified{false};
            std::string       path_str = "/test/path";
            std::string_view  path(path_str);

            std::thread waiter([&]() {
                auto guard = waitMap.wait(path);
                guard.wait_until(std::chrono::system_clock::now() + 500ms, [&]() { return notified.load(); });
            });

            std::this_thread::sleep_for(100ms);
            waitMap.clear();
            waitMap.notify(path); // Notification after clear

            waiter.join();
            CHECK_FALSE(notified); // Should timeout because clear removed the condition variable
        }
    }

    TEST_CASE("Path Pattern Matching") {
        WaitMap waitMap;

        SUBCASE("Basic Glob Pattern Matching Test") {
            WaitMap           waitMap;
            std::atomic<bool> waiterNotified{false};
            std::string       path_str = "/test/match/1";

            // Create a single waiter we expect to be notified
            std::thread waiter([&]() {
                std::string_view path(path_str);
                auto             guard = waitMap.wait(path);
                if (guard.wait_until(std::chrono::system_clock::now() + 200ms, []() { return true; })) {
                    waiterNotified = true;
                }
            });

            // Wait to ensure waiter is ready
            std::this_thread::sleep_for(50ms);

            // Try to notify using glob pattern
            waitMap.notify("/test/match/*");

            waiter.join();
            CHECK(waiterNotified); // Let's see if this waiter gets notified
        }

        // Also test the direct path match case for comparison
        SUBCASE("Direct Path Match Test") {
            WaitMap           waitMap;
            std::atomic<bool> waiterNotified{false};
            std::string       path_str = "/test/match/1";

            std::thread waiter([&]() {
                std::string_view path(path_str);
                auto             guard = waitMap.wait(path);
                if (guard.wait_until(std::chrono::system_clock::now() + 200ms, []() { return true; })) {
                    waiterNotified = true;
                }
            });

            std::this_thread::sleep_for(50ms);

            // Try to notify using exact same path
            waitMap.notify(path_str);

            waiter.join();
            CHECK(waiterNotified);
        }

        SUBCASE("Glob Pattern Notification") {
            std::atomic<int>         counter{0};
            const int                NUM_PATHS = 3;
            std::vector<std::thread> waiters;
            std::vector<std::string> path_strs; // Keep strings alive
            path_strs.reserve(NUM_PATHS);

            // Create waiters for paths /test/1, /test/2, /test/3
            for (int i = 1; i <= NUM_PATHS; ++i) {
                path_strs.push_back("/test/" + std::to_string(i));
                waiters.emplace_back([&, i]() {
                    std::string_view path(path_strs[i - 1]);
                    auto             guard = waitMap.wait(path);
                    guard.wait_until(std::chrono::system_clock::now() + 1s, [&]() { return counter.load() > 0; });
                    counter++;
                });
            }

            std::this_thread::sleep_for(100ms);
            counter = 1;
            // Notify all paths matching the pattern
            waitMap.notify("/test/*");

            for (auto& t : waiters) {
                t.join();
            }

            CHECK(counter == NUM_PATHS + 1);
        }

        SUBCASE("Partial Glob Pattern Match With Diagnostics") {
            WaitMap                   waitMap;
            std::atomic<bool>         notification_sent{false};
            std::atomic<bool>         match_waiter_done{false};
            std::atomic<bool>         nomatch_waiter_done{false};
            std::atomic<bool>         match_waiter_notified{false};
            std::atomic<bool>         nomatch_waiter_notified{false};
            std::vector<std::thread> waiters;

            // Keep string storage alive for the whole test
            std::string_view match_path   = "/test/match/1";
            std::string_view nomatch_path = "/test/nomatch/1";
            std::string      pattern      = "/test/match/*";

            // First waiter - should be notified
            waiters.emplace_back([&]() {
                auto guard            = waitMap.wait(match_path);
                bool was_notified     = guard.wait_until(std::chrono::system_clock::now() + 300ms) == std::cv_status::no_timeout;
                match_waiter_notified = was_notified;
                match_waiter_done     = true;
            });

            // Second waiter - should not be notified
            waiters.emplace_back([&]() {
                auto guard              = waitMap.wait(nomatch_path);
                bool was_notified       = guard.wait_until(std::chrono::system_clock::now() + 300ms) == std::cv_status::no_timeout;
                nomatch_waiter_notified = was_notified;
                nomatch_waiter_done     = true;
            });

            // Wait for waiters to be ready
            while (!waitMap.hasWaiters()) {
                std::this_thread::yield();
            }
            std::this_thread::sleep_for(50ms);

            // Send notification
            waitMap.notify(pattern);
            notification_sent = true;

            for (auto& t : waiters) { if (t.joinable()) t.join(); }
            waiters.clear(); // Clear after joining all threads

            CHECK(match_waiter_notified);         // Should be notified
            CHECK_FALSE(nomatch_waiter_notified); // Should not be notified
        }
    }

    TEST_CASE("Stress Testing") {
        WaitMap          waitMap;
        const int        NUM_THREADS           = 10;
        const int        OPERATIONS_PER_THREAD = 100;
        std::atomic<int> completedOperations{0};

        SUBCASE("Concurrent Operations") {
            std::vector<std::thread> threads;
            std::atomic<int>         readyThreads{0};
            std::vector<std::string> path_strs;

            // Pre-create paths to avoid string allocation during test
            for (int i = 0; i < 5; ++i) {
                path_strs.push_back("/test/" + std::to_string(i));
            }

            for (int i = 0; i < NUM_THREADS; ++i) {
                threads.emplace_back([&, i]() {
                    std::random_device              rd;
                    std::mt19937                    gen(rd());
                    std::uniform_int_distribution<> dis(0, 4);
                    readyThreads++;

                    // Wait for all threads to be ready
                    while (readyThreads < NUM_THREADS) {
                        std::this_thread::yield();
                    }

                    for (int j = 0; j < OPERATIONS_PER_THREAD; ++j) {
                        // Randomly choose between waiting and notifying
                        if (gen() % 2) {
                            std::string_view path(path_strs[dis(gen)]);
                            auto             guard = waitMap.wait(path);
                            guard.wait_until(std::chrono::system_clock::now() + 50ms, []() { return true; });
                        } else {
                            std::string_view path(path_strs[dis(gen)]);
                            waitMap.notify(path);
                        }
                        completedOperations++;
                    }
                });
            }

            for (auto& t : threads) {
                t.join();
            }

            CHECK(completedOperations == NUM_THREADS * OPERATIONS_PER_THREAD);
        }
    }

    TEST_CASE("Notify returns promptly under contention") {
        WaitMap waitMap;
        std::atomic<bool> ready{false};
        std::atomic<bool> stop{false};
        std::string       path_str = "/hot/path";
        std::string_view  path(path_str);

        // Single waiter holds a long wait to simulate contention on the entry mutex.
        std::thread waiter([&]() {
            auto guard = waitMap.wait(path);
            ready      = true;
            guard.wait_until(std::chrono::system_clock::now() + 2s, [&] { return stop.load(); });
        });

        // Ensure waiter is parked.
        while (!ready.load()) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(20ms);

        auto start = std::chrono::steady_clock::now();
        waitMap.notify(path);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);

        stop = true;
        waitMap.notify(path); // wake waiter
        waiter.join();

        CHECK_MESSAGE(elapsed < 200ms, "notify should not block even when a waiter holds the entry mutex");
    }

    TEST_CASE("Edge Cases") {
        WaitMap waitMap;

        SUBCASE("Empty Path") {
            std::string      empty_str;
            std::string_view path(empty_str);
            bool             exceptionThrown = false;
            try {
                waitMap.notify(path);
            } catch (...) {
                exceptionThrown = true;
            }
            CHECK_FALSE(exceptionThrown);
        }

        SUBCASE("Root Path") {
            std::atomic<bool> notified{false};
            std::string       root_str("/");
            std::string_view  path(root_str);

            std::thread waiter([&]() {
                auto guard = waitMap.wait(path);
                guard.wait_until(std::chrono::system_clock::now() + 100ms, [&]() { return notified.load(); });
            });

            std::this_thread::sleep_for(50ms);
            notified = true;
            waitMap.notify(path);

            if (waiter.joinable()) waiter.join();
            CHECK(notified);
        }
    }

    TEST_CASE("WaitMap Pattern Matching - Concrete Notification Matches Glob Waiter") {
        WaitMap           waitMap;
        std::atomic<bool> wasNotified{false};

        std::thread waiter([&]() {
            auto guard = waitMap.wait("/foo/[a-z]*"); // Wait with glob pattern
            if (guard.wait_until(std::chrono::system_clock::now() + 100ms)
                == std::cv_status::no_timeout) {
                wasNotified = true;
            }
        });

        std::this_thread::sleep_for(50ms);
        waitMap.notify("/foo/bar"); // Notify with concrete path

        waiter.join();
        CHECK(wasNotified);
    }

    TEST_CASE("WaitMap Pattern Matching - Glob Notification Matches Concrete Waiter") {
        WaitMap           waitMap;
        std::atomic<bool> wasNotified{false};

        std::thread waiter([&]() {
            auto guard = waitMap.wait("/foo/bar"); // Wait with concrete path
            if (guard.wait_until(std::chrono::system_clock::now() + 100ms)
                == std::cv_status::no_timeout) {
                wasNotified = true;
            }
        });

        std::this_thread::sleep_for(50ms);
        waitMap.notify("/foo/[a-z]*"); // Notify with glob pattern

        waiter.join();
        CHECK(wasNotified);
    }
}
