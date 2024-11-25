#pragma once
#include "path/ConcretePathIterator.hpp"
#include "path/GlobPathIterator.hpp"
#include "path/PathView.hpp"
#include "type/NodeDataHashMap.hpp"

namespace SP {
struct Error;
struct In;
struct InsertReturn;
struct InputData;
struct Out;
struct ConstructiblePath;

class PathSpaceLeaf {
public:
    auto in(PathViewGlob const& iter, InputData const& inputData, In const& options, InsertReturn& ret) -> void;
    auto out(PathViewConcrete const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> Expected<int>;
    auto clear() -> void;

private:
    auto inFinalComponent(PathViewGlob const& iter, InputData const& inputData, In const& options, InsertReturn& ret) -> void;
    auto inIntermediateComponent(PathViewGlob const& iter, InputData const& inputData, In const& options, InsertReturn& ret) -> void;
    auto outFinalComponent(PathViewConcrete const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> Expected<int>;
    auto outIntermediateComponent(PathViewConcrete const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> Expected<int>;

    NodeDataHashMap nodeDataMap;
};

} // namespace SP