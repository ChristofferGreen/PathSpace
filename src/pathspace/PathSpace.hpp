#pragma once

#include "core/Capabilities.hpp"
#include "core/Error.hpp"
#include "core/InsertOptions.hpp"
#include "core/InsertReturn.hpp"
#include "core/ReadOptions.hpp"
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
 * supporting operations like insert, read, and grab with path-based access.
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
        } else {
            this->insertInternal(path.begin(), path.end(), InputData{data}, options, ret);
        }
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
     * @tparam DataType The type of data to be grabbed.
     * @param path The concrete path from which to grab the data.
     * @param capabilities Capabilities controlling access to the data.
     * @return Expected<DataType> containing the grabbed data if successful, or an error if not.
     */
    template <typename DataType>
    auto grab(ConcretePathStringView const& path, Capabilities const& capabilities = Capabilities::All()) -> Expected<DataType> {
        DataType obj;
        if (auto ret = this->grabInternal(path.begin(), path.end(), InputMetadataT<DataType>{}, &obj, capabilities); !ret) {
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
    /**
     * @brief Internal method for inserting data into the PathSpace.
     *
     * @param iter Iterator to the current path component.
     * @param end Iterator to the end of the path.
     * @param inputData The data to be inserted.
     * @param options Insertion options.
     * @param ret InsertReturn object to store the result of the operation.
     */
    auto insertInternal(GlobPathIteratorStringView const& iter, GlobPathIteratorStringView const& end, InputData const& inputData, InsertOptions const& options, InsertReturn& ret)
            -> void;

    /**
     * @brief Inserts data at the final component of a path.
     *
     * @param pathComponent The final path component.
     * @param inputData The data to be inserted.
     * @param options Insertion options.
     * @param ret InsertReturn object to store the result of the operation.
     */
    auto insertFinalComponent(GlobName const& pathComponent, InputData const& inputData, InsertOptions const& options, InsertReturn& ret) -> void;

    /**
     * @brief Inserts data at an intermediate component of a path.
     *
     * @param iter Iterator to the current path component.
     * @param end Iterator to the end of the path.
     * @param pathComponent The current path component.
     * @param inputData The data to be inserted.
     * @param options Insertion options.
     * @param ret InsertReturn object to store the result of the operation.
     */
    auto insertIntermediateComponent(GlobPathIteratorStringView const& iter,
                                     GlobPathIteratorStringView const& end,
                                     GlobName const& pathComponent,
                                     InputData const& inputData,
                                     InsertOptions const& options,
                                     InsertReturn& ret) -> void;

    /**
     * @brief Inserts data at a concrete (non-glob) path component.
     *
     * @param concreteName The concrete path component.
     * @param inputData The data to be inserted.
     * @param options Insertion options.
     * @param ret InsertReturn object to store the result of the operation.
     */
    auto insertConcreteDataName(ConcreteName const& concreteName, InputData const& inputData, InsertOptions const& options, InsertReturn& ret) -> void;

    /**
     * @brief Inserts data at a glob path component.
     *
     * @param globName The glob path component.
     * @param inputData The data to be inserted.
     * @param options Insertion options.
     * @param ret InsertReturn object to store the result of the operation.
     */
    auto insertGlobDataName(GlobName const& globName, InputData const& inputData, InsertOptions const& options, InsertReturn& ret) -> void;

    /**
     * @brief Inserts data at a glob path component for intermediate path components.
     *
     * @param iter Iterator to the current path component.
     * @param end Iterator to the end of the path.
     * @param globName The glob path component.
     * @param inputData The data to be inserted.
     * @param options Insertion options.
     * @param ret InsertReturn object to store the result of the operation.
     */
    auto insertGlobPathComponent(GlobPathIteratorStringView const& iter,
                                 GlobPathIteratorStringView const& end,
                                 GlobName const& globName,
                                 InputData const& inputData,
                                 InsertOptions const& options,
                                 InsertReturn& ret) -> void;

    /**
     * @brief Inserts data at a concrete path component for intermediate path components.
     *
     * @param iter Iterator to the current path component.
     * @param end Iterator to the end of the path.
     * @param concreteName The concrete path component.
     * @param inputData The data to be inserted.
     * @param options Insertion options.
     * @param ret InsertReturn object to store the result of the operation.
     */
    auto insertConcretePathComponent(GlobPathIteratorStringView const& iter,
                                     GlobPathIteratorStringView const& end,
                                     ConcreteName const& concreteName,
                                     InputData const& inputData,
                                     InsertOptions const& options,
                                     InsertReturn& ret) -> void;

    /**
     * @brief Internal method for reading data from the PathSpace.
     *
     * @param iter Iterator to the current path component.
     * @param end Iterator to the end of the path.
     * @param inputMetadata Metadata about the input type.
     * @param obj Pointer to the object where the read data will be stored.
     * @param options Read options.
     * @return Expected<int> indicating success or an error.
     */
    auto readInternal(ConcretePathIteratorStringView const& iter,
                      ConcretePathIteratorStringView const& end,
                      InputMetadata const& inputMetadata,
                      void* obj,
                      ReadOptions const& options) const -> Expected<int>;

    /**
     * @brief Reads data from a concrete path component.
     *
     * @param concreteName The concrete path component.
     * @param nextIter Iterator to the next path component.
     * @param end Iterator to the end of the path.
     * @param inputMetadata Metadata about the input type.
     * @param obj Pointer to the object where the read data will be stored.
     * @param options Read options.
     * @return Expected<int> indicating success or an error.
     */
    auto readDataName(ConcreteName const& concreteName,
                      ConcretePathIteratorStringView const& nextIter,
                      ConcretePathIteratorStringView const& end,
                      InputMetadata const& inputMetadata,
                      void* obj,
                      ReadOptions const& options) const -> Expected<int>;

    /**
     * @brief Reads data from a concrete path component for intermediate path components.
     *
     * @param nextIter Iterator to the next path component.
     * @param end Iterator to the end of the path.
     * @param concreteName The concrete path component.
     * @param inputMetadata Metadata about the input type.
     * @param obj Pointer to the object where the read data will be stored.
     * @param options Read options.
     * @return Expected<int> indicating success or an error.
     */
    auto readConcretePathComponent(ConcretePathIteratorStringView const& nextIter,
                                   ConcretePathIteratorStringView const& end,
                                   ConcreteName const& concreteName,
                                   InputMetadata const& inputMetadata,
                                   void* obj,
                                   ReadOptions const& options) const -> Expected<int>;

    /**
     * @brief Internal method for grabbing (reading and removing) data from the PathSpace.
     *
     * @param iter Iterator to the current path component.
     * @param end Iterator to the end of the path.
     * @param inputMetadata Metadata about the input type.
     * @param obj Pointer to the object where the grabbed data will be stored.
     * @param capabilities Capabilities controlling access to the data.
     * @return Expected<int> indicating success or an error.
     */
    auto grabInternal(ConcretePathIteratorStringView const& iter,
                      ConcretePathIteratorStringView const& end,
                      InputMetadata const& inputMetadata,
                      void* obj,
                      Capabilities const& capabilities) -> Expected<int>;

    /**
     * @brief Grabs data from a concrete path component.
     *
     * @param concreteName The concrete path component.
     * @param nextIter Iterator to the next path component.
     * @param end Iterator to the end of the path.
     * @param inputMetadata Metadata about the input type.
     * @param obj Pointer to the object where the grabbed data will be stored.
     * @param capabilities Capabilities controlling access to the data.
     * @return Expected<int> indicating success or an error.
     */
    auto grabDataName(ConcreteName const& concreteName,
                      ConcretePathIteratorStringView const& nextIter,
                      ConcretePathIteratorStringView const& end,
                      InputMetadata const& inputMetadata,
                      void* obj,
                      Capabilities const& capabilities) -> Expected<int>;

    /**
     * @brief Grabs data from a concrete path component for intermediate path components.
     *
     * @param nextIter Iterator to the next path component.
     * @param end Iterator to the end of the path.
     * @param concreteName The concrete path component.
     * @param inputMetadata Metadata about the input type.
     * @param obj Pointer to the object where the grabbed data will be stored.
     * @param capabilities Capabilities controlling access to the data.
     * @return Expected<int> indicating success or an error.
     */
    auto grabConcretePathComponent(ConcretePathIteratorStringView const& nextIter,
                                   ConcretePathIteratorStringView const& end,
                                   ConcreteName const& concreteName,
                                   InputMetadata const& inputMetadata,
                                   void* obj,
                                   Capabilities const& capabilities) -> Expected<int>;

    /**
     * @brief The main data structure holding the PathSpace hierarchy.
     *
     * This hash map stores the hierarchical structure of the PathSpace,
     * with ConcreteName as keys and either NodeData or nested PathSpaces as values.
     */
    NodeDataHashMap nodeDataMap;

    /**
     * @brief Pointer to the TaskPool used for managing asynchronous operations.
     *
     * This TaskPool is used for executing asynchronous tasks related to the PathSpace,
     * such as delayed insertions or background processing.
     */
    TaskPool* pool;
};

} // namespace SP