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

auto TaskPool::add(void (*executionWrapper)(void* const functionPointer, void* returnData),
                   void* const functionPointer,
                   void* returnData) -> void {
    executionWrapper(functionPointer, returnData);
}

} // namespace SP