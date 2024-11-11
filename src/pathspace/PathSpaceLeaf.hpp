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
    auto in(GlobPathIteratorStringView const& iter,
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

    auto getLeafNode(ConcretePathIteratorStringView const& iter, ConcretePathIteratorStringView const& end) -> Expected<PathSpaceLeaf*>;
    auto getLeafNode(ConcretePathIteratorStringView const& iter, ConcretePathIteratorStringView const& end) const
            -> Expected<PathSpaceLeaf const*>;

private:
    auto inFinalComponent(GlobName const& pathComponent, InputData const& inputData, InOptions const& options, InsertReturn& ret) -> void;
    auto inIntermediateComponent(GlobPathIteratorStringView const& iter,
                                 GlobPathIteratorStringView const& end,
                                 GlobName const& pathComponent,
                                 InputData const& inputData,
                                 InOptions const& options,
                                 InsertReturn& ret) -> void;
    auto outDataName(ConcreteNameStringView const& concreteName,
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
    auto getLeafNodeImpl(ConcretePathIteratorStringView const& iter, ConcretePathIteratorStringView const& end) const
            -> Expected<PathSpaceLeaf*>;

    NodeDataHashMap nodeDataMap;
};

} // namespace SP