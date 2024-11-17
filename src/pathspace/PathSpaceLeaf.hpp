#pragma once
#include "path/ConcretePathIterator.hpp"
#include "path/GlobPathIterator.hpp"
#include "path/PathView.hpp"
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
    auto in(PathViewGlob const& iter, InputData const& inputData, InOptions const& options, InsertReturn& ret) -> void;
    auto out(PathViewConcrete const& iter, InputMetadata const& inputMetadata, void* obj, OutOptions const& options, bool const isExtract) -> Expected<int>;
    auto clear() -> void;

private:
    auto inFinalComponent(PathViewGlob const& iter, InputData const& inputData, InOptions const& options, InsertReturn& ret) -> void;
    auto inIntermediateComponent(PathViewGlob const& iter, GlobName const& pathComponent, InputData const& inputData, InOptions const& options, InsertReturn& ret) -> void;
    auto outFinalComponent(PathViewConcrete const& iter, InputMetadata const& inputMetadata, void* obj, OutOptions const& options, bool const isExtract) -> Expected<int>;
    auto outIntermediateComponent(PathViewConcrete const& iter, InputMetadata const& inputMetadata, void* obj, OutOptions const& options, bool const isExtract) -> Expected<int>;

    NodeDataHashMap nodeDataMap;
};

} // namespace SP