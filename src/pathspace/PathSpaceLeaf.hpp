#pragma once

#include "core/Capabilities.hpp"
#include "core/Error.hpp"
#include "core/InOptions.hpp"
#include "core/InsertReturn.hpp"
#include "core/OutOptions.hpp"
#include "path/ConstructiblePath.hpp"
#include "type/Helper.hpp"
#include "type/InputData.hpp"

namespace SP {

class PathSpaceLeaf {
public:
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
};

} // namespace SP