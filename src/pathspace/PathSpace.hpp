#pragma once
#include "PathSpaceLeaf.hpp"
#include "core/OutOptions.hpp"
#include "taskpool/TaskPool.hpp"

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
        InputData const inputData{std::forward<DataType>(data)};
        ConstructiblePath constructedPath = path.isConcrete() ? ConstructiblePath{path} : ConstructiblePath{};
        if (std::optional<Task> task = this->createTask(constructedPath, data, inputData, options)) {
            this->pool->addTask(std::move(task.value()));
            return {.nbrTasksCreated = 1};
        } else
            return this->inImpl(constructedPath, path, inputData, options);
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
    auto read(ConcretePathStringView const& path,
              OutOptions const& options = {.doPop = false},
              Capabilities const& capabilities = Capabilities::All()) const -> Expected<DataType> {
        if (options.doPop)
            return std::unexpected(Error{Error::Code::PopInRead, std::string("read does not support doPop: ").append(path.getPath())});
        DataType obj;
        if (auto ret = const_cast<PathSpace*>(this)->outImpl(path, InputMetadataT<DataType>{}, options, capabilities, &obj); !ret)
            return std::unexpected(ret.error());
        return obj;
    }

    template <typename DataType>
    auto readBlock(ConcretePathStringView const& path,
                   OutOptions const& options = {.block{{.behavior = BlockOptions::Behavior::Wait}}, .doPop = false},
                   Capabilities const& capabilities = Capabilities::All()) const -> Expected<DataType> {
        if (options.doPop)
            return std::unexpected(Error{Error::Code::PopInRead, std::string("readBlock does not support doPop: ").append(path.getPath())});
        DataType obj;
        if (auto ret = const_cast<PathSpace*>(this)->outImpl(path, InputMetadataT<DataType>{}, options, capabilities, &obj); !ret)
            return std::unexpected(ret.error());
        return obj;
    }

    /**
     * @brief Reads and removes data from the PathSpace at the specified path.
     *
     * @tparam DataType The type of data to be extractbed.
     * @param path The concrete path from which to extract the data.
     * @param capabilities Capabilities controlling access to the data.
     * @return Expected<DataType> containing the extractbed data if successful, or an error if not.
     */
    template <typename DataType>
    auto extract(ConcretePathStringView const& path, OutOptions const& options = {}, Capabilities const& capabilities = Capabilities::All())
            -> Expected<DataType> {
        DataType obj;
        if (auto ret = this->outImpl(path, InputMetadataT<DataType>{}, options, capabilities, &obj); !ret)
            return std::unexpected(ret.error());
        return obj;
    }

    template <typename DataType>
    auto extractBlock(ConcretePathStringView const& path,
                      OutOptions const& options = {.block{{.behavior = BlockOptions::Behavior::Wait}}},
                      Capabilities const& capabilities = Capabilities::All()) -> Expected<DataType> {
        DataType obj;
        if (auto ret = this->outImpl(path, InputMetadataT<DataType>{}, options, capabilities, &obj); !ret)
            return std::unexpected(ret.error());
        return obj;
    }

protected:
    template <typename DataType>
    auto createTask(ConstructiblePath const& constructedPath, DataType const& data, InputData const& inputData, InOptions const& options)
            -> std::optional<Task> {
        bool const isFunctionPointer = (inputData.metadata.category == DataCategory::ExecutionFunctionPointer);
        bool const isImmediateExecution
                = (!options.execution.has_value()
                   || (options.execution.has_value()
                       && options.execution.value().category
                                  == ExecutionOptions::Category::Immediate)); // ToDo: Add support for lazy executions
        if constexpr (ExecutionFunctionPointer<DataType>) {
            if (isFunctionPointer && isImmediateExecution) { // ToDo:: Add support for glob based executions
                auto fun = [](Task const& task) {
                    assert(task.space);
                    if (task.userSuppliedFunctionPointer != nullptr) {
                        auto userFunction = reinterpret_cast<std::invoke_result_t<DataType> (*)()>(task.userSuppliedFunctionPointer);
                        task.space->insert(task.pathToInsertReturnValueTo.getPath(), userFunction());
                    }
                };
                return Task{.userSuppliedFunctionPointer = inputData.obj,
                            .space = this,
                            .pathToInsertReturnValueTo = constructedPath,
                            .executionOptions = options.execution.has_value() ? options.execution.value() : ExecutionOptions{},
                            .taskExecutor = fun};
            }
        }
        return std::nullopt;
    }

    virtual auto inImpl(ConstructiblePath& constructedPath, GlobPathStringView const& path, InputData const& data, InOptions const& options)
            -> InsertReturn;

    virtual auto outImpl(ConcretePathStringView const& path,
                         InputMetadata const& inputMetadata,
                         OutOptions const& options,
                         Capabilities const& capabilities,
                         void* obj) -> Expected<int>;

    auto shutdown() -> void;

    TaskPool* pool;
    std::condition_variable cv;
    std::mutex mutex;
    PathSpaceLeaf root;
    std::atomic<bool> shuttingDown{false};
};

} // namespace SP