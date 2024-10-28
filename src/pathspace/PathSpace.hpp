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
            bool const isImmidiate
                    = (!options.execution.has_value())
                      || (options.execution.has_value() && options.execution.value().category == ExecutionOptions::Category::Immediate);
            bool const isOnReadOrExtract
                    = options.execution.has_value() && options.execution.value().category == ExecutionOptions::Category::OnReadOrExtract;
            if (std::shared_ptr<Task> Task = this->createTask(constructedPath, std::forward<DataType>(data), inputData, options)) {
                if (isImmidiate) {
                    this->pool->addTask(Task);
                    this->storage.store(std::move(Task));
                    return {.nbrTasksCreated = 1};
                } else if (isOnReadOrExtract) {
                    inputData.task = Task;
                }
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
        bool const doPop = false;
        if (auto ret = const_cast<PathSpace*>(this)->out(path, InputMetadataT<DataType>{}, options, &obj, doPop); !ret)
            return std::unexpected(ret.error());
        return obj;
    }

    template <typename DataType>
    auto readBlock(ConcretePathStringView const& path,
                   OutOptions const& options = {.block{{.behavior = BlockOptions::Behavior::Wait}}}) const -> Expected<DataType> {
        sp_log("PathSpace::readBlock", "Function Called");
        DataType obj;
        bool const doPop = false;
        auto result = const_cast<PathSpace*>(this)->out(path, InputMetadataT<DataType>{}, options, &obj, doPop);

        if (result.has_value() || !options.block.has_value()
            || (options.block.has_value() && options.block.value().behavior == BlockOptions::Behavior::DontWait)) {
            if (result.has_value() && result.value() > 0) {
                return obj;
            }
            std::unexpected(result.error());
        }

        bool const exitLoopAfterFirstRun = options.block && options.block->timeout;
        auto const timeout = (options.block && options.block->timeout) ? std::chrono::system_clock::now() + *options.block->timeout
                                                                       : std::chrono::system_clock::time_point::max();
        auto guard = waitMap.wait(path);
        while (!result.has_value()) {
            if (guard.wait_until(timeout, [&]() {
                    result = const_cast<PathSpace*>(this)->out(path, InputMetadataT<DataType>{}, options, &obj, doPop);
                    return (result.has_value() && result.value() > 0);
                })) {
                break;
            }
            if (exitLoopAfterFirstRun)
                break;
        }

        if (result.has_value() && result.value() > 0) {
            return obj;
        }
        return std::unexpected(result.error());
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
        bool const doPop = true;
        auto const ret = this->out(path, InputMetadataT<DataType>{}, options, &obj, doPop);
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
        DataType obj;
        bool const doPop = true;
        auto result = this->out(path, InputMetadataT<DataType>{}, options, &obj, doPop);

        if (result.has_value() || !options.block.has_value()
            || (options.block.has_value() && options.block.value().behavior == BlockOptions::Behavior::DontWait)) {
            if (result.has_value() && result.value() > 0) {
                return obj;
            }
            return std::unexpected(result.error());
        }

        bool const exitLoopAfterFirstRun = options.block && options.block->timeout;
        auto const timeout = (options.block && options.block->timeout) ? std::chrono::system_clock::now() + *options.block->timeout
                                                                       : std::chrono::system_clock::time_point::max();
        auto guard = waitMap.wait(path);
        while (!result.has_value()) {
            if (guard.wait_until(timeout, [&]() {
                    result = this->out(path, InputMetadataT<DataType>{}, options, &obj, doPop);
                    return (result.has_value() && result.value() > 0);
                })) {
                break;
            }
            if (exitLoopAfterFirstRun)
                break;
        }

        if (result.has_value() && result.value() > 0) {
            return obj;
        }
        return std::unexpected(result.error());
    }

    auto clear() -> void;

protected:
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
            return std::make_shared<Task>(
                    Task{.space = this,
                         .pathToInsertReturnValueTo = constructedPath,
                         .executionOptions = options.execution.has_value() ? options.execution.value() : ExecutionOptions{},
                         .function = std::move(function)});
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