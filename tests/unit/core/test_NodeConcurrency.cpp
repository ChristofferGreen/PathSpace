#include "core/Node.hpp"

#include "third_party/doctest.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace SP;

TEST_SUITE("core.node.concurrency") {
// getOrCreateChild must hand out a valid child while other threads keep growing the same children
// map. The map rehashes a shard when it grows, so the child has to be resolved while the shard is
// locked; a pointer read through an iterator afterwards can land in the old slot array.
TEST_CASE("concurrent getOrCreateChild returns one stable child per name") {
    constexpr int Rounds  = 200;
    constexpr int Threads = 4;
    constexpr int Names   = 48;

    for (int round = 0; round < Rounds; ++round) {
        Node                                  parent;
        std::vector<std::vector<Node*>>       seen(Threads, std::vector<Node*>(Names, nullptr));
        std::atomic<int>                      ready{0};
        std::vector<std::thread>              workers;
        workers.reserve(Threads);
        for (int t = 0; t < Threads; ++t) {
            workers.emplace_back([&, t] {
                ready.fetch_add(1);
                while (ready.load() < Threads) {
                    std::this_thread::yield();
                }
                for (int n = 0; n < Names; ++n) {
                    // Different start offsets so threads collide on creation and on lookup.
                    int const index = (n + t * 7) % Names;
                    seen[t][index]  = &parent.getOrCreateChild("child" + std::to_string(index));
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }

        bool consistent = true;
        for (int n = 0; n < Names; ++n) {
            Node* expected = parent.getChild("child" + std::to_string(n));
            consistent     = consistent && expected != nullptr;
            for (int t = 0; t < Threads; ++t) {
                consistent = consistent && seen[t][n] == expected;
            }
        }
        REQUIRE(consistent);
    }
}
}
