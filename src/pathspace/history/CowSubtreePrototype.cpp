#include "CowSubtreePrototype.hpp"

#include <algorithm>

namespace SP::History {

CowSubtreePrototype::Payload::Payload(std::vector<std::byte> bytesIn)
    : bytes(std::make_shared<const std::vector<std::byte>>(std::move(bytesIn))) {}

CowSubtreePrototype::CowSubtreePrototype() : nextGeneration_(1) {}

auto CowSubtreePrototype::emptySnapshot() const -> Snapshot {
    Snapshot snap;
    snap.generation = 0;
    return snap;
}

auto CowSubtreePrototype::apply(Snapshot const& base, Mutation const& mutation) const -> Snapshot {
    return apply(base, std::span<const Mutation>(&mutation, 1));
}

auto CowSubtreePrototype::apply(Snapshot const& base, std::span<const Mutation> mutations) const -> Snapshot {
    NodePtr result = base.root;
    for (auto const& mut : mutations) {
        result = applyAt(result, mut, 0);
    }
    Snapshot snap;
    snap.root       = std::move(result);
    snap.generation = nextGeneration_++;
    return snap;
}

auto CowSubtreePrototype::analyze(Snapshot const& snapshot) const -> MemoryStats {
    MemoryStats stats;
    if (!snapshot.root) {
        return stats;
    }

    std::unordered_set<Node const*> visited;
    std::vector<NodePtr> stack;
    stack.push_back(snapshot.root);

    while (!stack.empty()) {
        NodePtr current = stack.back();
        stack.pop_back();
        auto raw = current.get();
        if (!raw || !visited.insert(raw).second) {
            continue;
        }
        stats.uniqueNodes++;
        stats.payloadBytes += current->payload.size();
        for (auto const& child : current->children) {
            if (child.second) {
                stack.push_back(child.second);
            }
        }
    }
    return stats;
}

auto CowSubtreePrototype::analyzeDelta(Snapshot const& baseline, Snapshot const& updated) const -> DeltaStats {
    DeltaStats stats;

    auto const baselineNodes = collect(baseline.root);
    auto const updatedNodes  = collect(updated.root);

    std::unordered_set<Node const*> baselineSet;
    baselineSet.reserve(baselineNodes.size());
    for (auto const& node : baselineNodes) {
        baselineSet.insert(node.get());
    }

    std::unordered_set<Node const*> updatedSet;
    updatedSet.reserve(updatedNodes.size());
    for (auto const& node : updatedNodes) {
        auto raw = node.get();
        updatedSet.insert(raw);
        if (baselineSet.contains(raw)) {
            stats.reusedNodes++;
            stats.reusedPayloadBytes += node->payload.size();
        } else {
            stats.newNodes++;
            stats.newPayloadBytes += node->payload.size();
        }
    }

    for (auto const& node : baselineNodes) {
        if (!updatedSet.contains(node.get())) {
            stats.removedNodes++;
        }
    }

    return stats;
}

auto CowSubtreePrototype::parsePath(std::string_view concretePath) -> std::optional<std::vector<std::string>> {
    Iterator iter{concretePath};
    if (auto error = iter.validate(ValidationLevel::Full)) {
        (void)error;
        return std::nullopt;
    }

    std::vector<std::string> components;
    auto const pathView = iter.toStringView();
    if (pathView.size() <= 1) {
        return components;
    }

    std::size_t start = 1; // skip leading '/'
    for (std::size_t i = 1; i <= pathView.size(); ++i) {
        if (i == pathView.size() || pathView[i] == '/') {
            auto componentView = pathView.substr(start, i - start);
            if (!componentView.empty()) {
                if (is_glob(componentView)) {
                    return std::nullopt;
                }
                components.emplace_back(componentView);
            }
            start = i + 1;
        }
    }

    return components;
}

auto CowSubtreePrototype::applyAt(NodePtr const& node, Mutation const& mutation, std::size_t depth) -> NodePtr {
    if (depth >= mutation.components.size()) {
        return writePayload(node, mutation, depth);
    }
    auto mutableNode = cloneNode(node);
    auto const& key  = mutation.components[depth];

    NodePtr child;
    if (node) {
        auto it = node->children.find(key);
        if (it != node->children.end()) {
            child = it->second;
        }
    }

    auto updatedChild = applyAt(child, mutation, depth + 1);
    mutableNode->children[key] = std::move(updatedChild);
    return std::static_pointer_cast<const Node>(mutableNode);
}

auto CowSubtreePrototype::writePayload(NodePtr const& node, Mutation const& mutation, std::size_t /*depth*/) -> NodePtr {
    auto mutableNode = cloneNode(node);
    mutableNode->payload = mutation.payload;
    return std::static_pointer_cast<const Node>(mutableNode);
}

auto CowSubtreePrototype::cloneNode(NodePtr const& node) -> std::shared_ptr<Node> {
    if (node) {
        return std::make_shared<Node>(*node);
    }
    return std::make_shared<Node>();
}

auto CowSubtreePrototype::collect(NodePtr const& root) -> std::vector<NodePtr> {
    std::vector<NodePtr> nodes;
    if (!root) {
        return nodes;
    }
    std::unordered_set<Node const*> visited;
    std::vector<NodePtr> stack;
    stack.push_back(root);
    while (!stack.empty()) {
        NodePtr current = stack.back();
        stack.pop_back();
        auto raw = current.get();
        if (!raw || !visited.insert(raw).second) {
            continue;
        }
        nodes.push_back(current);
        for (auto const& [_, child] : current->children) {
            if (child) {
                stack.push_back(child);
            }
        }
    }
    return nodes;
}

void CowSubtreePrototype::setNextGeneration(std::size_t next) {
    nextGeneration_ = next;
}

auto CowSubtreePrototype::nextGeneration() const noexcept -> std::size_t {
    return nextGeneration_;
}

} // namespace SP::History
