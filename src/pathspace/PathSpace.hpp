#pragma once
#include "PathSpaceLeaf.hpp"
#include "core/InsertReturn.hpp"
#include "core/OutOptions.hpp"
#include "core/WaitMap.hpp"
#include "path/GlobPath.hpp"
#include "path/validation.hpp"
#include "task/Task.hpp"
#include "type/InputData.hpp"
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
        if (auto error = path.validate())
            return InsertReturn{.errors = {*error}};

        InputData inputData{std::forward<DataType>(data)};

        if (inputData.metadata.dataCategory == DataCategory::Execution) {
            inputData.taskCreator = [&, pathStr = std::string(path.getPath())]() -> std::shared_ptr<Task> {
                return Task::Create(this, pathStr, std::forward<DataType>(data), inputData, options);
            };
        }

        return this->in(path, inputData, options);
    }

    template <fixed_string pathIn, typename DataType>
        requires(validate_path(pathIn))
    auto insert(DataType&& data, InOptions const& options = {}) -> InsertReturn {
        sp_log("PathSpace::insert", "Function Called");
        GlobPathStringView const& path{pathIn};

        InputData inputData{std::forward<DataType>(data)};

        if (inputData.metadata.dataCategory == DataCategory::Execution) {
            inputData.taskCreator = [&, pathStr = std::string(path.getPath())]() -> std::shared_ptr<Task> {
                return Task::Create(this, pathStr, std::forward<DataType>(data), inputData, options);
            };
        }

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
        if (auto error = path.validate())
            return std::unexpected(*error);
        DataType   obj;
        bool const doExtract = false;
        if (auto ret = const_cast<PathSpace*>(this)->out(path, InputMetadataT<DataType>{}, options, &obj, doExtract); !ret)
            return std::unexpected(ret.error());
        return obj;
    }

    template <typename DataType>
    auto readBlock(ConcretePathStringView const& path, OutOptions const& options = {.block{{.behavior = BlockOptions::Behavior::Wait}}}) const -> Expected<DataType> {
        sp_log("PathSpace::readBlock", "Function Called");
        if (auto error = path.validate())
            return std::unexpected(*error);
        bool const doExtract = false;
        return const_cast<PathSpace*>(this)->outBlock<DataType>(path, options, doExtract);
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
        if (auto error = path.validate())
            return std::unexpected(*error);
        DataType   obj;
        bool const doExtract = true;
        auto const ret       = this->out(path, InputMetadataT<DataType>{}, options, &obj, doExtract);
        if (!ret)
            return std::unexpected(ret.error());
        if (ret.has_value() && (ret.value() == 0))
            return std::unexpected(Error{Error::Code::NoObjectFound, std::string("Object not found at: ").append(path.getPath())});
        return obj;
    }

    template <typename DataType>
    auto extractBlock(ConcretePathStringView const& path, OutOptions const& options = {.block{{.behavior = BlockOptions::Behavior::Wait}}}) -> Expected<DataType> {
        sp_log("PathSpace::extractBlock", "Function Called");
        if (auto error = path.validate())
            return std::unexpected(*error);
        bool const doExtract = true;
        return this->outBlock<DataType>(path, options, doExtract);
    }

    auto clear() -> void;

protected:
    friend class TaskPool;

    template <typename DataType>
    auto outBlock(ConcretePathStringView const& path, OutOptions const& options, bool const doExtract) -> Expected<DataType> {
        sp_log("PathSpace::outBlock", "Function Called");

        DataType      obj;
        Expected<int> result; // Moved outside to be accessible in all scopes
        auto const    inputMetaData = InputMetadataT<DataType>{};
        auto const    deadline      = options.block && options.block->timeout ? std::chrono::system_clock::now() + *options.block->timeout : std::chrono::system_clock::time_point::max();

        // First try entirely outside the loop to minimize lock time
        {
            result = this->out(path, inputMetaData, options, &obj, doExtract);
            if (result.has_value() && result.value() > 0) {
                return obj;
            }
        }

        while (true) {
            // Check deadline first
            auto now = std::chrono::system_clock::now();
            if (now >= deadline) {
                return std::unexpected(Error{Error::Code::Timeout, "Operation timed out waiting for data at path: " + std::string(path.getPath())});
            }

            // Wait with minimal scope
            auto guard = waitMap.wait(path);
            {
                bool success = guard.wait_until(deadline, [&]() {
                    result          = this->out(path, inputMetaData, options, &obj, doExtract);
                    bool haveResult = (result.has_value() && result.value() > 0);
                    return haveResult;
                });

                if (success && result.has_value() && result.value() > 0) {
                    return obj;
                }
            }

            if (std::chrono::system_clock::now() >= deadline) {
                return std::unexpected(Error{Error::Code::Timeout, "Operation timed out after waking from guard, waiting for data at path: " + std::string(path.getPath())});
            }
        }
    }

    virtual auto in(GlobPathStringView const& path, InputData const& data, InOptions const& options) -> InsertReturn;
    virtual auto out(ConcretePathStringView const& path, InputMetadata const& inputMetadata, OutOptions const& options, void* obj, bool const doExtract) -> Expected<int>;
    auto         shutdown() -> void;

    TaskPool*     pool = nullptr;
    PathSpaceLeaf root;
    WaitMap       waitMap;
};

} // namespace SP