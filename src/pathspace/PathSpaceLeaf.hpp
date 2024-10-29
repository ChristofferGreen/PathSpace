#pragma once
#include "path/ConcretePathIterator.hpp"
#include "path/GlobPathIterator.hpp"
#include "type/NodeDataHashMap.hpp"

namespace SP {
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
            InsertReturn& ret) -> void;
    auto out(ConcretePathIteratorStringView const& iter,
             ConcretePathIteratorStringView const& end,
             InputMetadata const& inputMetadata,
             void* obj,
             OutOptions const& options,
             bool const isExtract) -> Expected<int>;

    auto clear() -> void;

private:
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

    auto outDataName(ConcreteNameStringView const& concreteName,
                     ConcretePathIteratorStringView const& nextIter,
                     ConcretePathIteratorStringView const& end,
                     InputMetadata const& inputMetadata,
                     void* obj,
                     OutOptions const& options,
                     bool const isExtract) -> Expected<int>;
    auto outConcretePathComponent(ConcretePathIteratorStringView const& nextIter,
                                  ConcretePathIteratorStringView const& end,
                                  ConcreteNameStringView const& concreteName,
                                  InputMetadata const& inputMetadata,
                                  void* obj,
                                  OutOptions const& options,
                                  bool const isExtract) -> Expected<int>;
    NodeDataHashMap nodeDataMap;
};

} // namespace SP