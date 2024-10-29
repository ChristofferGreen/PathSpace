#pragma once
#include "PathSpaceLeaf.hpp"
#include "core/OutOptions.hpp"
#include "core/WaitMap.hpp"
#include "path/GlobPath.hpp"
#include "taskpool/TaskPool.hpp"
#include "taskpool/TaskStorage.hpp"
#include "utils/TaggedLogger.hpp"
#include <memory>

namespace SP {
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
        ConstructiblePath constructedPath = path.isConcrete() ? ConstructiblePath{path} : ConstructiblePath{};

        if (inputData.metadata.category == DataCategory::ExecutionFunctionPointer
            || inputData.metadata.category == DataCategory::ExecutionStdFunction) {
            bool const isImmediate
                    = (!options.execution.has_value())
                      || (options.execution.has_value() && options.execution.value().category == ExecutionOptions::Category::Immediate);
            bool const isOnReadOrExtract
                    = options.execution.has_value() && options.execution.value().category == ExecutionOptions::Category::OnReadOrExtract;
            if (std::shared_ptr<Task> Task = this->createTask(constructedPath, std::forward<DataType>(data), inputData, options)) {
                if (isImmediate) {
                    this->pool->addTask(Task);
                    this->storage.store(std::move(Task));
                    return {.nbrTasksCreated = 1};
                }
                inputData.task = Task;
            }
        }

        return this->in(constructedPath, path, inputData, options);
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
    template <typename DataType>
    auto outBlock(ConcretePathStringView const& path, OutOptions const& options, bool const isExtract) -> Expected<DataType> {
        sp_log("PathSpace::outBlock", "Function Called");

        bool const hasTimeout = options.block && options.block->timeout;

        auto const inputMetaData = InputMetadataT<DataType>{};
        DataCategory const category = inputMetaData.category;
        bool const isAsync
                = hasTimeout && (category == DataCategory::ExecutionFunctionPointer || category == DataCategory::ExecutionStdFunction);

        if (isAsync) {
            if (options.execution.has_value())
                const_cast<OutOptions&>(options).execution.value().category = ExecutionOptions::Category::Async;
            else
                const_cast<OutOptions&>(options).execution = ExecutionOptions{.category = ExecutionOptions::Category::Async};
        }

        DataType obj;
        auto result = this->out(path, inputMetaData, options, &obj, isExtract);

        // If we got a value or shouldn't block, return immediately
        if (result.has_value() || !options.block.has_value()
            || (options.block.has_value() && options.block.value().behavior == BlockOptions::Behavior::DontWait)) {
            if (result.has_value() && result.value() > 0) {
                return obj;
            }
            // For non-blocking calls, return the original error
            return std::unexpected(result.error());
        }

        auto guard = waitMap.wait(path);
        auto const timeout
                = hasTimeout ? std::chrono::system_clock::now() + *options.block->timeout : std::chrono::system_clock::time_point::max();
        while (true) {
            if (guard.wait_until(timeout, [&]() {
                    result = this->out(path, inputMetaData, options, &obj, isExtract);
                    return (result.has_value() && result.value() > 0);
                })) {
                break;
            }

            // Check for timeout
            if (hasTimeout && std::chrono::system_clock::now() >= timeout) {
                return std::unexpected(
                        Error{Error::Code::Timeout, "Operation timed out waiting for data at path: " + std::string(path.getPath())});
            }

            // If we have a timeout set, only try once
            if (hasTimeout)
                break;
        }

        // Final timeout check before returning
        if (hasTimeout && std::chrono::system_clock::now() >= timeout) {
            return std::unexpected(
                    Error{Error::Code::Timeout, "Operation timed out waiting for data at path: " + std::string(path.getPath())});
        }

        if (result.has_value() && result.value() > 0) {
            return obj;
        }
        return std::unexpected(result.error());
    }

    template <typename DataType>
    auto createTask(ConstructiblePath const& constructedPath, DataType const& data, InputData const& inputData, InOptions const& options)
            -> std::shared_ptr<Task> { // ToDo:: Add support for glob based executions
        sp_log("PathSpace::createTask", "Function Called");
        if constexpr (ExecutionFunctionPointer<DataType> || ExecutionStdFunction<DataType>) {
            auto function = [userFunction = std::move(data)](Task const& task, void* obj, bool isOut) {
                if (isOut) {
                    *static_cast<std::function<std::invoke_result_t<DataType>()>*>(obj) = userFunction;
                } else {
                    if (obj == nullptr) {
                        assert(task.space != nullptr);
                        task.space->insert(task.pathToInsertReturnValueTo.getPath(), userFunction());
                    } else {
                        *static_cast<std::invoke_result_t<DataType>*>(obj) = userFunction();
                    }
                }
            };
            bool const hasTimeout = options.block.has_value() && options.block->timeout.has_value();
            auto task = std::make_shared<Task>(
                    Task{.space = this,
                         .pathToInsertReturnValueTo = constructedPath,
                         .executionOptions = options.execution.has_value() ? options.execution.value() : ExecutionOptions{},
                         .function = std::move(function),
                         .resultStorage = hasTimeout ? std::invoke_result_t<DataType>() : std::any(),
                         .resultCopy = [](void const* const from, void* const to) {
                             *static_cast<std::invoke_result_t<DataType>*>(to) = *static_cast<std::invoke_result_t<DataType> const*>(from);
                         }});
            task->resultPtr = std::any_cast<std::invoke_result_t<DataType>*>(&task->resultStorage);
            if (hasTimeout && task->executionOptions.category == ExecutionOptions::Category::Immediate) {
                task->executionOptions.category = ExecutionOptions::Category::Async;
            }
            return std::move(task);
        }
        return {};
    }

    virtual auto in(ConstructiblePath& constructedPath, GlobPathStringView const& path, InputData const& data, InOptions const& options)
            -> InsertReturn;

    virtual auto
    out(ConcretePathStringView const& path, InputMetadata const& inputMetadata, OutOptions const& options, void* obj, bool const doPop)
            -> Expected<int>;

    auto shutdown() -> void;

    TaskStorage storage;
    TaskPool* pool = nullptr;
    PathSpaceLeaf root;
    mutable WaitMap waitMap;
};

} // namespace SP