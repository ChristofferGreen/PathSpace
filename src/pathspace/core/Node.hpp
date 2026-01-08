#pragma once

#include "core/NodeData.hpp"
#include "path/TransparentString.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <parallel_hashmap/phmap.h>

namespace SP {

/**
 * Unified node type for the path trie.
 *
 * Motivation:
 * - Replace the variant-based node representation with a single, explicit structure.
 * - Keep structure (children) and payload (data or nested space) clearly separated.
 * - Prepare for better concurrency semantics and easier reasoning about invariants.
 *
 * Structure:
 * - children: sub-tree keyed by the next path component
 * - data: optional payload stored at this exact node
 * - nested: optional nested PathSpaceBase anchored at this node
 *
 * Notes:
 * - Concurrency: the children map uses phmap::parallel_node_hash_map with internal sharding.
 *   Additional higher-level locking/ordering may still be desirable depending on operations.
 * - Invariants: a node may have children, and/or a payload, and/or a nested space.
 *   Higher layers should define the precise combinations they permit.
 */


struct Node final {
    // Tunable shard count for parallel_node_hash_map
    static constexpr int kDefaultSubmaps = 12;

    using ChildrenMap = phmap::parallel_node_hash_map<
        std::string,
        std::unique_ptr<Node>,
        TransparentStringHash,
        std::equal_to<>,
        std::allocator<std::pair<const std::string, std::unique_ptr<Node>>>,
        kDefaultSubmaps,
        std::mutex>;

    // Sub-tree structure
    ChildrenMap children;

    // Protects payload members; children has its own internal sharding locks.
    mutable std::mutex payloadMutex;

    // Data payload at this node (if present)
    std::unique_ptr<NodeData> data;

    Node()  = default;
    ~Node() = default;

    // Structural queries
    bool hasChildren() const noexcept { return !children.empty(); }
    bool hasData() const noexcept { return static_cast<bool>(data); }
    bool isLeaf() const noexcept { return !hasChildren(); }

    // Create or fetch a child node for the given name
    Node& getOrCreateChild(std::string_view name) {
        auto [it, inserted] = children.try_emplace(name, std::make_unique<Node>());
        return *it->second;
    }

    // Lookup a child by name (const)
    Node const* getChild(std::string_view name) const {
        Node const* result = nullptr;
        children.if_contains(name, [&](auto const& kv) { result = kv.second.get(); });
        return result;
    }

    // Lookup a child by name (mutable)
    Node* getChild(std::string_view name) {
        Node* result = nullptr;
        children.if_contains(name, [&](auto const& kv) { result = kv.second.get(); });
        return result;
    }

    // Iterate over children (const)
    template <typename Fn>
    void forEachChild(Fn&& fn) const {
        children.for_each([&](auto const& kv) { fn(std::string_view{kv.first}, *kv.second); });
    }

    // Remove a child by name; returns true if erased
    bool eraseChild(std::string_view name) {
        return children.erase(name) > 0;
    }

    // Clear payloads at this node (does not clear children)
    void clearLocal() noexcept {
        data.reset();
    }

    // Clear entire sub-tree (children and payloads)
    void clearRecursive() noexcept {
        clearLocal();
        children.clear();
    }
};

} // namespace SP
