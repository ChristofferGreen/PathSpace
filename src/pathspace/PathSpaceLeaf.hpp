#pragma once
#include "type/Helper.hpp"
#include "utils/WaitEntry.hpp"

namespace SP {
struct Capabilities;
struct Error;
struct InOptions;
struct InsertReturn;
struct InputData;
struct OutOptions;
struct ConstructiblePath;

class PathSpaceLeaf {
public:
    auto in(ConstructiblePath& path,
            GlobPathIteratorStringView const& iter,
            GlobPathIteratorStringView const& end,
            InputData const& inputData,
            InOptions const& options,
            InsertReturn& ret,
            WaitMap& waitMap) -> void;
    auto out(ConcretePathIteratorStringView const& iter,
             ConcretePathIteratorStringView const& end,
             InputMetadata const& inputMetadata,
             void* obj,
             OutOptions const& options,
             Capabilities const& capabilities,
             WaitMap& waitMap) -> Expected<int>;

private:
    auto inFinalComponent(ConstructiblePath& path,
                          GlobName const& pathComponent,
                          InputData const& inputData,
                          InOptions const& options,
                          InsertReturn& ret,
                          WaitMap& waitMap) -> void;
    auto inIntermediateComponent(ConstructiblePath& path,
                                 GlobPathIteratorStringView const& iter,
                                 GlobPathIteratorStringView const& end,
                                 GlobName const& pathComponent,
                                 InputData const& inputData,
                                 InOptions const& options,
                                 InsertReturn& ret,
                                 WaitMap& waitMap) -> void;
    auto inConcreteDataName(ConstructiblePath& path,
                            ConcreteName const& concreteName,
                            InputData const& inputData,
                            InOptions const& options,
                            InsertReturn& ret,
                            WaitMap& waitMap) -> void;
    auto inGlobDataName(ConstructiblePath& path,
                        GlobName const& globName,
                        InputData const& inputData,
                        InOptions const& options,
                        InsertReturn& ret,
                        WaitMap& waitMap) -> void;
    auto inGlobPathComponent(ConstructiblePath& path,
                             GlobPathIteratorStringView const& iter,
                             GlobPathIteratorStringView const& end,
                             GlobName const& globName,
                             InputData const& inputData,
                             InOptions const& options,
                             InsertReturn& ret,
                             WaitMap& waitMap) -> void;
    auto inConcretePathComponent(ConstructiblePath& path,
                                 GlobPathIteratorStringView const& iter,
                                 GlobPathIteratorStringView const& end,
                                 ConcreteName const& concreteName,
                                 InputData const& inputData,
                                 InOptions const& options,
                                 InsertReturn& ret,
                                 WaitMap& waitMap) -> void;

    auto outDataName(ConcreteName const& concreteName,
                     ConcretePathIteratorStringView const& nextIter,
                     ConcretePathIteratorStringView const& end,
                     InputMetadata const& inputMetadata,
                     void* obj,
                     OutOptions const& options,
                     Capabilities const& capabilities,
                     WaitMap& waitMap) -> Expected<int>;
    auto outConcretePathComponent(ConcretePathIteratorStringView const& nextIter,
                                  ConcretePathIteratorStringView const& end,
                                  ConcreteName const& concreteName,
                                  InputMetadata const& inputMetadata,
                                  void* obj,
                                  OutOptions const& options,
                                  Capabilities const& capabilities,
                                  WaitMap& waitMap) -> Expected<int>;
    NodeDataHashMap nodeDataMap;
};

} // namespace SP