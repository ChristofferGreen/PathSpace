#pragma once
#include "path/PathIterator.hpp"
#include "type/NodeDataHashMap.hpp"

namespace SP {
struct Error;
struct InsertReturn;
struct InputData;

class PathSpaceLeaf {
public:
    auto in(PathIterator const& iter, InputData const& inputData, InsertReturn& ret) -> void;
    auto out(PathIterator const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> std::optional<Error>;
    auto clear() -> void;

private:
    auto inFinalComponent(PathIterator const& iter, InputData const& inputData, InsertReturn& ret) -> void;
    auto inIntermediateComponent(PathIterator const& iter, InputData const& inputData, InsertReturn& ret) -> void;
    auto outFinalComponent(PathIterator const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> std::optional<Error>;
    auto outIntermediateComponent(PathIterator const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> std::optional<Error>;

    NodeDataHashMap nodeDataMap;
};

} // namespace SP