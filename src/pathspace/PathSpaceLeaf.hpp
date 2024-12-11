#pragma once
#include "path/PathIterator.hpp"
#include "path/PathView.hpp"
#include "type/NodeDataHashMap.hpp"

namespace SP {
struct Error;
struct InsertReturn;
struct InputData;
struct ConstructiblePath;

class PathSpaceLeaf {
public:
    auto in(PathIterator const& iter, InputData const& inputData, InsertReturn& ret) -> void;
    auto out(PathViewGlob const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> std::optional<Error>;
    auto out(PathIterator const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> std::optional<Error>;
    auto clear() -> void;

private:
    auto inFinalComponent(PathIterator const& iter, InputData const& inputData, InsertReturn& ret) -> void;
    auto inIntermediateComponent(PathIterator const& iter, InputData const& inputData, InsertReturn& ret) -> void;
    auto outFinalComponent(PathViewGlob const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> std::optional<Error>;
    auto outFinalComponent(PathIterator const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> std::optional<Error>;
    auto outIntermediateComponent(PathViewGlob const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> std::optional<Error>;
    auto outIntermediateComponent(PathIterator const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> std::optional<Error>;

    NodeDataHashMap nodeDataMap;
};

} // namespace SP