#include "PathSpaceBase.hpp"

#include "core/Node.hpp"
#include "core/PodPayload.hpp"
#include "type/DataCategory.hpp"
#include "path/utils.hpp"

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <optional>

namespace SP {

struct ValueHandle::Impl {
    Impl(Node const* owner, PathSpaceBase const* space, std::string pathIn)
        : node(owner)
        , path(std::move(pathIn))
        , space(space) {}
    Node const*            node  = nullptr;
    std::string            path;
    PathSpaceBase const*   space = nullptr;
};

template <typename Fn>
using SpanRawConstFn = std::function<void(void const*, std::size_t)>;

namespace VisitDetail {

auto Access::MakeHandle(PathSpaceBase const& owner, Node const& node, std::string const& path, bool includeValues) -> ValueHandle {
    return owner.makeValueHandle(node, path, includeValues);
}

auto Access::SerializeNodeData(ValueHandle const& handle) -> std::optional<std::vector<std::byte>> {
    if (!handle.impl_ || !handle.impl_->node) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> guard(handle.impl_->node->payloadMutex);
    if (handle.impl_->node->data) {
        return handle.impl_->node->data->serializeSnapshot();
    }
    if (handle.impl_->node->podPayload) {
        auto payload = handle.impl_->node->podPayload;
        std::optional<std::vector<std::byte>> snapshotBytes;
        NodeData tmp;
        bool ok = true;
        auto const& meta = payload->podMetadata();
        auto elemSize    = payload->elementSize();
        auto err = payload->withSpanRaw([&](void const* data, std::size_t count) {
            auto* base = static_cast<std::byte const*>(data);
            for (std::size_t i = 0; i < count; ++i) {
                InputData in{base + i * elemSize, meta};
                if (auto e = tmp.serialize(in)) {
                    ok = false;
                    return;
                }
            }
        });
        if (!err.has_value() && ok) {
            snapshotBytes = tmp.serializeSnapshot();
        }
        if (snapshotBytes.has_value()) {
            return snapshotBytes;
        }
    }
    return std::nullopt;
}

} // namespace VisitDetail

namespace {

constexpr auto kUnlimitedDepth = std::numeric_limits<std::size_t>::max();

struct VisitStart {
    Node*        node      = nullptr;
    std::string  path      = "/";
    std::size_t  depth     = 0;
};

struct NodeCapture {
    PathEntry            entry;
    ValueHandle          handle;
    std::vector<std::string> children;
};

struct VisitState {
    PathSpaceBase& owner;
    PathVisitor const& visitor;
    VisitOptions const& options;
    std::size_t baseDepth = 0;
    std::optional<std::size_t> rootIndex;
    bool stopRequested    = false;
};

auto toCanonicalRoot(std::string const& root) -> std::string {
    if (root.empty()) {
        return "/";
    }
    ConcretePathString canonicalIn{root};
    auto               canonical = canonicalIn.canonicalized();
    if (!canonical) {
        return "/";
    }
    return canonical->getPath();
}

auto splitComponents(std::string const& canonicalPath) -> Expected<std::vector<std::string>> {
    ConcretePathStringView view{canonicalPath};
    auto                   components = view.components();
    if (!components) {
        return std::unexpected(components.error());
    }
    return components.value();
}

auto depthForPath(std::string const& canonicalPath) -> std::size_t {
    if (canonicalPath == "/") {
        return 0;
    }
    std::size_t depth = 0;
    for (auto ch : canonicalPath) {
        if (ch == '/') {
            ++depth;
        }
    }
    return depth;
}

auto joinChildPath(std::string const& parent, std::string const& child) -> std::string {
    if (parent == "/") {
        std::string result{"/"};
        result.append(child);
        return result;
    }
    std::string result = parent;
    if (!result.empty() && result.back() != '/') {
        result.push_back('/');
    }
    result.append(child);
    return result;
}

auto buildSubPath(std::vector<std::string> const& components, std::size_t startIndex) -> std::string {
    if (startIndex >= components.size()) {
        return "/";
    }
    std::string result;
    for (std::size_t idx = startIndex; idx < components.size(); ++idx) {
        result.push_back('/');
        result.append(components[idx]);
    }
    if (result.empty()) {
        return "/";
    }
    return result;
}

auto gatherChildren(Node const& node) -> std::vector<std::string> {
    std::vector<std::string> names;
    node.children.for_each([&](auto const& kv) { names.emplace_back(kv.first); });
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

auto snapshotNode(PathSpaceBase& owner, Node& node, std::string const& path, VisitOptions const& options) -> NodeCapture {
    NodeCapture capture{};
    capture.entry.path = path;
    capture.children   = gatherChildren(node);
    capture.entry.approxChildCount = capture.children.size();
    capture.entry.hasChildren      = !capture.children.empty();

    {
        std::lock_guard<std::mutex> guard(node.payloadMutex);
        capture.entry.hasNestedSpace = node.data && node.data->hasNestedSpaces();
        if (node.data) {
            capture.entry.hasValue = true;
            auto const& summary    = node.data->typeSummary();
            if (!summary.empty()) {
                capture.entry.frontCategory = summary.front().category;
            }
        } else if (node.podPayload) {
            capture.entry.hasValue        = true;
            capture.entry.frontCategory   = DataCategory::Fundamental;
        }
    }

    capture.handle = VisitDetail::Access::MakeHandle(owner, node, path, options.includeValues);
    return capture;
}

auto appendNestedPath(std::string const& prefix, std::string const& nestedPath) -> std::string {
    if (nestedPath == "/") {
        return prefix;
    }
    if (prefix == "/") {
        return nestedPath;
    }
    std::string result = prefix;
    if (!result.empty() && result.back() != '/') {
        result.push_back('/');
    }
    // nestedPath always begins with '/'
    result.append(nestedPath.begin() + 1, nestedPath.end());
    return result;
}

auto remainingDepthBudget(std::size_t baseDepth, std::size_t nodeDepth, VisitOptions const& options) -> std::optional<std::size_t> {
    if (options.maxDepth == kUnlimitedDepth) {
        return kUnlimitedDepth;
    }
    auto relativeDepth = nodeDepth - baseDepth;
    if (relativeDepth >= options.maxDepth) {
        return std::nullopt;
    }
    return options.maxDepth - relativeDepth;
}

auto visitNestedSpace(Node& node,
                      std::string const& path,
                      std::size_t depth,
                      VisitState& state) -> Expected<void> {
    if (!state.options.includeNestedSpaces) {
        return {};
    }

    std::vector<std::pair<std::shared_ptr<PathSpaceBase>, std::size_t>> nestedSpaces;
    {
        std::unique_lock<std::mutex> guard(node.payloadMutex);
        if (!node.data || !node.data->hasNestedSpaces()) {
            return {};
        }
        auto count = node.data->nestedCount();
        nestedSpaces.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            if (state.rootIndex && depth == state.baseDepth && *state.rootIndex != i) {
                continue;
            }
            if (auto nested = node.data->borrowNestedShared(i)) {
                nestedSpaces.emplace_back(std::move(nested), i);
            }
        }
    }

    auto budget = remainingDepthBudget(state.baseDepth, depth, state.options);
    if (!budget.has_value()) {
        return {};
    }

    VisitOptions nestedOptions = state.options;
    nestedOptions.root        = "/";
    nestedOptions.maxDepth    = budget.value();

    for (auto const& entry : nestedSpaces) {
        if (state.stopRequested) {
            break;
        }
        auto const& nestedHolder = entry.first;
        auto* nested             = nestedHolder.get();
        auto idx                 = entry.second;
        auto nestedVisitor = [&](PathEntry const& entry, ValueHandle& handle) -> VisitControl {
            if (state.stopRequested) {
                return VisitControl::Stop;
            }
            if (entry.path == "/") {
                return VisitControl::Continue;
            }
            PathEntry remapped = entry;
            std::string basePath = path;
            if (!(state.rootIndex && depth == state.baseDepth)) {
                basePath = append_index_suffix(path, idx);
            }
            remapped.path      = appendNestedPath(basePath, entry.path);
            auto control       = state.visitor(remapped, handle);
            if (control == VisitControl::Stop) {
                state.stopRequested = true;
            }
            return control;
        };

        auto result = nested->visit(nestedVisitor, nestedOptions);
        if (!result) {
            return result;
        }
    }
    return {};
}

auto pushChildren(Node& node,
                  std::string const& path,
                  std::size_t depth,
                  std::vector<VisitStart>& stack,
                  VisitOptions const& options) -> void {
    auto names = gatherChildren(node);
    std::size_t toTake = names.size();
    if (options.childLimitEnabled() && options.maxChildren < toTake) {
        toTake = options.maxChildren;
    }
    for (std::size_t idx = toTake; idx > 0; --idx) {
        auto const& name = names[idx - 1];
        Node* child = node.getChild(name);
        if (!child) {
            continue;
        }
        stack.push_back(VisitStart{child, joinChildPath(path, name), depth + 1});
    }
}

auto walkSubtree(VisitStart const& start, VisitState& state) -> Expected<void> {
    std::vector<VisitStart> stack;
    stack.push_back(start);

    while (!stack.empty()) {
        if (state.stopRequested) {
            break;
        }
        auto current = stack.back();
        stack.pop_back();

        if (!current.node) {
            continue;
        }

        auto relativeDepth = current.depth >= state.baseDepth ? current.depth - state.baseDepth : 0;
        if (state.options.maxDepth != kUnlimitedDepth && relativeDepth > state.options.maxDepth) {
            continue;
        }

        auto capture = snapshotNode(state.owner, *current.node, current.path, state.options);
        auto control = state.visitor(capture.entry, capture.handle);
        if (control == VisitControl::Stop) {
            state.stopRequested = true;
            break;
        }
        if (control == VisitControl::SkipChildren || (state.options.maxDepth != kUnlimitedDepth && relativeDepth == state.options.maxDepth)) {
            continue;
        }

        pushChildren(*current.node, current.path, current.depth, stack, state.options);

        auto nestedResult = visitNestedSpace(*current.node, current.path, current.depth, state);
        if (!nestedResult) {
            return nestedResult;
        }
    }

    return {};
}

auto resolveStart(PathSpaceBase& space,
                  Node& rootNode,
                  std::string const& canonicalRoot,
                  std::vector<std::string> const& components,
                  VisitOptions const& options,
                  VisitState& state) -> Expected<std::optional<VisitStart>> {
    if (components.empty()) {
        return VisitStart{&rootNode, canonicalRoot, depthForPath(canonicalRoot)};
    }

    Node*       current    = &rootNode;
    std::string currentPath = "/";
    std::size_t currentDepth = 0;

    for (std::size_t idx = 0; idx < components.size(); ++idx) {
        auto parsed = parse_indexed_component(components[idx]);
        if (parsed.malformed) {
            return std::unexpected(Error{Error::Code::InvalidPath, "Malformed indexed path component"});
        }
        auto* child = current->getChild(parsed.base);
        if (!child) {
            std::shared_ptr<PathSpaceBase> nested;
            {
                std::unique_lock<std::mutex> guard(current->payloadMutex);
                if (current->data && state.options.includeNestedSpaces) {
                    nested = current->data->borrowNestedShared(parsed.index.value_or(0));
                }
            }
            if (nested) {
                VisitOptions nestedOptions = options;
                nestedOptions.root         = buildSubPath(components, idx);
                auto nestedVisitor = [&](PathEntry const& entry, ValueHandle& handle) -> VisitControl {
                    if (state.stopRequested) {
                        return VisitControl::Stop;
                    }
                    PathEntry remapped = entry;
                    auto basePath      = append_index_suffix(currentPath, parsed.index.value_or(0));
                    remapped.path      = appendNestedPath(basePath, entry.path);
                    auto control       = state.visitor(remapped, handle);
                    if (control == VisitControl::Stop) {
                        state.stopRequested = true;
                    }
                    return control;
                };
                auto result = nested->visit(nestedVisitor, nestedOptions);
                if (!result) {
                    return std::unexpected(result.error());
                }
                return std::optional<VisitStart>{};
            }
            if (parsed.index.has_value()) {
                return std::unexpected(Error{Error::Code::NoSuchPath, "visit root not found"});
            }
            return std::unexpected(Error{Error::Code::NoSuchPath, "visit root not found"});
        }

        current     = child;
        currentPath = joinChildPath(currentPath, components[idx]);
        currentDepth = idx + 1;

        bool finalComponent = (idx + 1 == components.size());
        std::shared_ptr<PathSpaceBase> nested;
        {
            std::unique_lock<std::mutex> guard(child->payloadMutex);
            if (child->data) {
                nested = child->data->borrowNestedShared(parsed.index.value_or(0));
            }
        }
        if (nested && !finalComponent) {
            if (!state.options.includeNestedSpaces) {
                return std::unexpected(Error{Error::Code::NoSuchPath, "visit root not found"});
            }
            VisitOptions nestedOptions = options;
            nestedOptions.root         = buildSubPath(components, idx + 1);
            auto nestedVisitor = [&](PathEntry const& entry, ValueHandle& handle) -> VisitControl {
                if (state.stopRequested) {
                    return VisitControl::Stop;
                }
                if (entry.path == "/") {
                    return VisitControl::Continue;
                }
                PathEntry remapped = entry;
                remapped.path      = appendNestedPath(append_index_suffix(currentPath, parsed.index.value_or(0)), entry.path);
                auto control       = state.visitor(remapped, handle);
                if (control == VisitControl::Stop) {
                    state.stopRequested = true;
                }
                return control;
            };

            auto result = nested->visit(nestedVisitor, nestedOptions);
            if (!result) {
                return std::unexpected(result.error());
            }
            return std::optional<VisitStart>{};
        }
        if (!nested && parsed.index.has_value()) {
            return std::unexpected(Error{Error::Code::NoSuchPath, "visit root not found"});
        }
    }

    return VisitStart{current, currentPath, currentDepth};
}

} // namespace

auto ValueHandle::queueDepth() const -> std::size_t {
    if (!impl_ || !impl_->node) {
        return 0;
    }
    std::unique_lock<std::mutex> guard(impl_->node->payloadMutex);
    if (impl_->node->data) {
        return impl_->node->data->typeSummary().size();
    }
    if (impl_->node->podPayload) {
        return impl_->node->podPayload->size();
    }
    return 0;
}

auto ValueHandle::readInto(void* destination, InputMetadata const& metadata) const -> std::optional<Error> {
    if (!this->hasValues()) {
        return Error{Error::Code::NotSupported, "Value sampling disabled for this visit"};
    }
    if (!impl_ || !impl_->node) {
        return Error{Error::Code::UnknownError, "ValueHandle missing node"};
    }
    std::unique_lock<std::mutex> guard(impl_->node->payloadMutex);
    if (impl_->node->data) {
        return impl_->node->data->deserialize(destination, metadata);
    }
    if (impl_->node->podPayload && metadata.typeInfo) {
        auto payload = impl_->node->podPayload;
        if (!payload->matches(*metadata.typeInfo)) {
            return Error{Error::Code::TypeMismatch, "POD fast path type mismatch"};
        }
        if (auto err = payload->readTo(destination)) {
            // Fallback: try snapshotting the front element without pop.
            bool fixed = false;
            payload->withSpanRaw([&](void const* data, std::size_t count) {
                if (count == 0 || fixed) return;
                auto elemSize = payload->elementSize();
                std::memcpy(destination, data, std::min(elemSize, payload->elementSize()));
                fixed = true;
            });
            if (!fixed) {
                // Fallback: snapshot via NodeData
                NodeData tmp;
                auto elemSize = payload->elementSize();
                auto const& meta = payload->podMetadata();
                auto spanErr = payload->withSpanRaw([&](void const* data, std::size_t count) {
                    auto* base = static_cast<std::byte const*>(data);
                    for (std::size_t i = 0; i < count; ++i) {
                        InputData in{base + i * elemSize, meta};
                        if (auto e = tmp.serialize(in)) {
                            err = e;
                            return;
                        }
                    }
                });
                if (spanErr.has_value()) {
                    return spanErr;
                }
                if (auto deser = tmp.deserialize(destination, metadata)) {
                    return deser;
                }
                return std::nullopt;
            }
            return err;
        }
        return std::nullopt;
    }
    if (impl_->space) {
        guard.unlock();
        if (auto bytes = VisitDetail::Access::SerializeNodeData(*this)) {
            if (auto restored = NodeData::deserializeSnapshot(std::span<const std::byte>{bytes->data(), bytes->size()})) {
                return restored->deserialize(destination, metadata);
            }
        }
    }
    return Error{Error::Code::NoObjectFound, "No value present at node"};
}

auto ValueHandle::snapshot() const -> Expected<ValueSnapshot> {
    if (!impl_ || !impl_->node) {
        return std::unexpected(Error{Error::Code::UnknownError, "ValueHandle missing node"});
    }
    std::lock_guard<std::mutex> guard(impl_->node->payloadMutex);
    ValueSnapshot snapshot{};
    if (impl_->node->data) {
        snapshot.queueDepth = impl_->node->data->typeSummary().size();
        snapshot.types.assign(impl_->node->data->typeSummary().begin(), impl_->node->data->typeSummary().end());
        snapshot.hasExecutionPayload  = impl_->node->data->hasExecutionPayload();
        snapshot.hasSerializedPayload = !impl_->node->data->rawBuffer().empty();
        snapshot.rawBufferBytes       = impl_->node->data->rawBuffer().size();
        return snapshot;
    }
    if (impl_->node->podPayload) {
        auto const& ti = impl_->node->podPayload->type();
        auto const& meta = impl_->node->podPayload->podMetadata();
        ElementType et;
        et.typeInfo = &ti;
        et.category = meta.dataCategory;
        et.elements = 1;
        snapshot.queueDepth = impl_->node->podPayload->size();
        snapshot.types.assign(snapshot.queueDepth, et);
        snapshot.hasExecutionPayload  = false;
        snapshot.hasSerializedPayload = false;
        snapshot.rawBufferBytes       = 0;
        return snapshot;
    }
    return snapshot;
}

auto PathSpaceBase::makeValueHandle(Node const& node, std::string path, bool includeValues) const -> ValueHandle {
    auto storage = std::make_shared<ValueHandle::Impl>(&node, this, std::move(path));
    return ValueHandle(std::move(storage), includeValues);
}

auto PathSpaceBase::visit(PathVisitor const& visitor, VisitOptions const& options) -> Expected<void> {
    if (!visitor) {
        return std::unexpected(Error{Error::Code::InvalidType, "Visitor callback is empty"});
    }

    Node* rootNode = this->getRootNode();
    if (!rootNode) {
        return std::unexpected(Error{Error::Code::NotSupported, "This space does not expose a node trie"});
    }

    auto canonicalRoot = toCanonicalRoot(options.root);
    auto components    = splitComponents(canonicalRoot);
    if (!components) {
        return std::unexpected(components.error());
    }

    VisitState state{*this, visitor, options, depthForPath(canonicalRoot)};
    if (!components->empty()) {
        auto parsedRoot   = parse_indexed_component(components->back());
        if (parsedRoot.malformed) {
            return std::unexpected(Error{Error::Code::InvalidPath, "Malformed indexed path component"});
        }
        state.rootIndex = parsedRoot.index;
    }
    auto       resolved = resolveStart(*this, *rootNode, canonicalRoot, *components, options, state);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    if (!resolved->has_value()) {
        return {};
    }

    state.baseDepth = resolved->value().depth;
    auto walkResult = walkSubtree(resolved->value(), state);
    if (!walkResult) {
        return walkResult;
    }
    return {};
}

} // namespace SP
