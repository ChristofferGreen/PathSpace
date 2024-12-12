#pragma once
#include "path/Iterator.hpp"
#include "type/NodeDataHashMap.hpp"

namespace SP {
struct Error;
struct InsertReturn;
struct InputData;

class PathSpaceLeaf {
public:
    auto in(Iterator const& iter, InputData const& inputData, InsertReturn& ret) -> void;
    auto out(Iterator const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> std::optional<Error>;
    auto clear() -> void;

private:
    auto inFinalComponent(Iterator const& iter, InputData const& inputData, InsertReturn& ret) -> void;
    auto inIntermediateComponent(Iterator const& iter, InputData const& inputData, InsertReturn& ret) -> void;
    auto outFinalComponent(Iterator const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> std::optional<Error>;
    auto outIntermediateComponent(Iterator const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> std::optional<Error>;

    NodeDataHashMap nodeDataMap;
};

} // namespace SP