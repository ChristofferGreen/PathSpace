#pragma once

#include "core/Capabilities.hpp"
#include "core/Error.hpp"
#include "core/ExtractOptions.hpp"
#include "core/InsertOptions.hpp"
#include "core/InsertReturn.hpp"
#include "core/ReadOptions.hpp"
#include "path/ConstructiblePath.hpp"
#include "taskpool/TaskPool.hpp"
#include "type/Helper.hpp"
#include "type/InputData.hpp"

#include <functional>
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
    auto insert(GlobPathStringView const& path, DataType const& data, InsertOptions const& options = {}) -> InsertReturn {
        InsertReturn ret;
        if (!path.isValid()) {
            ret.errors.emplace_back(Error::Code::InvalidPath, std::string("The path was not valid: ").append(path.getPath()));
            return ret;
        }
        bool const isConcretePath = path.isConcrete();
        auto constructedPath = isConcretePath
                                       ? ConstructiblePath{path}
                                       : ConstructiblePath{};
        if (!this->insertFunctionPointer(isConcretePath, constructedPath, data, options))
            this->insertInternal(constructedPath, path.begin(), path.end(), InputData{data}, options, ret);
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
    auto read(ConcretePathStringView const& path, ReadOptions const& options = {}) const -> Expected<DataType> {
        DataType obj;
        if (auto ret = this->readInternal(path.begin(), path.end(), InputMetadataT<DataType>{}, &obj, options); !ret) {
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
    auto extract(ConcretePathStringView const& path, ExtractOptions const& options = {}, Capabilities const& capabilities = Capabilities::All()) -> Expected<DataType> {
        DataType obj;
        if (auto ret = this->extractInternal(path.begin(), path.end(), InputMetadataT<DataType>{}, &obj, options, capabilities); !ret) {
            return std::unexpected(ret.error());
        }
        return obj;
    }

    /**
     * @brief Subscribes to changes at the specified path.
     *
     * @param path The glob-style path to subscribe to.
     * @param callback Function to be called when changes occur.
     * @param capabilities Capabilities controlling access to the data.
     * @return Expected<void> indicating success or an error.
     */
    auto
    subscribe(GlobPathStringView const& path, std::function<void(const GlobPathStringView&)> callback, Capabilities const& capabilities = Capabilities::All()) -> Expected<void>;

    /**
     * @brief Visits and potentially modifies data at the specified path.
     *
     * @tparam DataType The type of data to be visited.
     * @param path The glob-style path to visit.
     * @param visitor Function to be called for each matching item.
     * @param capabilities Capabilities controlling access to the data.
     * @return Expected<void> indicating success or an error.
     */
    template <typename DataType>
    auto visit(GlobPathStringView const& path, std::function<void(DataType&)> visitor, Capabilities const& capabilities = Capabilities::All()) -> Expected<void>;

    /**
     * @brief Converts the PathSpace to a JSON representation.
     *
     * @param isHumanReadable If true, formats the JSON for human readability.
     * @return String containing the JSON representation of the PathSpace.
     */
    auto toJSON(bool const isHumanReadable) const -> std::string;

    /**
     * @brief Serializes the PathSpace.
     *
     * @tparam Archive The type of archive to serialize to.
     * @param ar The archive object to serialize to.
     */
    template <class Archive>
    void serialize(Archive& ar) {
        ar(this->nodeDataMap);
    }

private:
    template <typename DataType>
    auto insertFunctionPointer(bool const isConcretePath, ConstructiblePath const& constructedPath, DataType const& data, InsertOptions const& options) -> bool {
        bool const isFunctionPointer = (InputData{data}.metadata.category == DataCategory::ExecutionFunctionPointer);
        bool const isImmediateExecution = (options.execution.has_value() && options.execution.value().category == ExecutionOptions::Category::Immediate);
        if (isConcretePath && isFunctionPointer && isImmediateExecution) {
            FunctionPointerTask task = [](PathSpace* space, ConstructiblePath const& path, ExecutionOptions const& executionOptions, void* userSuppliedFunction) {
                assert(userSuppliedFunction != nullptr);
                if constexpr (std::is_function_v<std::remove_pointer_t<DataType>>) {
                    space->insert(path.getPath(), reinterpret_cast<std::invoke_result_t<DataType> (*)()>(userSuppliedFunction)());
                }
            };
            void* fun = nullptr;
            if constexpr (std::is_function_v<std::remove_pointer_t<decltype(data)>>) {
                fun = static_cast<void*>(data);
            }
            pool->addTask({
                    .userSuppliedFunction = fun,
                    .wrapperFunction = task,
                    .space = this,
                    .path = constructedPath,
                    .executionOptions = options.execution.has_value() ? options.execution.value() : ExecutionOptions{},
            });
            return true;
        }
        return false;
    }
    auto insertInternal(ConstructiblePath& path,
                        GlobPathIteratorStringView const& iter,
                        GlobPathIteratorStringView const& end,
                        InputData const& inputData,
                        InsertOptions const& options,
                        InsertReturn& ret) -> void;
    auto insertFinalComponent(ConstructiblePath& path, GlobName const& pathComponent, InputData const& inputData, InsertOptions const& options, InsertReturn& ret) -> void;
    auto insertIntermediateComponent(ConstructiblePath& path,
                                     GlobPathIteratorStringView const& iter,
                                     GlobPathIteratorStringView const& end,
                                     GlobName const& pathComponent,
                                     InputData const& inputData,
                                     InsertOptions const& options,
                                     InsertReturn& ret) -> void;
    auto
    insertConcreteDataName(ConstructiblePath& path, ConcreteName const& concreteName, InputData const& inputData, InsertOptions const& options, InsertReturn& ret) -> void;
    auto insertGlobDataName(ConstructiblePath& path, GlobName const& globName, InputData const& inputData, InsertOptions const& options, InsertReturn& ret) -> void;
    auto insertGlobPathComponent(ConstructiblePath& path,
                                 GlobPathIteratorStringView const& iter,
                                 GlobPathIteratorStringView const& end,
                                 GlobName const& globName,
                                 InputData const& inputData,
                                 InsertOptions const& options,
                                 InsertReturn& ret) -> void;
    auto insertConcretePathComponent(ConstructiblePath& path,
                                     GlobPathIteratorStringView const& iter,
                                     GlobPathIteratorStringView const& end,
                                     ConcreteName const& concreteName,
                                     InputData const& inputData,
                                     InsertOptions const& options,
                                     InsertReturn& ret) -> void;
    auto readInternal(ConcretePathIteratorStringView const& iter,
                      ConcretePathIteratorStringView const& end,
                      InputMetadata const& inputMetadata,
                      void* obj,
                      ReadOptions const& options) const -> Expected<int>;
    auto readDataName(ConcreteName const& concreteName,
                      ConcretePathIteratorStringView const& nextIter,
                      ConcretePathIteratorStringView const& end,
                      InputMetadata const& inputMetadata,
                      void* obj,
                      ReadOptions const& options) const -> Expected<int>;
    auto readConcretePathComponent(ConcretePathIteratorStringView const& nextIter,
                                   ConcretePathIteratorStringView const& end,
                                   ConcreteName const& concreteName,
                                   InputMetadata const& inputMetadata,
                                   void* obj,
                                   ReadOptions const& options) const -> Expected<int>;
    auto extractInternal(ConcretePathIteratorStringView const& iter,
                         ConcretePathIteratorStringView const& end,
                         InputMetadata const& inputMetadata,
                         void* obj,
                         ExtractOptions const& options,
                         Capabilities const& capabilities) -> Expected<int>;
    auto extractDataName(ConcreteName const& concreteName,
                         ConcretePathIteratorStringView const& nextIter,
                         ConcretePathIteratorStringView const& end,
                         InputMetadata const& inputMetadata,
                         void* obj,
                         ExtractOptions const& options,
                         Capabilities const& capabilities) -> Expected<int>;
    auto extractConcretePathComponent(ConcretePathIteratorStringView const& nextIter,
                                      ConcretePathIteratorStringView const& end,
                                      ConcreteName const& concreteName,
                                      InputMetadata const& inputMetadata,
                                      void* obj,
                                      ExtractOptions const& options,
                                      Capabilities const& capabilities) -> Expected<int>;
    NodeDataHashMap nodeDataMap;
    TaskPool* pool;
};

} // namespace SP