#pragma once
#include "pathspace/task/MemoryPool.hpp"

namespace SP {

template <typename T>
class PoolDeleter {
public:
    explicit PoolDeleter(MemoryPool &pool) : pool(&pool) {}

    void operator()(T* ptr) const {
        if (ptr) {
            ptr->~T(); // Explicitly call the destructor
            pool->deallocate(ptr);
        }
    }

private:
    MemoryPool *pool;
};

}