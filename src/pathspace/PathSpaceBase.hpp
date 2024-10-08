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
class PathSpaceBase {
public:
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
        DataType obj;
        if (auto ret = this->outImpl(path, InputMetadataT<DataType>{}, options, capabilities, &obj); !ret)
            return std::unexpected(ret.error());
        return obj;
    }

    template <typename DataType>
    auto readBlock(ConcretePathStringView const& path,
                   OutOptions const& options = {.block{{.behavior = BlockOptions::Behavior::Wait}}, .doPop = false},
                   Capabilities const& capabilities = Capabilities::All()) const -> Expected<DataType> {
        DataType obj;
        if (auto ret = this->outImpl(path, InputMetadataT<DataType>{}, options, capabilities, &obj); !ret)
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
    virtual auto inImpl(GlobPathStringView const& path, InputData const& data, InOptions const& options) -> InsertReturn {
        return {};
    }
    virtual auto outImpl(ConcretePathStringView const& path,
                         InputMetadata const& inputMetadata,
                         OutOptions const& options,
                         Capabilities const& capabilities,
                         void* obj) const -> Expected<int> {
        return {};
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
    // std::unique_ptr<Root> root; // If this is the root node we store extra data
    // Need to add blocking functionality per path
    // std::map<ConcretePath, std::mutex> pathMutexMap;
};

} // namespace SP