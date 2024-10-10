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
    explicit PathSpace(TaskPool* pool = nullptr) {
        if (this->pool == nullptr)
            this->pool = &TaskPool::Instance();
    };

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
    auto insert(GlobPathStringView const& path, DataType const& data, InOptions const& options = {}) -> InsertReturn {
        InputData const inputData{data};
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

    virtual InsertReturn
    inImpl(ConstructiblePath& constructedPath, GlobPathStringView const& path, InputData const& data, InOptions const& options) {
        InsertReturn ret;
        if (!path.isValid()) {
            ret.errors.emplace_back(Error::Code::InvalidPath, std::string("The path was not valid: ").append(path.getPath()));
            return ret;
        }
        this->root.in(constructedPath, path.begin(), path.end(), InputData{data}, options, ret);
        if (ret.nbrSpacesInserted > 0 || ret.nbrValuesInserted > 0)
            this->cv.notify_all();
        return ret;
    };

    virtual Expected<int> outImpl(ConcretePathStringView const& path,
                                  InputMetadata const& inputMetadata,
                                  OutOptions const& options,
                                  Capabilities const& capabilities,
                                  void* obj) {
        auto const ret = this->root.out(path.begin(), path.end(), inputMetadata, obj, options, capabilities);
        if (ret.has_value())
            return ret;
        if (options.block.has_value() && options.block.value().behavior != BlockOptions::Behavior::DontWait
            && ret.error().code == Error::Code::NoSuchPath) {
            while (true) {
                std::unique_lock<std::mutex> lock(this->mutex);
                auto const ret = this->root.out(path.begin(), path.end(), inputMetadata, obj, options, capabilities);
                if (ret.has_value())
                    return ret;
                this->cv.wait(lock);
            }
        }
        return ret;
    }

    TaskPool* pool;
    std::condition_variable cv;
    std::mutex mutex;
    PathSpaceLeaf root;
};

} // namespace SP