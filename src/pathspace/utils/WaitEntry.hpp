#pragma once
#include "path/ConcretePath.hpp"

#include <parallel_hashmap/phmap.h>

#include <atomic>
#include <mutex>

namespace SP {

struct WaitEntry {
    std::condition_variable cv;
    std::atomic<int> active_threads{0};
    std::mutex mutex;
};
using WaitMap = phmap::parallel_flat_hash_map<ConcretePathStringView, std::unique_ptr<WaitEntry>>;

} // namespace SP
