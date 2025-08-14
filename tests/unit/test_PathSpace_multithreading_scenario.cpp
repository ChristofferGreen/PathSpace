#include <pathspace/PathSpace.hpp>
#include <random>
#include <string>

#include "core/Out.hpp"
#include "ext/doctest.h"

using namespace SP;
/*SUBCASE("Dining Philosophers") {
    PathSpace pspace;
    const int NUM_PHILOSOPHERS     = 5;
    const int EATING_DURATION_MS   = 10;
    const int THINKING_DURATION_MS = 10;
    const int TEST_DURATION_MS     = 5000; // Increased to 5 seconds

    struct PhilosopherStats {
        std::atomic<int> meals_eaten{0};
        std::atomic<int> times_starved{0};
        std::atomic<int> forks_acquired{0};
    };

    std::vector<PhilosopherStats> stats(NUM_PHILOSOPHERS);

    auto philosopher = [&](int id) {
        std::string first_fork  = std::format("/fork/{}", std::min(id, (id + 1) % NUM_PHILOSOPHERS));
        std::string second_fork = std::format("/fork/{}", std::max(id, (id + 1) % NUM_PHILOSOPHERS));

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
    std::vector<std::jthread> philosophers;
    for (int i = 0; i < NUM_PHILOSOPHERS; ++i) {
        philosophers.emplace_back(philosopher, i);
    }

    // Wait for test to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(TEST_DURATION_MS));

    // Join all threads
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
}*/

static constexpr int NUM_PHILOSOPHERS     = 7;
static constexpr int NUM_TIMES_TO_EAT     = 200;
static constexpr int EATING_DURATION_MS   = 10;
static constexpr int THINKING_DURATION_MS = 3;

struct Philosopher {
    int leftForkUnavailable  = 0;
    int rightForkUnavailable = 0;
    int nbrTimesEaten        = 0;
};

Philosopher philosopher_action(PathSpace& space, int i) {
    Philosopher                     philosopher;
    std::mt19937                    rng(i);
    std::uniform_int_distribution<> eat_dist(3, EATING_DURATION_MS);
    std::uniform_int_distribution<> think_dist(3, THINKING_DURATION_MS);

    const int         left_fork_index  = i;
    const int         right_fork_index = (i + 1) % NUM_PHILOSOPHERS;
    const std::string left_fork_path   = "/forks/" + std::to_string(left_fork_index);
    const std::string right_fork_path  = "/forks/" + std::to_string(right_fork_index);

    while (philosopher.nbrTimesEaten < NUM_TIMES_TO_EAT) {
        Expected<int> left_fork;
        while (!(left_fork = space.take<int>(left_fork_path, Block(6ms)))) {
            philosopher.leftForkUnavailable++;
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
        Expected<int> right_fork;
        while (!(right_fork = space.take<int>(right_fork_path, Block(6ms)))) {
            philosopher.rightForkUnavailable++;
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(eat_dist(rng)));
        philosopher.nbrTimesEaten++;
        space.insert(left_fork_path, left_fork.value());
        space.insert(right_fork_path, right_fork.value());
        std::this_thread::sleep_for(std::chrono::milliseconds(think_dist(rng)));
    }
    return philosopher;
}

TEST_CASE("PathSpace Multithreading Scenario") {
    SUBCASE("Dining Philosophers") {
        PathSpace space;

        for (int i = 0; i < NUM_PHILOSOPHERS; ++i) {
            space.insert("/forks/" + std::to_string(i), i);
            space.insert<"/philosophers">([&space, i]() -> Philosopher { return philosopher_action(space, i); });
        }
        for (int i = 0; i < NUM_PHILOSOPHERS; ++i) {
            auto philosopherOpt = space.take<"/philosophers", Philosopher>(Block{});
            REQUIRE(philosopherOpt.has_value());
            Philosopher philosopher = philosopherOpt.value();
            REQUIRE(philosopher.nbrTimesEaten == 200);
            REQUIRE(philosopher.leftForkUnavailable > 0);
            REQUIRE(philosopher.rightForkUnavailable > 0);
        }
    }
}