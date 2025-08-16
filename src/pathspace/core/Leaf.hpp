#pragma once
#include "core/Node.hpp"
#include "task/IFutureAny.hpp"
#include <string_view>

namespace SP {
struct Error;
struct Iterator;
struct InsertReturn;
struct InputMetadata;
struct InputData;

class Leaf {
public:
    auto in(Iterator const& iter, InputData const& inputData, InsertReturn& ret) -> void;
    auto out(Iterator const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> std::optional<Error>;
    auto clear() -> void;



        // Return a type-erased FutureAny handle for an execution at the given path (typed tasks).
        auto peekAnyFuture(Iterator const& iter) const -> std::optional<FutureAny>;

        // Return a weak Future-like handle for an execution at the given path.
        // If the node at the path stores an execution (task), this returns a Future
        // constructed from the front task. Otherwise, returns an empty optional.
        auto peekFuture(Iterator const& iter) const -> std::optional<Future>;

private:
    auto inFinalComponent(Iterator const& iter, InputData const& inputData, InsertReturn& ret) -> void;
    auto inIntermediateComponent(Iterator const& iter, InputData const& inputData, InsertReturn& ret) -> void;
    auto outIntermediateComponent(Iterator const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> std::optional<Error>;
    auto outFinalComponent(Iterator const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> std::optional<Error>;

    // Internal helpers (members for friend access to PathSpaceBase)
    auto inAtNode(Node& node, Iterator const& iter, InputData const& inputData, InsertReturn& ret) -> void;
    auto outAtNode(Node& node, Iterator const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract, Node* parent, std::string_view keyInParent) -> std::optional<Error>;
    static auto ensureNodeData(Node& n, InputData const& inputData, InsertReturn& ret) -> NodeData*;
    static void mergeInsertReturn(InsertReturn& into, InsertReturn const& from);

    Node root;
};

} // namespace SP