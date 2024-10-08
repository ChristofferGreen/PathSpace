#pragma once

#include "core/Capabilities.hpp"
#include "core/Error.hpp"
#include "core/InOptions.hpp"
#include "core/InsertReturn.hpp"
#include "core/OutOptions.hpp"
#include "path/ConstructiblePath.hpp"
#include "taskpool/TaskPool.hpp"
#include "type/Helper.hpp"
#include "type/InputData.hpp"

#include <string>

namespace SP {
/**
 * @class PathSpace
 * @brief Represents a hierarchical space for storing and managing data.
 *
 * PathSpace provides a tree-like structure for organizing and accessing data,
 * supporting operations like insert, read, and extract with path-based access.
 * It allows for flexible data storage and retrieval using glob-style paths.
 *
 * The class is designed to be thread-safe and efficient, using a TaskPool
 * for managing asynchronous operations. It supports various data types,
 * including user-defined serializable classes and lambda functions.
 */
class PathSpace {
public:
    /**
     * @brief Constructs a PathSpace object.
     * @param pool Pointer to a TaskPool for managing asynchronous operations. If nullptr, uses the global instance.
     */
    explicit PathSpace(TaskPool* pool = nullptr);

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
        InsertReturn ret;
        if (!path.isValid()) {
            ret.errors.emplace_back(Error::Code::InvalidPath, std::string("The path was not valid: ").append(path.getPath()));
            return ret;
        }
        bool const isConcretePath = path.isConcrete();
        auto constructedPath = isConcretePath ? ConstructiblePath{path} : ConstructiblePath{};
        if (!this->inFunctionPointer(isConcretePath, constructedPath, data, options))
            this->inInternal(constructedPath, path.begin(), path.end(), InputData{data}, options, ret);
        return ret;
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
        DataType obj;
        // ToDo: Make sure options.doPop is set to false
        if (auto ret
            = const_cast<PathSpace*>(this)->outInternal(path.begin(), path.end(), InputMetadataT<DataType>{}, &obj, options, capabilities);
            !ret) {
            return std::unexpected(ret.error());
        }
        return obj;
    }

    template <typename DataType>
    auto readBlock(ConcretePathStringView const& path,
                   OutOptions const& options = {.block{{.behavior = BlockOptions::Behavior::Wait}}, .doPop = false},
                   Capabilities const& capabilities = Capabilities::All()) const -> Expected<DataType> {
        DataType obj;
        // ToDo: Make sure options.doPop is set to false
        if (auto ret
            = const_cast<PathSpace*>(this)->outInternal(path.begin(), path.end(), InputMetadataT<DataType>{}, &obj, options, capabilities);
            !ret) {
            return std::unexpected(ret.error());
        }
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
        if (auto ret = this->outInternal(path.begin(), path.end(), InputMetadataT<DataType>{}, &obj, options, capabilities); !ret) {
            return std::unexpected(ret.error());
        }
        return obj;
    }

    template <typename DataType>
    auto extractBlock(ConcretePathStringView const& path,
                      OutOptions const& options = {.block{{.behavior = BlockOptions::Behavior::Wait}}},
                      Capabilities const& capabilities = Capabilities::All()) -> Expected<DataType> {
        DataType obj;
        if (auto ret = this->outInternal(path.begin(), path.end(), InputMetadataT<DataType>{}, &obj, options, capabilities); !ret) {
            return std::unexpected(ret.error());
        }
        return obj;
    }

private:
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
    auto inInternal(ConstructiblePath& path,
                    GlobPathIteratorStringView const& iter,
                    GlobPathIteratorStringView const& end,
                    InputData const& inputData,
                    InOptions const& options,
                    InsertReturn& ret) -> void;
    auto inFinalComponent(ConstructiblePath& path,
                          GlobName const& pathComponent,
                          InputData const& inputData,
                          InOptions const& options,
                          InsertReturn& ret) -> void;
    auto inIntermediateComponent(ConstructiblePath& path,
                                 GlobPathIteratorStringView const& iter,
                                 GlobPathIteratorStringView const& end,
                                 GlobName const& pathComponent,
                                 InputData const& inputData,
                                 InOptions const& options,
                                 InsertReturn& ret) -> void;
    auto inConcreteDataName(ConstructiblePath& path,
                            ConcreteName const& concreteName,
                            InputData const& inputData,
                            InOptions const& options,
                            InsertReturn& ret) -> void;
    auto inGlobDataName(ConstructiblePath& path,
                        GlobName const& globName,
                        InputData const& inputData,
                        InOptions const& options,
                        InsertReturn& ret) -> void;
    auto inGlobPathComponent(ConstructiblePath& path,
                             GlobPathIteratorStringView const& iter,
                             GlobPathIteratorStringView const& end,
                             GlobName const& globName,
                             InputData const& inputData,
                             InOptions const& options,
                             InsertReturn& ret) -> void;
    auto inConcretePathComponent(ConstructiblePath& path,
                                 GlobPathIteratorStringView const& iter,
                                 GlobPathIteratorStringView const& end,
                                 ConcreteName const& concreteName,
                                 InputData const& inputData,
                                 InOptions const& options,
                                 InsertReturn& ret) -> void;
    auto outInternal(ConcretePathIteratorStringView const& iter,
                     ConcretePathIteratorStringView const& end,
                     InputMetadata const& inputMetadata,
                     void* obj,
                     OutOptions const& options,
                     Capabilities const& capabilities) -> Expected<int>;
    auto outDataName(ConcreteName const& concreteName,
                     ConcretePathIteratorStringView const& nextIter,
                     ConcretePathIteratorStringView const& end,
                     InputMetadata const& inputMetadata,
                     void* obj,
                     OutOptions const& options,
                     Capabilities const& capabilities) -> Expected<int>;
    auto outConcretePathComponent(ConcretePathIteratorStringView const& nextIter,
                                  ConcretePathIteratorStringView const& end,
                                  ConcreteName const& concreteName,
                                  InputMetadata const& inputMetadata,
                                  void* obj,
                                  OutOptions const& options,
                                  Capabilities const& capabilities) -> Expected<int>;
    NodeDataHashMap nodeDataMap;
    TaskPool* pool;
    // std::unique_ptr<Root> root; // If this is the root node we store extra data
    // Need to add blocking functionality per path
    // std::map<ConcretePath, std::mutex> pathMutexMap;
};

} // namespace SP