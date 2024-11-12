#include "core/WaitMap.hpp"

#include "ext/doctest.h"

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
            std::atomic<bool>      notified = false;
            std::string            path_str = "/test/path";
            ConcretePathStringView path(path_str);

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
            std::atomic<int>       counter{0};
            std::string            path_str = "/test/path";
            ConcretePathStringView path(path_str);
            const int              NUM_WAITERS = 3;

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
            std::string            path_str = "/test/path";
            ConcretePathStringView path(path_str);
            bool                   condition_met = false;

            auto start = std::chrono::steady_clock::now();
            auto guard = waitMap.wait(path);
            guard.wait_until(std::chrono::system_clock::now() + 100ms, [&]() { return condition_met; });
            auto duration = std::chrono::steady_clock::now() - start;

            CHECK(duration >= 100ms);
            CHECK_FALSE(condition_met);
        }

        SUBCASE("Clear Operation") {
            std::atomic<bool>      notified{false};
            std::string            path_str = "/test/path";
            ConcretePathStringView path(path_str);

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
                ConcretePathStringView path(path_str);
                auto                   guard = waitMap.wait(path);
                if (guard.wait_until(std::chrono::system_clock::now() + 200ms, []() { return true; })) {
                    waiterNotified = true;
                }
            });

            // Wait to ensure waiter is ready
            std::this_thread::sleep_for(50ms);

            // Try to notify using glob pattern
            MESSAGE("Notifying with glob pattern /test/match/*");
            waitMap.notify(GlobPathStringView("/test/match/*"));

            waiter.join();
            CHECK(waiterNotified); // Let's see if this waiter gets notified
        }

        // Also test the direct path match case for comparison
        SUBCASE("Direct Path Match Test") {
            WaitMap           waitMap;
            std::atomic<bool> waiterNotified{false};
            std::string       path_str = "/test/match/1";

            std::thread waiter([&]() {
                ConcretePathStringView path(path_str);
                auto                   guard = waitMap.wait(path);
                if (guard.wait_until(std::chrono::system_clock::now() + 200ms, []() { return true; })) {
                    waiterNotified = true;
                }
            });

            std::this_thread::sleep_for(50ms);

            // Try to notify using exact same path
            MESSAGE("Notifying with exact path /test/match/1");
            waitMap.notify(ConcretePathStringView(path_str));

            waiter.join();
            CHECK(waiterNotified);
        }

        // Add diagnostic test for the glob matching logic
        SUBCASE("Glob Pattern Match Diagnostic") {
            std::string            path_str = "/test/match/1";
            GlobPathStringView     glob_pattern("/test/match/*");
            ConcretePathStringView concrete_path(path_str);

            MESSAGE("Checking if glob pattern matches path:");
            MESSAGE("  Pattern: " << glob_pattern.getPath());
            MESSAGE("  Path: " << concrete_path.getPath());

            bool matches = (glob_pattern == concrete_path);
            MESSAGE("  Direct comparison result: " << matches);

            // Check pattern matching segment by segment
            auto patternIter = glob_pattern.begin();
            auto pathIter    = concrete_path.begin();

            while (patternIter != glob_pattern.end() && pathIter != concrete_path.end()) {
                auto [isMatch, isSupermatch] = (*patternIter).match((*pathIter).getName());
                MESSAGE("  Segment match: " << (*pathIter).getName() << " against pattern: " << (*patternIter).getName() << " -> match=" << isMatch << ", supermatch=" << isSupermatch);
                ++patternIter;
                ++pathIter;
            }

            MESSAGE("  Pattern at end: " << (patternIter == glob_pattern.end()));
            MESSAGE("  Path at end: " << (pathIter == concrete_path.end()));
        }

        SUBCASE("Glob Pattern Notification") {
            std::atomic<int>         counter{0};
            const int                NUM_PATHS = 3;
            std::vector<std::thread> waiters;
            std::vector<std::string> path_strs; // Keep strings alive

            // Create waiters for paths /test/1, /test/2, /test/3
            for (int i = 1; i <= NUM_PATHS; ++i) {
                path_strs.push_back("/test/" + std::to_string(i));
                waiters.emplace_back([&, i]() {
                    ConcretePathStringView path(path_strs.back());
                    auto                   guard = waitMap.wait(path);
                    guard.wait_until(std::chrono::system_clock::now() + 1s, [&]() { return counter.load() > 0; });
                    counter++;
                });
            }

            std::this_thread::sleep_for(100ms);
            counter = 1;
            // Notify all paths matching the pattern
            waitMap.notify(GlobPathStringView("/test/*"));

            for (auto& t : waiters) {
                t.join();
            }

            CHECK(counter == NUM_PATHS + 1);
        }

        SUBCASE("Debug Glob Pattern Matching") {
            GlobPathStringView     pattern("/test/match/*");
            ConcretePathStringView path1("/test/match/1");
            ConcretePathStringView path2("/test/nomatch/1");

            MESSAGE("Testing pattern: " << pattern.getPath());

            // Test first path
            MESSAGE("Testing against path: " << path1.getPath());
            auto patternIter1 = pattern.begin();
            auto pathIter1    = path1.begin();
            while (patternIter1 != pattern.end() && pathIter1 != path1.end()) {
                auto [isMatch, isSuper] = (*patternIter1).match((*pathIter1).getName());
                MESSAGE("  Segment match: '" << (*pathIter1).getName() << "' against '" << (*patternIter1).getName() << "' -> " << isMatch << ", " << isSuper);
                CHECK(isMatch);
                ++patternIter1;
                ++pathIter1;
            }
            CHECK(patternIter1 == pattern.end());
            CHECK(pathIter1 == path1.end());

            // Test second path
            MESSAGE("Testing against path: " << path2.getPath());
            auto patternIter2 = pattern.begin();
            auto pathIter2    = path2.begin();
            while (patternIter2 != pattern.end() && pathIter2 != path2.end()) {
                auto [isMatch, isSuper] = (*patternIter2).match((*pathIter2).getName());
                MESSAGE("  Segment match: '" << (*pathIter2).getName() << "' against '" << (*patternIter2).getName() << "' -> " << isMatch << ", " << isSuper);
                if (!isMatch)
                    break;
                ++patternIter2;
                ++pathIter2;
            }
            CHECK(!(patternIter2 == pattern.end() && pathIter2 == path2.end()));
        }

        SUBCASE("Partial Glob Pattern Match With Diagnostics") {
            WaitMap                   waitMap;
            std::atomic<bool>         notification_sent{false};
            std::atomic<bool>         match_waiter_done{false};
            std::atomic<bool>         nomatch_waiter_done{false};
            std::atomic<bool>         match_waiter_notified{false};
            std::atomic<bool>         nomatch_waiter_notified{false};
            std::vector<std::jthread> waiters;

            // Keep string storage alive for the whole test
            std::string match_path   = "/test/match/1";
            std::string nomatch_path = "/test/nomatch/1";
            std::string pattern      = "/test/match/*";

            MESSAGE("Starting test with paths:");
            MESSAGE("  Match path: " << match_path);
            MESSAGE("  No-match path: " << nomatch_path);
            MESSAGE("  Pattern: " << pattern);

            // First waiter - should be notified
            waiters.emplace_back([&]() {
                MESSAGE("Match waiter starting");
                auto guard            = waitMap.wait(match_path);
                bool was_notified     = guard.wait_until(std::chrono::system_clock::now() + 300ms) == std::cv_status::no_timeout;
                match_waiter_notified = was_notified;
                match_waiter_done     = true;
                MESSAGE("Match waiter finished - was_notified: " << was_notified);
            });

            // Second waiter - should not be notified
            waiters.emplace_back([&]() {
                MESSAGE("No-match waiter starting");
                auto guard              = waitMap.wait(nomatch_path);
                bool was_notified       = guard.wait_until(std::chrono::system_clock::now() + 300ms) == std::cv_status::no_timeout;
                nomatch_waiter_notified = was_notified;
                nomatch_waiter_done     = true;
                MESSAGE("No-match waiter finished - was_notified: " << was_notified);
            });

            // Wait for waiters to be ready
            MESSAGE("Waiting for waiters to be ready");
            while (!waitMap.hasWaiters()) {
                std::this_thread::yield();
            }
            std::this_thread::sleep_for(50ms);

            // Send notification
            MESSAGE("Sending notification with pattern: " << pattern);
            waitMap.notify(GlobPathStringView(pattern));
            notification_sent = true;
            MESSAGE("Notification sent");

            waiters.clear(); // Clear waiters
            MESSAGE("All waiters completed");

            // Verify results
            MESSAGE("Final state:");
            MESSAGE("  Match waiter done: " << match_waiter_done.load());
            MESSAGE("  No-match waiter done: " << nomatch_waiter_done.load());
            MESSAGE("  Match waiter notified: " << match_waiter_notified.load());
            MESSAGE("  No-match waiter notified: " << nomatch_waiter_notified.load());

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
                            ConcretePathStringView path(path_strs[dis(gen)]);
                            auto                   guard = waitMap.wait(path);
                            guard.wait_until(std::chrono::system_clock::now() + 50ms, []() { return true; });
                        } else {
                            ConcretePathStringView path(path_strs[dis(gen)]);
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

    TEST_CASE("Edge Cases") {
        WaitMap waitMap;

        SUBCASE("Empty Path") {
            std::string            empty_str;
            ConcretePathStringView path(empty_str);
            bool                   exceptionThrown = false;
            try {
                waitMap.notify(path);
            } catch (...) {
                exceptionThrown = true;
            }
            CHECK_FALSE(exceptionThrown);
        }

        SUBCASE("Root Path") {
            std::atomic<bool>      notified{false};
            std::string            root_str("/");
            ConcretePathStringView path(root_str);

            std::thread waiter([&]() {
                auto guard = waitMap.wait(path);
                guard.wait_until(std::chrono::system_clock::now() + 100ms, [&]() { return notified.load(); });
            });

            std::this_thread::sleep_for(50ms);
            notified = true;
            waitMap.notify(path);

            waiter.join();
            CHECK(notified);
        }
    }
}