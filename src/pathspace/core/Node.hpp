#pragma once

#include "core/NodeData.hpp"
#include "core/PodPayload.hpp"
#include "path/TransparentString.hpp"

#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
    static constexpr int DefaultSubmaps = 12;

    using ChildrenMap = phmap::parallel_node_hash_map<
        std::string,
        std::unique_ptr<Node>,
        TransparentStringHash,
        std::equal_to<>,
        std::allocator<std::pair<const std::string, std::unique_ptr<Node>>>,
        DefaultSubmaps,
        std::mutex>;

    // Sub-tree structure
    ChildrenMap children;

    // Protects payload members; children has its own internal sharding locks.
    mutable std::mutex payloadMutex;

    // Data payload at this node (if present)
    std::unique_ptr<NodeData> data;
    std::shared_ptr<PodPayloadBase> podPayload;

    Node()  = default;
    ~Node() = default;

    static void* operator new(std::size_t size) {
        if (size != sizeof(Node)) {
            return ::operator new(size);
        }
        auto& pool = nodePool();
        {
            std::lock_guard<std::mutex> guard(pool.mutex);
            if (!pool.freeList.empty()) {
                void* ptr = pool.freeList.back();
                pool.freeList.pop_back();
                return ptr;
            }
        }
        return ::operator new(size);
    }

    static void operator delete(void* ptr) noexcept {
        if (!ptr) {
            return;
        }
        auto& pool = nodePool();
        {
            std::lock_guard<std::mutex> guard(pool.mutex);
            pool.freeList.push_back(ptr);
        }
    }

    static void operator delete(void* ptr, std::size_t size) noexcept {
        if (!ptr) {
            return;
        }
        if (size != sizeof(Node)) {
            ::operator delete(ptr);
            return;
        }
        auto& pool = nodePool();
        {
            std::lock_guard<std::mutex> guard(pool.mutex);
            pool.freeList.push_back(ptr);
        }
    }

    // Structural queries
    bool hasChildren() const noexcept { return !children.empty(); }
    bool hasData() const noexcept { return static_cast<bool>(data) || static_cast<bool>(podPayload); }
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
        podPayload.reset();
    }

    // Clear entire sub-tree (children and payloads)
    void clearRecursive() noexcept {
        clearLocal();
        children.clear();
    }

private:
    struct NodePool {
        std::mutex        mutex;
        std::vector<void*> freeList;
    };

    static auto nodePool() -> NodePool& {
        static NodePool pool{};
        return pool;
    }
};

} // namespace SP
