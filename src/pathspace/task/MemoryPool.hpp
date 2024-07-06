#pragma once
#include <vector>
#include <cstddef>

namespace SP {

class MemoryPool {
public:
    explicit MemoryPool(std::size_t blockSize, std::size_t blockCount)
        : blockSize(blockSize), blockCount(blockCount), pool(blockSize * blockCount) {
        for (std::size_t i = 0; i < blockCount; ++i) {
            freeBlocks.push_back(&pool[i * blockSize]);
        }
    }

    void* allocate() {
        if (freeBlocks.empty()) {
            throw std::bad_alloc();
        }
        void* block = freeBlocks.back();
        freeBlocks.pop_back();
        return block;
    }

    void deallocate(void* block) {
        freeBlocks.push_back(static_cast<char*>(block));
    }

private:
    std::size_t blockSize;
    std::size_t blockCount;
    std::vector<char> pool;
    std::vector<void*> freeBlocks;
};

}