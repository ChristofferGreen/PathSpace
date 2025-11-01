#pragma once

#include "path/Iterator.hpp"
#include "path/utils.hpp"
#include "path/validation.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace SP::History {

/**
 * Copy-on-write prototype for PathSpace subtrees.
 *
 * This helper models the structural sharing we need for undo/redo snapshots.
 * Nodes are immutable once published; mutations create a fresh chain of nodes
 * along the modified path while reusing untouched branches.
 *
 * The prototype is intentionally minimal: it supports setting byte payloads at
 * concrete paths and provides instrumentation to measure memory impact.
 */
class CowSubtreePrototype {
public:
    struct Payload {
        Payload() = default;
        explicit Payload(std::vector<std::uint8_t> bytesIn);

        [[nodiscard]] auto size() const noexcept -> std::size_t {
            return bytes ? bytes->size() : 0;
        }

        std::shared_ptr<const std::vector<std::uint8_t>> bytes;
    };

    struct Node;
    using NodePtr = std::shared_ptr<const Node>;

    struct Node {
        Payload payload;
        std::map<std::string, NodePtr, std::less<>> children;
    };

    struct Snapshot {
        NodePtr      root;
        std::size_t  generation = 0;

        [[nodiscard]] auto valid() const noexcept -> bool { return static_cast<bool>(root); }
    };

    struct Mutation {
        std::vector<std::string> components;
        Payload                 payload;
    };

    struct MemoryStats {
        std::size_t uniqueNodes       = 0;
        std::size_t payloadBytes      = 0;
    };

    struct DeltaStats {
        std::size_t newNodes          = 0;
        std::size_t reusedNodes       = 0;
        std::size_t removedNodes      = 0;
        std::size_t newPayloadBytes   = 0;
        std::size_t reusedPayloadBytes = 0;
    };

    CowSubtreePrototype();

    [[nodiscard]] auto emptySnapshot() const -> Snapshot;

    [[nodiscard]] auto apply(Snapshot const& base, Mutation const& mutation) const -> Snapshot;
    [[nodiscard]] auto apply(Snapshot const& base, std::span<const Mutation> mutations) const -> Snapshot;

    [[nodiscard]] auto analyze(Snapshot const& snapshot) const -> MemoryStats;
    [[nodiscard]] auto analyzeDelta(Snapshot const& baseline, Snapshot const& updated) const -> DeltaStats;

    [[nodiscard]] static auto parsePath(std::string_view concretePath) -> std::optional<std::vector<std::string>>;

private:
    static auto applyAt(NodePtr const& node, Mutation const& mutation, std::size_t depth) -> NodePtr;
    static auto writePayload(NodePtr const& node, Mutation const& mutation, std::size_t depth) -> NodePtr;
    static auto cloneNode(NodePtr const& node) -> std::shared_ptr<Node>;

    static auto collect(NodePtr const& root) -> std::vector<NodePtr>;

    mutable std::size_t nextGeneration_;
};

} // namespace SP::History
