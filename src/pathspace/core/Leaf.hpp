#pragma once
#include "PathSpaceBase.hpp"
#include "core/Error.hpp"
#include "core/Node.hpp"
#include "core/NodeData.hpp"
#include "core/Out.hpp"
#include "task/IFutureAny.hpp"
#include <atomic>
#include <string_view>
#include <string>
#include <mutex>

namespace SP {
namespace testing {
using PackInsertReservationHook = void (*)();
inline std::atomic<PackInsertReservationHook>& packInsertReservationHook() {
    static std::atomic<PackInsertReservationHook> hook{nullptr};
    return hook;
}
inline void SetPackInsertReservationHook(PackInsertReservationHook hook) {
    packInsertReservationHook().store(hook, std::memory_order_release);
}
inline PackInsertReservationHook GetPackInsertReservationHook() {
    return packInsertReservationHook().load(std::memory_order_acquire);
}
} // namespace testing
struct Error;
struct Iterator;
struct InsertReturn;
struct InputMetadata;
struct InputData;
struct Out;
namespace History {
class UndoableSpace;
}

class Leaf {
public:
    auto in(Iterator const& iter, InputData const& inputData, InsertReturn& ret) -> void;
    auto insertSerialized(Iterator const& iter, NodeData const& payload, InsertReturn& ret) -> void;
    auto out(Iterator const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> std::optional<Error>;
    auto extractSerialized(Iterator const& iter, NodeData& payload) -> std::optional<Error>;
    auto clear() -> void;

    auto rootNode() -> Node& { return root; }
    auto rootNode() const -> Node const& { return root; }



        // Return a type-erased FutureAny handle for an execution at the given path (typed tasks).
        auto peekAnyFuture(Iterator const& iter) const -> std::optional<FutureAny>;

        // Return a weak Future-like handle for an execution at the given path.
        // If the node at the path stores an execution (task), this returns a Future
        // constructed from the front task. Otherwise, returns an empty optional.
        auto peekFuture(Iterator const& iter) const -> std::optional<Future>;

    auto spanPackConst(std::span<const std::string> paths,
                       InputMetadata const& inputMetadata,
                       Out const& options,
                       SpanPackConstCallback const& fn) const -> Expected<void>;

    auto spanPackMut(std::span<const std::string> paths,
                     InputMetadata const& inputMetadata,
                     Out const& options,
                     SpanPackMutCallback const& fn) const -> Expected<void>;

    auto packInsert(std::span<const std::string> paths,
                    InputMetadata const& inputMetadata,
                    std::span<void const* const> values) -> InsertReturn;

private:
    auto inFinalComponent(Iterator const& iter, InputData const& inputData, InsertReturn& ret) -> void;
    auto inIntermediateComponent(Iterator const& iter, InputData const& inputData, InsertReturn& ret) -> void;
    auto outIntermediateComponent(Iterator const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> std::optional<Error>;
    auto outFinalComponent(Iterator const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> std::optional<Error>;
    auto extractSerializedAtNode(Node& node, Iterator const& iter, NodeData& payload) -> std::optional<Error>;

    // Internal helpers (members for friend access to PathSpaceBase)
    auto inAtNode(Node& node,
                  Iterator const& iter,
                  InputData const& inputData,
                  InsertReturn& ret,
                  std::string const& resolvedPath) -> void;
    auto outAtNode(Node& node, Iterator const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract, Node* parent, std::string_view keyInParent) -> std::optional<Error>;
    static auto ensureNodeData(Node& n, InputData const& inputData, InsertReturn& ret) -> NodeData*;
    static void mergeInsertReturn(InsertReturn& into, InsertReturn const& from);

    Node                       root;
    mutable std::mutex         packInsertMutex;
    mutable std::atomic<bool>  packInsertSeen{false};
    friend class History::UndoableSpace;
};

} // namespace SP
