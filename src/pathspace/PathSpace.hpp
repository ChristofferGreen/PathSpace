#pragma once
#include "PathSpaceLeaf.hpp"

namespace SP {
class PathSpace {
public:
    /**
     * @brief Constructs a PathSpace object.
     * @param pool Pointer to a TaskPool for managing asynchronous operations. If nullptr, uses the global instance.
     */
    explicit PathSpace(TaskPool* pool = nullptr) : pool(pool) {};

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
        return this->inImpl(path, InputData{data}, options);
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
    auto
    inFunctionPointer(bool const isConcretePath, ConstructiblePath const& constructedPath, DataType const& data, InOptions const& options)
            -> bool {
        bool const isFunctionPointer = (InputData{data}.metadata.category == DataCategory::ExecutionFunctionPointer);
        bool const isImmediateExecution
                = (!options.execution.has_value()
                   || (options.execution.has_value() && options.execution.value().category == ExecutionOptions::Category::Immediate));
        if (isConcretePath && isFunctionPointer && isImmediateExecution) {
            if constexpr (std::is_function_v<std::remove_pointer_t<DataType>>) {
                pool->addTask({.userSuppliedFunctionPointer = reinterpret_cast<void*>(data),
                               .space = this,
                               .pathToInsertReturnValueTo = constructedPath,
                               .executionOptions = options.execution.has_value() ? options.execution.value() : ExecutionOptions{},
                               .taskExecutor = [](Task const& task) {
                                   assert(task.space);
                                   if (task.userSuppliedFunctionPointer != nullptr) {
                                       auto const fun
                                               = reinterpret_cast<std::invoke_result_t<DataType> (*)()>(task.userSuppliedFunctionPointer);
                                       task.space->insert(task.pathToInsertReturnValueTo.getPath(), fun());
                                   }
                               }});
            }
            return true;
        }
        return false;
    }

    virtual InsertReturn inImpl(GlobPathStringView const& path, InputData const& data, InOptions const& options) {
        InsertReturn ret;
        if (!path.isValid()) {
            ret.errors.emplace_back(Error::Code::InvalidPath, std::string("The path was not valid: ").append(path.getPath()));
            return ret;
        }
        bool const isConcretePath = path.isConcrete();
        auto constructedPath = isConcretePath ? ConstructiblePath{path} : ConstructiblePath{};
        if (!this->inFunctionPointer(isConcretePath, constructedPath, data, options))
            this->root.inInternal(constructedPath, path.begin(), path.end(), InputData{data}, options, ret);
        return ret;
    };

    virtual Expected<int> outImpl(ConcretePathStringView const& path,
                                  InputMetadata const& inputMetadata,
                                  OutOptions const& options,
                                  Capabilities const& capabilities,
                                  void* obj) {
        return this->root.outInternal(path.begin(), path.end(), inputMetadata, obj, options, capabilities);
    }

    TaskPool* pool;
    PathSpaceLeaf root;
    // std::function<std::unique_ptr<PathSpaceLeaf>(ConcretePathStringView const&)> leafFactory;
};

} // namespace SP