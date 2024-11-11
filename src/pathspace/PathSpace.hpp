#pragma once
#include "PathSpaceLeaf.hpp"
#include "core/Cache.hpp"
#include "core/OutOptions.hpp"
#include "core/WaitMap.hpp"
#include "path/GlobPath.hpp"
#include "utils/TaggedLogger.hpp"

namespace SP {
struct TaskPool;

class PathSpace {
public:
    // Updated constructor to accept cache size
    explicit PathSpace(size_t cacheSize = 1000, TaskPool* pool = nullptr);
    ~PathSpace();

    template <typename DataType>
    auto insert(GlobPathStringView const& path, DataType&& data, InOptions const& options = {}) -> InsertReturn {
        sp_log("PathSpace::insert", "Function Called");
        InputData inputData{std::forward<DataType>(data)};

        if (inputData.metadata.dataCategory == DataCategory::Execution) {
            inputData.task = Task::Create(this, path.getPath(), std::forward<DataType>(data), inputData, options);
        }

        auto ret = this->in(path, inputData, options);

        // Invalidate cache after insert
        if (path.isGlob()) {
            cache.invalidatePattern(path.getPath());
        } else {
            cache.invalidatePrefix(path.getPath());
        }

        return ret;
    }

    template <typename DataType>
    auto read(ConcretePathStringView const& path, OutOptions const& options = {}) const -> Expected<DataType> {
        sp_log("PathSpace::read", "Function Called");

        // Skip cache if bypass requested
        if (options.bypassCache) {
            return readDirect<DataType>(path, options);
        }

        // Check cache first
        if (auto cachedLeaf = cache.lookup(ConcretePathString{path}, root)) {
            DataType obj;
            bool const isExtract = false;
            if (auto ret = cachedLeaf.value()->out(path.begin(), path.end(), InputMetadataT<DataType>{}, &obj, options, isExtract); ret) {
                return obj;
            }
        }

        // Cache miss - do direct read and cache result
        auto result = readDirect<DataType>(path, options);
        if (result) {
            cache.store(path, const_cast<PathSpaceLeaf&>(root));
        }
        return result;
    }

    template <typename DataType>
    auto readBlock(ConcretePathStringView const& path,
                   OutOptions const& options = {.block{{.behavior = BlockOptions::Behavior::Wait}}}) const -> Expected<DataType> {
        sp_log("PathSpace::readBlock", "Function Called");
        bool const isExtract = false;
        return const_cast<PathSpace*>(this)->outBlock<DataType>(path, options, isExtract);
    }

    template <typename DataType>
    auto extract(ConcretePathStringView const& path, OutOptions const& options = {}) -> Expected<DataType> {
        sp_log("PathSpace::extract", "Function Called");

        // Always invalidate cache for extracts since they modify data
        cache.invalidate(ConcretePathString{path});

        DataType obj;
        bool const isExtract = true;
        auto const ret = this->out(path, InputMetadataT<DataType>{}, options, &obj, isExtract);
        if (!ret) {
            return std::unexpected(ret.error());
        }
        if (ret.value() == 0) {
            return std::unexpected(Error{Error::Code::NoObjectFound, std::string("Object not found at: ").append(path.getPath())});
        }
        return obj;
    }

    template <typename DataType>
    auto extractBlock(ConcretePathStringView const& path, OutOptions const& options = {.block{{.behavior = BlockOptions::Behavior::Wait}}})
            -> Expected<DataType> {
        sp_log("PathSpace::extractBlock", "Function Called");
        bool const isExtract = true;
        return this->outBlock<DataType>(path, options, isExtract);
    }

    auto clear() -> void;
    auto getCacheStats() const -> const CacheStats;
    auto resetCacheStats() -> void;

protected:
    friend class TaskPool;

    template <typename DataType>
    auto outBlock(ConcretePathStringView const& path, OutOptions const& options, bool const isExtract) -> Expected<DataType> {
        sp_log("PathSpace::outBlock", "Function Called");

        DataType obj;
        auto const inputMetaData = InputMetadataT<DataType>{};
        auto const deadline = options.block && options.block->timeout ? std::chrono::system_clock::now() + *options.block->timeout
                                                                      : std::chrono::system_clock::time_point::max();
        // Retry loop
        while (true) {
            // First try without waiting
            auto result = this->out(path, inputMetaData, options, &obj, isExtract);
            if (result.has_value() && result.value() > 0) {
                return obj;
            }

            // Check if we're already past deadline
            auto now = std::chrono::system_clock::now();
            if (now >= deadline) {
                return std::unexpected(
                        Error{Error::Code::Timeout, "Operation timed out waiting for data at path: " + std::string(path.getPath())});
            }

            // Wait for data with proper timeout
            auto guard = waitMap.wait(path);
            bool success = guard.wait_until(deadline, [&]() {
                result = this->out(path, inputMetaData, options, &obj, isExtract);
                return (result.has_value() && result.value() > 0);
            });

            if (success && result.has_value() && result.value() > 0) {
                return obj;
            }

            // If we timed out, return error
            if (std::chrono::system_clock::now() >= deadline) {
                return std::unexpected(
                        Error{Error::Code::Timeout,
                              "Operation timed out after waking from guard, waiting for data at path: " + std::string(path.getPath())});
            }
        }
    }

    virtual auto in(GlobPathStringView const& path, InputData const& data, InOptions const& options) -> InsertReturn;
    virtual auto
    out(ConcretePathStringView const& path, InputMetadata const& inputMetadata, OutOptions const& options, void* obj, bool const isExtract)
            -> Expected<int>;

    auto shutdown() -> void;

    template <typename DataType>
    auto readDirect(ConcretePathStringView const& path, OutOptions const& options) const -> Expected<DataType> {
        DataType obj;
        bool const isExtract = false;
        if (auto ret = const_cast<PathSpace*>(this)->out(path, InputMetadataT<DataType>{}, options, &obj, isExtract); !ret) {
            return std::unexpected(ret.error());
        }
        return obj;
    }

    TaskPool* pool = nullptr;
    PathSpaceLeaf root;
    WaitMap waitMap;
    mutable Cache cache;
};

} // namespace SP