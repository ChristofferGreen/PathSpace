#include "TaskPool.hpp"

namespace SP {

static int const pool_size = 2 * std::thread::hardware_concurrency();
static int const max_threads = 256 * pool_size;

TaskPool& TaskPool::Instance() {
    static TaskPool instance;
    return instance;
}

TaskPool::TaskPool() {
    // this->startWorkerThreads();
}

TaskPool::~TaskPool() {
    // this->shutdown();
}

} // namespace SP