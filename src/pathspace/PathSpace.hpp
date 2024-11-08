#pragma once
#include "PathSpaceLeaf.hpp"
#include "core/OutOptions.hpp"
#include "core/WaitMap.hpp"
#include "path/GlobPath.hpp"
#include "utils/TaggedLogger.hpp"

namespace SP {
struct TaskPool;
class PathSpace {
public:
    /**
     * @brief Constructs a PathSpace object.
     * @param pool Pointer to a TaskPool for managing asynchronous operations. If nullptr, uses the global instance.
     */
    explicit PathSpace(TaskPool* pool = nullptr);
    ~PathSpace();

    /**
     * @brief Inserts data into the PathSpace at the specified path.
     *
     * @tparam DataType The type of data being inserted.
     * @param path The glob-style path where the data should be inserted.
     * @param data The data to be inserted.
     * @param options Options controlling the insertion behavior, such as overwrite policies.
     * @return InsertReturn object containing information about the insertion operation, including any errors.
     */
    template <typename DataType>
    auto insert(GlobPathStringView const& path, DataType&& data, InOptions const& options = {}) -> InsertReturn {
        sp_log("PathSpace::insert", "Function Called");
        InputData inputData{std::forward<DataType>(data)};

        if (inputData.metadata.dataCategory == DataCategory::Execution)
            inputData.task = Task::Create(this, path.getPath(), std::forward<DataType>(data), inputData, options);

        return this->in(path, inputData, options);
    }

    /**
     * @brief Reads data from the PathSpace at the specified path.
     *
     * @tparam DataType The type of data to be read.
     * @param path The concrete path from which to read the data.
     * @param options Options controlling the read behavior, such as blocking policies.
     * @return Expected<DataType> containing the read data if successful, or an error if not.
     */
    template <typename DataType>
    auto read(ConcretePathStringView const& path, OutOptions const& options = {}) const -> Expected<DataType> {
        sp_log("PathSpace::read", "Function Called");
        DataType obj;
        bool const isExtract = false;
        if (auto ret = const_cast<PathSpace*>(this)->out(path, InputMetadataT<DataType>{}, options, &obj, isExtract); !ret)
            return std::unexpected(ret.error());
        return obj;
    }

    template <typename DataType>
    auto readBlock(ConcretePathStringView const& path,
                   OutOptions const& options = {.block{{.behavior = BlockOptions::Behavior::Wait}}}) const -> Expected<DataType> {
        sp_log("PathSpace::readBlock", "Function Called");
        bool const isExtract = false;
        return const_cast<PathSpace*>(this)->outBlock<DataType>(path, options, isExtract);
    }

    /**
     * @brief Reads and removes data from the PathSpace at the specified path.
     *
     * @tparam DataType The type of data to be extractbed.
     * @param path The concrete path from which to extract the data.
     * @return Expected<DataType> containing the extractbed data if successful, or an error if not.
     */
    template <typename DataType>
    auto extract(ConcretePathStringView const& path, OutOptions const& options = {}) -> Expected<DataType> {
        sp_log("PathSpace::extract", "Function Called");
        DataType obj;
        bool const isExtract = true;
        auto const ret = this->out(path, InputMetadataT<DataType>{}, options, &obj, isExtract);
        if (!ret)
            return std::unexpected(ret.error());
        if (ret.has_value() && (ret.value() == 0))
            return std::unexpected(Error{Error::Code::NoObjectFound, std::string("Object not found at: ").append(path.getPath())});
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

    TaskPool* pool = nullptr;
    PathSpaceLeaf root;
    WaitMap waitMap;
};

} // namespace SP