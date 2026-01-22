#include "Leaf.hpp"
#include "PathSpaceBase.hpp"
#include "PathSpace.hpp"

#include "core/Error.hpp"
#include "core/InsertReturn.hpp"
#include "core/PodPayload.hpp"
#include "path/Iterator.hpp"
#include "path/utils.hpp"
#include "type/InputData.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace SP {

// Forward declarations of internal helpers (defined below)
static auto ensureNodeData(Node& n, InputData const& inputData, InsertReturn& ret) -> NodeData*;
static void mergeInsertReturn(InsertReturn& into, InsertReturn const& from);
static auto inAtNode(Node& node,
                     Iterator const& iter,
                     InputData const& inputData,
                     InsertReturn& ret,
                     std::string const& resolvedPath) -> void;
static auto outAtNode(Node& node,
                      Iterator const& iter,
                      InputMetadata const& inputMetadata,
                      void* obj,
                      bool const doExtract,
                      Node* parent,
                      std::string_view keyInParent) -> std::optional<Error>;
static auto extractNestedSpace(Node& node,
                              InputMetadata const& inputMetadata,
                              void* obj,
                              bool const doExtract,
                              std::optional<std::size_t> index) -> std::optional<Error>;
static auto insertSerializedAtNode(Node& node,
                                  Iterator const& iter,
                                  NodeData const& payload,
                                  InsertReturn& ret,
                                  std::string const& resolvedPath) -> void;
static auto migratePodToNodeData(Node& node, InsertReturn& ret) -> bool;

auto Leaf::ensureNodeData(Node& n, InputData const& inputData, InsertReturn& ret) -> NodeData* {
    if (!migratePodToNodeData(n, ret)) {
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lg(n.payloadMutex);
        if (!n.data) {
            auto data = std::make_unique<NodeData>();
            if (auto err = data->serialize(inputData); err.has_value()) {
                ret.errors.emplace_back(*err);
                return nullptr;
            }
            n.data = std::move(data);
        } else {
            if (auto err = n.data->serialize(inputData); err.has_value()) {
                ret.errors.emplace_back(err.value());
                return nullptr;
            }
        }
    }
    return n.data.get();
}

void Leaf::mergeInsertReturn(InsertReturn& into, InsertReturn const& from) {
    into.nbrValuesInserted += from.nbrValuesInserted;
    into.nbrSpacesInserted += from.nbrSpacesInserted;
    into.nbrTasksInserted  += from.nbrTasksInserted;
    into.nbrValuesSuppressed += from.nbrValuesSuppressed;
    if (!from.retargets.empty()) {
        into.retargets.insert(into.retargets.end(), from.retargets.begin(), from.retargets.end());
    }
    if (!from.errors.empty()) {
        into.errors.insert(into.errors.end(), from.errors.begin(), from.errors.end());
    }
}

namespace {

auto appendPayload(Node& node, NodeData const& payload, InsertReturn& ret) -> bool {
    std::lock_guard<std::mutex> guard(node.payloadMutex);
    if (node.podPayload) {
        ret.errors.emplace_back(Error{Error::Code::InvalidType, "Node already holds POD fast-path payload"});
        return false;
    }
    if (!node.data) {
        node.data = std::make_unique<NodeData>(payload);
    } else {
        if (auto error = node.data->append(payload); error.has_value()) {
            ret.errors.emplace_back(error.value());
            return false;
        }
    }
    ret.nbrValuesInserted += payload.valueCount();
    return true;
}

auto buildResolvedPath(std::string const& prefix, std::string_view component) -> std::string {
    if (prefix.empty() || prefix == "/") {
        return std::string("/") + std::string(component);
    }
    std::string result = prefix;
    if (result.back() != '/') {
        result.push_back('/');
    }
    result.append(component.begin(), component.end());
    return result;
}

auto joinMountPrefix(std::string const& parent, std::string const& child) -> std::string {
    if (parent.empty() || parent == "/") {
        return child;
    }
    if (child.empty() || child == "/") {
        return parent;
    }
    std::string result = parent;
    if (!result.empty() && result.back() == '/') {
        result.pop_back();
    }
    result.append(child);
    return result;
}

auto rebaseRetargets(InsertReturn& ret, std::string const& mountPrefix) -> void {
    for (auto& req : ret.retargets) {
        req.mountPrefix = joinMountPrefix(mountPrefix, req.mountPrefix);
    }
}

static auto ensurePodPayload(Node& node, InputData const& inputData) -> std::shared_ptr<PodPayloadBase> {
    std::lock_guard<std::mutex> lg(node.payloadMutex);
    if (node.data) {
        return {};
    }
    if (node.podPayload) {
        if (node.podPayload->matches(*inputData.metadata.typeInfo)) {
            return node.podPayload;
        }
        return {};
    }
    if (!inputData.metadata.createPodPayload) {
        return {};
    }
    auto payload    = inputData.metadata.createPodPayload();
    node.podPayload = payload;
    return payload;
}

static auto tryPodInsert(Node& node, InputData const& inputData, InsertReturn& ret) -> std::optional<Error> {
    if (!inputData.metadata.podPreferred || inputData.task || inputData.metadata.dataCategory == DataCategory::UniquePtr) {
        return Error{Error::Code::NotSupported, "POD fast path not applicable"};
    }

    std::shared_ptr<PodPayloadBase> mismatchPayload;
    {
        std::lock_guard<std::mutex> lg(node.payloadMutex);
        if (node.data) {
            return Error{Error::Code::NotSupported, "Node already holds generic payload"};
        }
        if (node.podPayload && !node.podPayload->matches(*inputData.metadata.typeInfo)) {
            mismatchPayload = node.podPayload;
        }
    }

    if (mismatchPayload) {
        if (!migratePodToNodeData(node, ret)) {
            return Error{Error::Code::InvalidType, "Failed to upgrade POD payload to generic storage"};
        }
        return Error{Error::Code::NotSupported, "Upgraded POD fast path to generic payload"};
    }

    auto payload = ensurePodPayload(node, inputData);
    if (!payload) {
        return Error{Error::Code::InvalidType, "POD fast path unavailable for this node"};
    }
    if (!inputData.obj) {
        return Error{Error::Code::InvalidType, "Input pointer missing"};
    }
    if (!payload->pushValue(inputData.obj)) {
        return Error{Error::Code::NotSupported, "POD fast path temporarily unavailable"};
    }
    ret.nbrValuesInserted += 1;
    return std::nullopt;
}

static auto clearNodePayload(Node& node) -> std::size_t {
    std::lock_guard<std::mutex> lg(node.payloadMutex);
    std::size_t removed = 0;
    if (node.data) {
        removed += node.data->valueCount();
        node.data.reset();
    }
    if (node.podPayload) {
        removed += node.podPayload->size();
        node.podPayload.reset();
    }
    return removed;
}

static auto tryPodRead(Node& node, InputMetadata const& inputMetadata, void* obj, bool doExtract) -> std::optional<Error> {
    if (!inputMetadata.podPreferred || inputMetadata.dataCategory == DataCategory::UniquePtr) {
        return Error{Error::Code::NotSupported, "POD fast path not applicable"};
    }
    auto payload = node.podPayload;
    if (!payload) {
        return Error{Error::Code::InvalidType, "POD fast path unavailable"};
    }
    if (!payload->matches(*inputMetadata.typeInfo)) {
        return Error{Error::Code::InvalidType, "POD fast path type mismatch or unavailable"};
    }
    return doExtract ? payload->takeTo(obj) : payload->readTo(obj);
}

template <typename Fn>
static auto tryPodSpan(Node const& node, InputMetadata const& inputMetadata, Fn&& fn) -> std::optional<Error> {
    if (!inputMetadata.podPreferred) {
        return Error{Error::Code::NotSupported, "POD span not applicable"};
    }
    auto payload = node.podPayload;
    if (!payload) {
        return Error{Error::Code::InvalidType, "POD fast path unavailable"};
    }
    if (!payload->matches(*inputMetadata.typeInfo)) {
        return Error{Error::Code::InvalidType, "POD fast path type mismatch or unavailable"};
    }
    return payload->withSpanRaw([&](void const* data, std::size_t count) { fn(data, count); });
}

template <typename Fn>
static auto tryPodSpanMutable(Node& node, InputMetadata const& inputMetadata, Fn&& fn) -> std::optional<Error> {
    if (!inputMetadata.podPreferred) {
        return Error{Error::Code::NotSupported, "POD span not applicable"};
    }
    auto payload = node.podPayload;
    if (!payload) {
        return Error{Error::Code::InvalidType, "POD fast path unavailable"};
    }
    if (!payload->matches(*inputMetadata.typeInfo)) {
        return Error{Error::Code::InvalidType, "POD fast path type mismatch or unavailable"};
    }
    return payload->withSpanMutableRaw([&](void* data, std::size_t count) { fn(data, count); });
}

template <typename Fn>
static auto tryPodSpanMutableFrom(Node& node, InputMetadata const& inputMetadata, std::size_t startIndex, Fn&& fn) -> std::optional<Error> {
    if (!inputMetadata.podPreferred) {
        return Error{Error::Code::NotSupported, "POD span not applicable"};
    }
    auto payload = node.podPayload;
    if (!payload) {
        return Error{Error::Code::InvalidType, "POD fast path unavailable"};
    }
    if (!payload->matches(*inputMetadata.typeInfo)) {
        return Error{Error::Code::InvalidType, "POD fast path type mismatch or unavailable"};
    }
    return payload->withSpanMutableRawFrom(startIndex, [&](void* data, std::size_t count) { fn(data, count); });
}

template <typename Fn>
static auto tryPodSpanPinned(Node const& node, InputMetadata const& inputMetadata, Fn&& fn) -> std::optional<Error> {
    if (!inputMetadata.podPreferred) {
        return Error{Error::Code::NotSupported, "POD span not applicable"};
    }
    auto payload = node.podPayload;
    if (!payload) {
        return Error{Error::Code::InvalidType, "POD fast path unavailable"};
    }
    if (!payload->matches(*inputMetadata.typeInfo)) {
        return Error{Error::Code::InvalidType, "POD fast path type mismatch or unavailable"};
    }
    return payload->withSpanRawPinned([&](void const* data, std::size_t count, std::shared_ptr<void> const& keeper) {
        fn(data, count, keeper);
    });
}

template <typename Fn>
static auto tryPodSpanMutableFromPinned(Node& node, InputMetadata const& inputMetadata, std::size_t startIndex, Fn&& fn) -> std::optional<Error> {
    if (!inputMetadata.podPreferred) {
        return Error{Error::Code::NotSupported, "POD span not applicable"};
    }
    auto payload = node.podPayload;
    if (!payload) {
        return Error{Error::Code::InvalidType, "POD fast path unavailable"};
    }
    if (!payload->matches(*inputMetadata.typeInfo)) {
        return Error{Error::Code::InvalidType, "POD fast path type mismatch or unavailable"};
    }
    return payload->withSpanMutableRawFromPinned(
        startIndex, [&](void* data, std::size_t count, std::shared_ptr<void> const& keeper) { fn(data, count, keeper); });
}

static auto resolveConcreteNode(Node& root, std::string const& pathStr) -> Expected<Node*> {
    if (pathStr.empty() || pathStr.front() != '/') {
        return std::unexpected(Error{Error::Code::InvalidPath, "Path must start with '/'"});
    }
    Node* current = &root;
    std::size_t pos = 1;
    while (pos < pathStr.size()) {
        auto next = pathStr.find('/', pos);
        std::size_t len = (next == std::string::npos) ? pathStr.size() - pos : next - pos;
        if (len == 0) {
            pos = (next == std::string::npos) ? pathStr.size() : next + 1;
            continue;
        }
        std::string_view name{pathStr.data() + pos, len};
        if (is_glob(name) || name.find('[') != std::string_view::npos) {
            return std::unexpected(Error{Error::Code::InvalidPath, "Span pack requires concrete non-indexed paths"});
        }
        auto* child = current->getChild(name);
        if (!child) {
            return std::unexpected(Error{Error::Code::NoSuchPath, "Path not found"});
        }
        current = child;
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return current;
}

} // namespace

auto migratePodToNodeData(Node& node, InsertReturn& ret) -> bool {
    std::unique_lock<std::mutex> lg(node.payloadMutex);
    if (!node.podPayload) {
        return true;
    }
    auto payload = node.podPayload;
    payload->freezeForUpgrade();
    auto const& meta      = payload->podMetadata();
    auto        elemBytes = payload->elementSize();
    std::vector<std::byte> buffer(elemBytes);

    auto data = std::make_unique<NodeData>();
    for (;;) {
        auto err = payload->takeTo(buffer.data());
        if (err) {
            if (err->code == Error::Code::NoObjectFound) {
                break;
            }
            lg.unlock();
            ret.errors.emplace_back(*err);
            return false;
        }
        InputData copy{buffer.data(), meta};
        if (auto ser = data->serialize(copy)) {
            lg.unlock();
            ret.errors.emplace_back(*ser);
            return false;
        }
    }

    if (!node.data) {
        node.data = std::move(data);
    } else {
        if (auto err = node.data->append(*data)) {
            lg.unlock();
            ret.errors.emplace_back(*err);
            return false;
        }
    }
    node.podPayload.reset();
    lg.unlock();
    return true;
}

auto Leaf::insertSerialized(Iterator const& iter,
                            NodeData const& payload,
                            InsertReturn&   ret) -> void {
    if (payload.valueCount() == 0) {
        return;
    }
    if (auto error = iter.validate(ValidationLevel::Full)) {
        ret.errors.emplace_back(*error);
        return;
    }
    if (iter.isAtEnd() || iter.currentComponent().empty()) {
        appendPayload(root, payload, ret);
        return;
    }
    insertSerializedAtNode(root, iter, payload, ret, "/");
}

static auto insertSerializedAtNode(Node& node,
                                  Iterator const& iter,
                                  NodeData const& payload,
                                  InsertReturn& ret,
                                  std::string const& resolvedPath) -> void {
    auto component = iter.currentComponent();
    bool const final = iter.isAtFinalComponent();

    if (component.empty()) {
        appendPayload(node, payload, ret);
        return;
    }
    if (is_glob(component)) {
        ret.errors.emplace_back(Error::Code::InvalidPath,
                                "Serialized inserts do not support glob paths");
        return;
    }

    Node& child = node.getOrCreateChild(component);
    if (final) {
        appendPayload(child, payload, ret);
        return;
    }

    std::shared_ptr<PathSpaceBase> nested;
    {
        std::lock_guard<std::mutex> guard(child.payloadMutex);
        if (child.data) {
            nested = child.data->borrowNestedShared(0);
        }
    }
    if (nested) {
        ret.errors.emplace_back(Error::Code::NotSupported,
                                "Serialized inserts cannot target nested PathSpaces yet");
        return;
    }

    auto const nextIter = iter.next();
    auto nextResolved   = buildResolvedPath(resolvedPath, component);
    insertSerializedAtNode(child, nextIter, payload, ret, nextResolved);
}

static auto extractNestedSpace(Node& node,
                              InputMetadata const& inputMetadata,
                              void* obj,
                              bool const doExtract,
                              std::optional<std::size_t> index) -> std::optional<Error> {
    if (!doExtract) {
        return Error{Error::Code::NotSupported, "Nested PathSpaces can only be taken"};
    }

    bool const wantsPathSpace     = inputMetadata.typeInfo == &typeid(std::unique_ptr<PathSpace>);
    bool const wantsBasePathSpace = inputMetadata.typeInfo == &typeid(std::unique_ptr<PathSpaceBase>);

    if (!wantsPathSpace && !wantsBasePathSpace) {
        return Error{Error::Code::InvalidType, "Unsupported unique_ptr<T> requested for nested space"};
    }

    std::unique_ptr<PathSpaceBase> moved;
    PathSpace*                      derived = nullptr;

    std::size_t targetIndex = index.value_or(0);

    {
        std::lock_guard<std::mutex> lg(node.payloadMutex);
        if (!node.data || !node.data->hasNestedSpaces()) {
            return Error{Error::Code::NoSuchPath, "No nested PathSpace present at path"};
        }
        auto* nestedPtr = node.data->nestedAt(targetIndex);
        if (!nestedPtr) {
            return Error{Error::Code::NoSuchPath, "No nested PathSpace present at requested index"};
        }
        if (wantsPathSpace) {
            derived = dynamic_cast<PathSpace*>(nestedPtr);
            if (!derived) {
                return Error{Error::Code::InvalidType, "Nested space is not an SP::PathSpace"};
            }
        }
        moved = node.data->takeNestedAt(targetIndex);
        if (!moved) {
            return Error{Error::Code::NoSuchPath, "Failed to remove nested PathSpace at requested index"};
        }
        if (node.data->empty()) {
            node.data.reset();
        }
    }

    if (wantsPathSpace) {
        auto* dest = static_cast<std::unique_ptr<PathSpace>*>(obj);
        auto* raw  = moved.release();
        if (!raw) {
            return Error{Error::Code::NoSuchPath, "Nested PathSpace missing after extraction"};
        }
        *dest = std::unique_ptr<PathSpace>(static_cast<PathSpace*>(raw));
    } else {
        auto* dest = static_cast<std::unique_ptr<PathSpaceBase>*>(obj);
        *dest      = std::move(moved);
    }

    return std::nullopt;
}

auto Leaf::inAtNode(Node& node,
                    Iterator const& iter,
                    InputData const& inputData,
                    InsertReturn& ret,
                    std::string const& resolvedPath) -> void {
    auto const name = iter.currentComponent();
    auto const parsed = parse_indexed_component(name);
    if (parsed.malformed) {
        ret.errors.emplace_back(Error{Error::Code::InvalidPath, "Malformed indexed path component"});
        return;
    }
    auto const baseNameView = parsed.base;
    std::string baseName{baseNameView};

    if (iter.isAtFinalComponent()) {
        bool const nameIsGlob = parsed.index.has_value() ? false : is_glob(name);
        if (nameIsGlob) {
            // Do not allow inserting nested spaces via glob
            if (inputData.metadata.dataCategory == DataCategory::UniquePtr) {
                ret.errors.emplace_back(Error::Code::InvalidType, "PathSpaces cannot be added in glob expressions.");
                return;
            }

            // Collect matching keys first, then modify
            std::vector<std::string> matchingKeys;
            node.children.for_each([&](auto const& kv) {
                auto const& key = kv.first;
                if (match_names(name, key)) {
                    matchingKeys.emplace_back(key);
                }
            });
            for (auto const& key : matchingKeys) {
                if (Node* childPtr = node.getChild(key)) {
                    Node& child = *childPtr;
                    if (inputData.replaceExistingPayload) {
                        clearNodePayload(child);
                    }
                    auto* data = ensureNodeData(child, inputData, ret);
                    if (!data) {
                        continue;
                    }
                    if (inputData.task) {
                        ret.nbrTasksInserted++;
                    } else {
                        ret.nbrValuesInserted++;
                    }
                }
            }
            return;
        }

        if (parsed.index.has_value() && inputData.metadata.dataCategory != DataCategory::UniquePtr) {
            ret.errors.emplace_back(Error{Error::Code::InvalidPath,
                                          "Indexed components require nested PathSpace payloads"});
            return;
        }

        // Concrete final component
        bool parentHasValue = false;
        {
            std::lock_guard<std::mutex> parentLock(node.payloadMutex);
            parentHasValue = ((node.data != nullptr) && !node.data->hasNestedSpaces()) || node.podPayload != nullptr;
        }
        auto const& childKey = parsed.base;
        Node& child = node.getOrCreateChild(childKey);
        if (inputData.replaceExistingPayload) {
            clearNodePayload(child);
        }
        if (inputData.metadata.dataCategory == DataCategory::UniquePtr) {
            if (parsed.index.has_value()) {
                ret.errors.emplace_back(Error{Error::Code::InvalidPath,
                                              "Indexed nested inserts are not supported"});
                return;
            }
            // Move nested PathSpaceBase into the payload queue
            auto* data = ensureNodeData(child, inputData, ret);
            if (!data) {
                return;
            }
            ret.nbrSpacesInserted++;
            if (child.data) {
                auto nestedIndex = child.data->nestedCount() > 0 ? child.data->nestedCount() - 1 : 0;
                if (auto* nestedPtr = child.data->nestedAt(nestedIndex)) {
                    auto mountPath = buildResolvedPath(resolvedPath, baseName);
                    ret.retargets.push_back(
                        InsertReturn::RetargetRequest{nestedPtr, append_index_suffix(mountPath, nestedIndex)});
                }
            }
        } else {
            if (inputData.metadata.podPreferred && !inputData.task) {
                std::unique_lock<std::mutex> packLock(this->packInsertMutex);
                if (auto podErr = tryPodInsert(child, inputData, ret); podErr.has_value()) {
                    if (podErr->code != Error::Code::NotSupported) {
                        ret.errors.emplace_back(*podErr);
                        return;
                    }
                    // Fall through to generic path when not supported.
                } else {
                    packLock.unlock();
                    if (parentHasValue)
                        ret.nbrValuesSuppressed++;
                    return;
                }
                packLock.unlock();
            }
            auto* data = ensureNodeData(child, inputData, ret);
            if (!data) {
                return;
            }
            if (inputData.task) {
                ret.nbrTasksInserted++;
            } else {
                ret.nbrValuesInserted++;
                if (parentHasValue)
                    ret.nbrValuesSuppressed++;
            }
        }
        return;
    }

    // Intermediate component
    auto const nextIterRaw = iter.next();
    bool const nameIsGlob = parsed.index.has_value() ? false : is_glob(name);
    if (nameIsGlob) {
        // Recurse into all matching existing children
        node.children.for_each([&](auto const& kv) {
            auto const& key = kv.first;
            if (!match_names(name, key)) {
                return;
            }
            Node& child = *kv.second;
            std::vector<std::pair<std::shared_ptr<PathSpaceBase>, std::size_t>> nestedTargets;
            {
                std::lock_guard<std::mutex> lg(child.payloadMutex);
                if (child.data) {
                    auto nestedCount = child.data->nestedCount();
                    nestedTargets.reserve(nestedCount);
                    for (std::size_t i = 0; i < nestedCount; ++i) {
                        if (auto nested = child.data->borrowNestedShared(i)) {
                            nestedTargets.emplace_back(std::move(nested), i);
                        }
                    }
                }
            }
            if (!nestedTargets.empty()) {
                std::string relative{"/"};
                relative.append(nextIterRaw.currentToEnd());
                for (auto const& nestedEntry : nestedTargets) {
                    auto const& nestedRaw = nestedEntry.first;
                    auto const  nestedIdx = nestedEntry.second;
                    Iterator nestedIter{relative};
                    InsertReturn nestedRet = nestedRaw->in(nestedIter, inputData);
                    auto mountBase = append_index_suffix(buildResolvedPath(resolvedPath, key), nestedIdx);
                    rebaseRetargets(nestedRet, mountBase);
                    mergeInsertReturn(ret, nestedRet);
                }
            } else {
                auto nextResolved = buildResolvedPath(resolvedPath, key);
                inAtNode(child, nextIterRaw, inputData, ret, nextResolved);
            }
        });
        return;
    }

    // Existing children may hold data; still recurse to allow mixed payload/child nodes (trellis stats etc.)
    if (Node* existing = node.getChild(baseName); existing) {
        std::shared_ptr<PathSpaceBase> nestedRaw;
        {
            std::lock_guard<std::mutex> lg(existing->payloadMutex);
            if (existing->data)
                nestedRaw = existing->data->borrowNestedShared(parsed.index.value_or(0));
        }
        if (nestedRaw) {
            std::string relative{"/"};
            relative.append(nextIterRaw.currentToEnd());
            Iterator nestedIter{relative};
            InsertReturn nestedRet = nestedRaw->in(nestedIter, inputData);
            auto mountBase = append_index_suffix(buildResolvedPath(resolvedPath, baseName),
                                                 parsed.index.value_or(0));
            rebaseRetargets(nestedRet, mountBase);
            mergeInsertReturn(ret, nestedRet);
        } else if (parsed.index.has_value()) {
            ret.errors.emplace_back(Error{Error::Code::NoSuchPath, "Nested PathSpace index not found"});
            return;
        } else {
            auto nextResolved = buildResolvedPath(resolvedPath, baseName);
            inAtNode(*existing, nextIterRaw, inputData, ret, nextResolved);
        }
    } else {
        Node& created      = node.getOrCreateChild(baseName);
        auto  nextResolved = buildResolvedPath(resolvedPath, baseName);
        inAtNode(created, nextIterRaw, inputData, ret, nextResolved);
        if (!ret.errors.empty() && !created.hasData() && !created.hasChildren()) {
            node.eraseChild(baseName);
        }
    }
}

auto Leaf::outAtNode(Node& node,
                     Iterator const& iter,
                     InputMetadata const& inputMetadata,
                     void* obj,
                     bool const doExtract,
                     Node* parent,
                     std::string_view keyInParent) -> std::optional<Error> {
    auto name = iter.currentComponent();
    auto parsed = parse_indexed_component(name);
    if (parsed.malformed) {
        return Error{Error::Code::InvalidPath, "Malformed indexed path component"};
    }
    auto baseNameView = parsed.base;
    std::string baseName{baseNameView};

    if (iter.isAtFinalComponent()) {
        if (inputMetadata.spanReader || inputMetadata.spanMutator) {
            if (parsed.index.has_value() || is_glob(name)) {
                return Error{Error::Code::InvalidPath, "Span reads require concrete non-indexed paths"};
            }
            Node* child = node.getChild(baseName);
            if (!child) {
                sp_log("Leaf::outAtNode(span) no such child: " + std::string(name), "Leaf");
                return Error{Error::Code::NoSuchPath, "Path not found"};
            }
            if (!inputMetadata.podPreferred) {
                return Error{Error::Code::NotSupported, "Span supported only on POD fast path"};
            }
            if (!child->podPayload) {
                return Error{Error::Code::NotSupported, "POD fast path unavailable"};
            }
            if (inputMetadata.spanReader) {
                return tryPodSpan(
                    *child, inputMetadata, [&](void const* data, std::size_t count) { inputMetadata.spanReader(data, count); });
            }
            return tryPodSpanMutable(
                *child, inputMetadata, [&](void* data, std::size_t count) { inputMetadata.spanMutator(data, count); });
        }

        // Support glob at final component by selecting the smallest matching key
        bool const nameIsGlob = parsed.index.has_value() ? false : is_glob(name);
        if (nameIsGlob) {
            // Collect matching keys and try each in lexicographic order until one succeeds
            std::vector<std::string> matches;
            node.children.for_each([&](auto const& kv) {
                auto const& key = kv.first;
                if (match_names(name, key)) {
                    matches.emplace_back(key);
                }
            });
            if (matches.empty()) {
                sp_log("Leaf::outAtNode(final,glob) no matches for pattern: " + std::string(name), "Leaf");
                return Error{Error::Code::NoSuchPath, "Path not found"};
            }
            std::sort(matches.begin(), matches.end());
            bool foundAny = false;
            for (auto const& k : matches) {
                Node* childTry = node.getChild(k);
                if (!childTry)
                    continue;
                std::optional<Error> res;
                bool attempted = false;
                if (childTry->podPayload && inputMetadata.podPreferred) {
                    foundAny  = true;
                    attempted = true;
                    res       = tryPodRead(*childTry, inputMetadata, obj, doExtract);
                } else {
                    std::lock_guard<std::mutex> lg(childTry->payloadMutex);
                    if (childTry->data) {
                        foundAny = true;
                        attempted = true;
                        sp_log("Leaf::outAtNode(final,glob) attempting deserialize on: " + k, "Leaf");
                        if (doExtract) {
                            res = childTry->data->deserializePop(obj, inputMetadata);
                            if (!res.has_value() && childTry->data->empty()) {
                                // Keep node; avoid erasure to prevent races under concurrency
                                childTry->data.reset();
                            }
                        } else {
                            res = childTry->data->deserialize(obj, inputMetadata);
                        }
                    } else {
                        sp_log("Leaf::outAtNode(final,glob) child has no data: " + k, "Leaf");
                    }
                }
                // Success path: return immediately only if we actually attempted and succeeded
                if (attempted && !res.has_value()) {
                    sp_log("Leaf::outAtNode(final,glob) success on: " + k, "Leaf");
                    return std::nullopt;
                }
                if (attempted && res.has_value()) {
                    sp_log("Leaf::outAtNode(final,glob) failed on: " + k + " code=" + std::to_string(static_cast<int>(res->code)) + " msg=" + res->message.value_or(""), "Leaf");
                }
            }
            // If we saw at least one matching child but none yielded a value of the requested type,
            // surface a type error; otherwise, report no such path.
            if (foundAny) {
                sp_log("Leaf::outAtNode(final,glob) type mismatch after attempts", "Leaf");
                return Error{Error::Code::InvalidType, "Type mismatch during deserialization"};
            }
            sp_log("Leaf::outAtNode(final,glob) no such path after matching", "Leaf");
            return Error{Error::Code::NoSuchPath, "Path not found"};
        }

        Node* child = node.getChild(baseName);
        if (!child) {
            sp_log("Leaf::outAtNode(final) no such child: " + std::string(name), "Leaf");
            return Error{Error::Code::NoSuchPath, "Path not found"};
        }
 
        if (inputMetadata.dataCategory == DataCategory::UniquePtr) {
            return extractNestedSpace(*child, inputMetadata, obj, doExtract, parsed.index);
        }

        if (parsed.index.has_value()) {
            if (child->podPayload) {
                InsertReturn upgradeRet;
                if (!migratePodToNodeData(*child, upgradeRet)) {
                    if (!upgradeRet.errors.empty()) {
                        return upgradeRet.errors.front();
                    }
                    return Error{Error::Code::UnknownError, "Failed to upgrade POD payload for indexed access"};
                }
            }
            std::lock_guard<std::mutex> lg(child->payloadMutex);
            if (!child->data) {
                sp_log("Leaf::outAtNode(final,indexed) no data present", "Leaf");
                return Error{Error::Code::NoSuchPath, "Path not found"};
            }
            auto res = child->data->deserializeIndexed(*parsed.index, inputMetadata, doExtract, obj);
            if (!res.has_value() && child->data->empty()) {
                // Keep node; avoid erasure to prevent races under concurrency
                child->data.reset();
            }
            return res;
        }

        if (child->podPayload && !inputMetadata.podPreferred) {
            return Error{Error::Code::InvalidType, "Type mismatch during deserialization"};
        }

        if (child->podPayload && inputMetadata.podPreferred) {
            auto res = tryPodRead(*child, inputMetadata, obj, doExtract);
            if (res.has_value()) {
                sp_log("Leaf::outAtNode(final) POD deserialize failed code=" + std::to_string(static_cast<int>(res->code)) + " msg=" + res->message.value_or(""), "Leaf");
            } else {
                sp_log("Leaf::outAtNode(final) POD deserialize success on child: " + std::string(name), "Leaf");
            }
            return res;
        }

        if (child->data) {
            std::optional<Error> res;
            {
                std::lock_guard<std::mutex> lg(child->payloadMutex);
                if (child->data) {
                    sp_log(std::string("Leaf::outAtNode(final) deserializing on child: ") + std::string(name) + (doExtract ? " (pop)" : " (read)"), "Leaf");
                    if (doExtract) {
                        res = child->data->deserializePop(obj, inputMetadata);
                        if (!res.has_value()) {
                            if (child->data->empty()) {
                                // Keep node; avoid erasure to prevent races under concurrency
                                child->data.reset();
                            }
                        }
                    } else {
                        res = child->data->deserialize(obj, inputMetadata);
                    }
                } else {
                    sp_log("Leaf::outAtNode(final) child hasData()==true but data==nullptr", "Leaf");
                }
            }
            // If popped to empty, child may have been erased under payload lock.
            if (res.has_value()) {
                sp_log("Leaf::outAtNode(final) deserialize failed code=" + std::to_string(static_cast<int>(res->code)) + " msg=" + res->message.value_or(""), "Leaf");
            } else {
                sp_log("Leaf::outAtNode(final) deserialize success on child: " + std::string(name), "Leaf");
            }
            return res;
        }

        // Final component, but nested space present or no data: treat as not found (compat)
        sp_log("Leaf::outAtNode(final) no data and no nested space for child: " + std::string(name), "Leaf");
        return Error{Error::Code::NoSuchPath, "Path not found"};
    }

    // Intermediate component
    bool const nameIsGlob = parsed.index.has_value() ? false : is_glob(name);
    if (nameIsGlob) {
        return Error{Error::Code::NoSuchPath, "Path not found"};
    }

    Node* child = node.getChild(baseName);
    if (!child)
        return Error{Error::Code::NoSuchPath, "Path not found"};

    // If this node stores data and no deeper structure, it's an invalid subcomponent
    auto const nextIter = iter.next();
    std::shared_ptr<PathSpaceBase> nested;
    {
        std::lock_guard<std::mutex> lg(child->payloadMutex);
        if (child->data) {
            if (parsed.index.has_value()) {
                nested = child->data->borrowNestedShared(*parsed.index);
            } else {
                nested = child->data->borrowNestedShared(0);
            }
        }
    }
    if (nested) {
        std::string relative{"/"};
        relative.append(nextIter.currentToEnd());
        Iterator nestedIter{relative};
        return nested->out(nestedIter, inputMetadata, Out{.doPop = doExtract}, obj);
    }
    if (parsed.index.has_value()) {
        return Error{Error::Code::NoSuchPath, "Nested PathSpace index not found"};
    }

    return outAtNode(*child, nextIter, inputMetadata, obj, doExtract, &node, nextIter.currentComponent());
}

auto Leaf::extractSerializedAtNode(Node& node,
                                   Iterator const& iter,
                                   NodeData&      payload) -> std::optional<Error> {
    auto name = iter.currentComponent();

    if (iter.isAtFinalComponent()) {
        if (is_glob(name)) {
            std::vector<std::string> matches;
            node.children.for_each([&](auto const& kv) {
                auto const& key = kv.first;
                if (match_names(name, key)) {
                    matches.emplace_back(key);
                }
            });
            if (matches.empty()) {
                return Error{Error::Code::NoSuchPath, "Path not found"};
            }
            std::sort(matches.begin(), matches.end());
            std::optional<Error> lastError;
            bool                 attempted = false;
            for (auto const& key : matches) {
                Node* childTry = node.getChild(key);
                if (!childTry)
                    continue;
                std::lock_guard<std::mutex> lg(childTry->payloadMutex);
                if (!childTry->data)
                    continue;
                attempted = true;
                NodeData serialized;
                auto     res = childTry->data->popFrontSerialized(serialized);
                if (!res.has_value()) {
                    payload = std::move(serialized);
                    if (!childTry->data || childTry->data->empty()) {
                        childTry->data.reset();
                    }
                    return std::nullopt;
                }
                lastError = res;
            }
            if (attempted && lastError.has_value()) {
                return lastError;
            }
            return Error{Error::Code::NoSuchPath, "Path not found"};
        }

        Node* child = node.getChild(name);
        if (!child) {
            return Error{Error::Code::NoSuchPath, "Path not found"};
        }

        std::lock_guard<std::mutex> lg(child->payloadMutex);
        if (!child->data) {
            return Error{Error::Code::NoSuchPath, "Path not found"};
        }
        NodeData serialized;
        auto     res = child->data->popFrontSerialized(serialized);
        if (!res.has_value()) {
            payload = std::move(serialized);
            if (!child->data || child->data->empty()) {
                child->data.reset();
            }
        }
        return res;
    }

    if (is_glob(name)) {
        return Error{Error::Code::NoSuchPath, "Path not found"};
    }

    Node* child = node.getChild(name);
    if (!child) {
        return Error{Error::Code::NoSuchPath, "Path not found"};
    }

    if (child->data && child->data->hasNestedSpaces()) {
        return Error{Error::Code::NotSupported,
                     "Serialized extraction unsupported for nested PathSpaces"};
    }

    return extractSerializedAtNode(*child, iter.next(), payload);
}



auto Leaf::clear() -> void {
    this->root.clearRecursive();
}

/*
    ############# In #############
*/
auto Leaf::in(Iterator const& iter, InputData const& inputData, InsertReturn& ret) -> void {
    this->inAtNode(this->root, iter, inputData, ret, "/");
}

auto Leaf::inFinalComponent(Iterator const& iter, InputData const& inputData, InsertReturn& ret) -> void {
    // Kept for compatibility with existing calls; redirect to generic handler.
    this->inAtNode(this->root, iter, inputData, ret, "/");
}

auto Leaf::inIntermediateComponent(Iterator const& iter, InputData const& inputData, InsertReturn& ret) -> void {
    // Kept for compatibility with existing calls; redirect to generic handler.
    this->inAtNode(this->root, iter, inputData, ret, "/");
}

/*
    ############# Out #############
*/
auto Leaf::out(Iterator const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> std::optional<Error> {
    return this->outAtNode(this->root, iter, inputMetadata, obj, doExtract, nullptr, {});
}

auto Leaf::outFinalComponent(Iterator const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> std::optional<Error> {
    return this->outAtNode(this->root, iter, inputMetadata, obj, doExtract, nullptr, {});
}

auto Leaf::outIntermediateComponent(Iterator const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> std::optional<Error> {
    return this->outAtNode(this->root, iter, inputMetadata, obj, doExtract, nullptr, {});
}

auto Leaf::extractSerialized(Iterator const& iter, NodeData& payload) -> std::optional<Error> {
    return this->extractSerializedAtNode(this->root, iter, payload);
}

auto Leaf::peekFuture(Iterator const& iter) const -> std::optional<Future> {
    // Walk down to the final component non-mutatingly
    Node const* node = &this->root;
    Iterator    it   = iter;
    while (!it.isAtFinalComponent()) {
        auto const name = it.currentComponent();
        if (is_glob(name)) {
            // For peek we only support concrete traversal
            return std::nullopt;
        }
        node = node->getChild(name);
        if (!node)
            return std::nullopt;
        it = it.next();
    }

    // At final component: if it's a glob, we don't resolve here
    auto const last = it.currentComponent();
    if (is_glob(last))
        return std::nullopt;

    Node const* child = node->getChild(last);
    if (!child)
        return std::nullopt;

    // Check if the node stores execution: we need to peek at the front task
    {
        std::lock_guard<std::mutex> lg(child->payloadMutex);
        if (!child->data || child->data->empty())
            return std::nullopt;
        if (auto fut = child->data->peekFuture())
            return fut;
        return std::nullopt;
    }
}


auto Leaf::peekAnyFuture(Iterator const& iter) const -> std::optional<FutureAny> {
    // Walk down to the final component non-mutatingly
    Node const* node = &this->root;
    Iterator    it   = iter;
    while (!it.isAtFinalComponent()) {
        auto const name = it.currentComponent();
        if (is_glob(name)) {
            // For peek we only support concrete traversal
            return std::nullopt;
        }
        node = node->getChild(name);
        if (!node)
            return std::nullopt;
        it = it.next();
    }

    // At final component: if it's a glob, we don't resolve here
    auto const last = it.currentComponent();
    if (is_glob(last))
        return std::nullopt;

    Node const* child = node->getChild(last);
    if (!child)
        return std::nullopt;

    // If the node stores execution, surface the type-erased future aligned with the front task
    {
        std::lock_guard<std::mutex> lg(child->payloadMutex);
        if (!child->data || child->data->empty())
            return std::nullopt;
        if (auto fut = child->data->peekAnyFuture())
            return fut;
        return std::nullopt;
    }
}

auto Leaf::spanPackConst(std::span<const std::string> paths,
                         InputMetadata const& inputMetadata,
                         Out const& options,
                         SpanPackConstCallback const& fn) const -> Expected<void> {
    if (options.doBlock || options.doPop) {
        return std::unexpected(Error{Error::Code::NotSupported, "Span pack does not support blocking or pop"});
    }
    if (!inputMetadata.podPreferred) {
        return std::unexpected(Error{Error::Code::NotSupported, "Span pack requires POD fast path"});
    }
    constexpr int MaxAttempts = 4;
    for (int attempt = 0; attempt < MaxAttempts; ++attempt) {
        std::vector<RawConstSpan>      spans(paths.size());
        std::vector<std::shared_ptr<void>> keepers(paths.size());
        std::optional<std::size_t>     expectedCount;
        Node&                          rootRef = const_cast<Node&>(this->root);
        std::optional<Error>           lastError;
        auto recurse = [&](auto&& self, std::size_t idx) -> Expected<void> {
            if (idx == paths.size()) {
                if (auto err = fn(std::span<RawConstSpan const>(spans.data(), spans.size()))) {
                    return std::unexpected(*err);
                }
                return {};
            }

            auto const& pathStr = paths[idx];
            auto nodeExpected   = resolveConcreteNode(rootRef, pathStr);
            if (!nodeExpected) {
                sp_log("Leaf::spanPackConst resolve failed path=" + pathStr, "Leaf");
                return std::unexpected(nodeExpected.error());
            }

            std::optional<Error> nestedErr;
            auto err = tryPodSpanPinned(*nodeExpected.value(), inputMetadata, [&](void const* data, std::size_t count, std::shared_ptr<void> const& keeper) {
                if (!expectedCount) {
                    expectedCount = count;
                } else if (*expectedCount != count) {
                    nestedErr = Error{Error::Code::InvalidType, "Span lengths mismatch"};
                    return;
                }
                spans[idx]   = RawConstSpan{data, count};
                keepers[idx] = keeper;
                auto next    = self(self, idx + 1);
                if (!next) {
                    nestedErr = next.error();
                }
            });

            if (err) {
                sp_log("Leaf::spanPackConst pod span error path=" + pathStr, "Leaf");
                return std::unexpected(*err);
            }
            if (nestedErr) {
                return std::unexpected(*nestedErr);
            }
            return {};
        };

        auto res = recurse(recurse, 0);
        if (res) {
            return res; // success
        }
        lastError = res.error();
        if (lastError->code != Error::Code::InvalidType
            || lastError->message != std::optional<std::string>{"Span lengths mismatch"}
            || attempt == MaxAttempts - 1) {
            return std::unexpected(*lastError);
        }
        // Retry on transient span length mismatches that can occur mid-insert.
        std::this_thread::yield();
    }
    return std::unexpected(Error{Error::Code::UnknownError, "Span pack read retry exhausted"});
}

auto Leaf::spanPackMut(std::span<const std::string> paths,
                       InputMetadata const& inputMetadata,
                       Out const& options,
                       SpanPackMutCallback const& fn) const -> Expected<void> {
    if (!inputMetadata.podPreferred) {
        return std::unexpected(Error{Error::Code::NotSupported, "Span pack requires POD fast path"});
    }
    if (!this->packInsertSeen.load(std::memory_order_acquire)) {
        for (int i = 0; i < 5 && !this->packInsertSeen.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    Node& rootRef = const_cast<Node&>(this->root);
    std::vector<Node*> nodes;
    nodes.reserve(paths.size());
    for (auto const& pathStr : paths) {
        auto nodeExpected = resolveConcreteNode(rootRef, pathStr);
        if (!nodeExpected) {
            sp_log("Leaf::spanPackMut resolve failed path=" + pathStr, "Leaf");
            return std::unexpected(nodeExpected.error());
        }
        nodes.push_back(nodeExpected.value());
        if (options.isMinimal) {
            auto payload = nodeExpected.value()->podPayload;
            if (!payload) {
                return std::unexpected(Error{Error::Code::InvalidType, "POD fast path unavailable"});
            }
        }
    }

    constexpr int MaxAttempts = 10;
    std::optional<Error> lastError;

    for (int attempt = 0; attempt < MaxAttempts; ++attempt) {
        std::vector<RawMutSpan>            spans(paths.size());
        std::vector<std::shared_ptr<void>> keepers(paths.size());
        std::vector<std::size_t>           heads(paths.size());
        std::vector<std::size_t>           starts(paths.size());
        std::optional<std::size_t>         expectedCount;
        lastError.reset();

        for (std::size_t idx = 0; idx < paths.size(); ++idx) {
            auto* node = nodes[idx];
            std::optional<Error> captureErr;
            std::size_t          startIndex = 0;
            std::size_t          headIndex  = 0;
            if (auto payload = node->podPayload) {
                headIndex = payload->headIndex();
                if (options.isMinimal) {
                    auto packStart = payload->packSpanStart();
                    if (!packStart) {
                        // No pending pack marker; default minimal window to current head.
                        payload->markPackSpanStart(headIndex);
                        packStart = payload->packSpanStart();
                    }
                    startIndex = (packStart && *packStart > headIndex) ? *packStart : headIndex;
                } else {
                    startIndex = headIndex;
                }
            }
            heads[idx]  = headIndex;
            starts[idx] = startIndex;

            auto err = tryPodSpanMutableFromPinned(*node,
                                                   inputMetadata,
                                                   startIndex,
                                                   [&](void* data, std::size_t count, std::shared_ptr<void> const& keeper) {
                                                       if (!expectedCount) {
                                                           expectedCount = count;
                                                       } else if (*expectedCount != count) {
                                                           captureErr = Error{Error::Code::InvalidType, "Span lengths mismatch"};
                                                           return;
                                                       }
                                                       spans[idx]   = RawMutSpan{data, count};
                                                       keepers[idx] = keeper;
                                                   });

            if (err) {
                sp_log("Leaf::spanPackMut pod span error path=" + std::string(paths[idx]), "Leaf");
                lastError = *err;
                break;
            }
            if (captureErr) {
                lastError = *captureErr;
                break;
            }
        }

        if (!lastError) {
            if (expectedCount && *expectedCount == 0) {
                return std::unexpected(Error{Error::Code::NoObjectFound, "No data available"});
            }

            auto result = fn(std::span<RawMutSpan const>(spans.data(), spans.size()));
            if (result.error) {
                return std::unexpected(*result.error);
            }

            if (result.shouldPop) {
                // Require all spans to start at head to avoid skipping prior elements.
                for (std::size_t idx = 0; idx < nodes.size(); ++idx) {
                    if (starts[idx] != heads[idx]) {
                        return std::unexpected(Error{Error::Code::InvalidType, "Pop requires head-aligned spans"});
                    }
                }
                for (std::size_t idx = 0; idx < nodes.size(); ++idx) {
                    auto payload = nodes[idx]->podPayload;
                    if (!payload) {
                        return std::unexpected(Error{Error::Code::InvalidType, "POD fast path unavailable"});
                    }
                    if (auto popErr = payload->popCount(*expectedCount)) {
                        return std::unexpected(*popErr);
                    }
                }
            }
            return {};
        }

        if (lastError->code == Error::Code::InvalidType
            && lastError->message == std::optional<std::string>{"Span lengths mismatch"}
            && attempt < MaxAttempts - 1) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
        }

        return std::unexpected(*lastError);
    }
    return std::unexpected(Error{Error::Code::UnknownError, "Span pack take retry exhausted"});
}

static auto createConcreteNode(Node& root, std::string const& pathStr) -> Expected<Node*> {
    if (pathStr.empty() || pathStr.front() != '/') {
        return std::unexpected(Error{Error::Code::InvalidPath, "Pack insert path must start with '/'"});
    }
    if (pathStr.size() == 1) {
        return std::unexpected(Error{Error::Code::InvalidPath, "Pack insert path cannot be root"});
    }
    Node*       current = &root;
    std::size_t pos     = 1;
    while (pos < pathStr.size()) {
        auto next = pathStr.find('/', pos);
        std::size_t len = (next == std::string::npos) ? pathStr.size() - pos : next - pos;
        if (len == 0) {
            return std::unexpected(Error{Error::Code::InvalidPath, "Empty path component"});
        }
        std::string_view name{pathStr.data() + pos, len};
        if (is_glob(name) || name.find('[') != std::string_view::npos) {
            return std::unexpected(Error{Error::Code::InvalidPath, "Pack insert requires concrete non-indexed paths"});
        }
        current = &current->getOrCreateChild(name);
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return current;
}

auto Leaf::packInsert(std::span<const std::string> paths,
                      InputMetadata const& inputMetadata,
                      std::span<void const* const> values) -> InsertReturn {
    std::scoped_lock packLock(this->packInsertMutex);
    this->packInsertSeen.store(true, std::memory_order_release);
    InsertReturn ret;
    if (!inputMetadata.podPreferred || inputMetadata.dataCategory == DataCategory::UniquePtr
        || inputMetadata.dataCategory == DataCategory::Execution) {
        ret.errors.emplace_back(Error{Error::Code::NotSupported, "Pack insert requires POD fast path"});
        return ret;
    }
    if (paths.size() != values.size()) {
        ret.errors.emplace_back(Error{Error::Code::InvalidType, "Pack insert arity mismatch"});
        return ret;
    }

    std::vector<Node*> nodes;
    nodes.reserve(paths.size());
    for (auto const& pathStr : paths) {
        auto nodeExpected = createConcreteNode(this->root, pathStr);
        if (!nodeExpected) {
            ret.errors.emplace_back(nodeExpected.error());
            return ret;
        }
        nodes.push_back(nodeExpected.value());
    }

    std::vector<std::shared_ptr<PodPayloadBase>> payloads;
    payloads.reserve(paths.size());
    std::vector<PodPayloadBase::Reservation> reservations;
    reservations.reserve(paths.size());
    std::vector<std::size_t> packStarts;
    packStarts.reserve(paths.size());

    // First pass: ensure payloads and reserve slots.
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        InputData inputData{values[i], inputMetadata};
        auto payload = ensurePodPayload(*nodes[i], inputData);
        if (!payload) {
            ret.errors.emplace_back(Error{Error::Code::InvalidType, "POD fast path unavailable for pack insert"});
            // Roll back earlier reservations if any
            for (std::size_t j = 0; j < reservations.size(); ++j) {
                payloads[j]->rollbackOne(reservations[j].index);
            }
            return ret;
        }
        auto startIndex = payload->publishedTail();

        auto reservation = payload->reserveOne();
        if (!reservation) {
            ret.errors.emplace_back(Error{Error::Code::NotSupported, "Pack insert reservation failed"});
            for (std::size_t j = 0; j < reservations.size(); ++j) {
                payloads[j]->rollbackOne(reservations[j].index);
            }
            return ret;
        }
        payloads.push_back(std::move(payload));
        reservations.push_back(*reservation);
        packStarts.push_back(startIndex);

        if (auto hook = testing::GetPackInsertReservationHook()) {
            hook();
        }
    }

    // Stamp pack markers only after all reservations succeed.
    for (std::size_t i = 0; i < payloads.size(); ++i) {
        payloads[i]->markPackSpanStart(packStarts[i]);
    }

    // Second pass: write values into reserved slots.
    for (std::size_t i = 0; i < reservations.size(); ++i) {
        auto* dst = reservations[i].ptr;
        auto* src = values[i];
        std::memcpy(dst, src, payloads[i]->elementSize());
    }

    // Final pass: publish all slots so they become visible together.
    for (std::size_t i = 0; i < reservations.size(); ++i) {
        payloads[i]->publishOne(reservations[i].index);
    }

    ret.nbrValuesInserted += static_cast<uint32_t>(reservations.size());
    return ret;
}

auto Leaf::packInsertSpans(std::span<const std::string> paths,
                           std::span<SpanInsertSpec const> specs) -> InsertReturn {
    std::scoped_lock packLock(this->packInsertMutex);
    this->packInsertSeen.store(true, std::memory_order_release);
    InsertReturn ret;

    if (paths.size() != specs.size()) {
        ret.errors.emplace_back(Error{Error::Code::InvalidType, "Span pack insert arity mismatch"});
        return ret;
    }
    if (specs.empty()) {
        return ret;
    }

    for (auto const& spec : specs) {
        if (!spec.metadata.podPreferred
            || spec.metadata.dataCategory == DataCategory::UniquePtr
            || spec.metadata.dataCategory == DataCategory::Execution) {
            ret.errors.emplace_back(Error{Error::Code::NotSupported, "Span pack insert requires POD fast path"});
            return ret;
        }
    }

    std::vector<Node*> nodes;
    nodes.reserve(paths.size());
    for (auto const& pathStr : paths) {
        auto nodeExpected = createConcreteNode(this->root, pathStr);
        if (!nodeExpected) {
            ret.errors.emplace_back(nodeExpected.error());
            return ret;
        }
        nodes.push_back(nodeExpected.value());
    }

    std::optional<std::size_t> expectedCount;
    for (auto const& spec : specs) {
        if (!expectedCount) {
            expectedCount = spec.span.count;
        } else if (*expectedCount != spec.span.count) {
            ret.errors.emplace_back(Error{Error::Code::InvalidType, "Span lengths mismatch"});
            return ret;
        }
    }

    std::vector<std::shared_ptr<PodPayloadBase>> payloads;
    payloads.reserve(nodes.size());

    std::optional<std::size_t> expectedHead;
    std::optional<std::size_t> expectedTail;

    for (std::size_t i = 0; i < nodes.size(); ++i) {
        InputData inputData{specs[i].span.data, specs[i].metadata};
        auto payload = ensurePodPayload(*nodes[i], inputData);
        if (!payload) {
            ret.errors.emplace_back(Error{Error::Code::InvalidType, "POD fast path unavailable for span insert"});
            return ret;
        }
        auto head = payload->headIndex();
        auto tail = payload->publishedTail();
        if (!expectedHead) {
            expectedHead = head;
            expectedTail = tail;
        } else if (*expectedHead != head || *expectedTail != tail) {
            ret.errors.emplace_back(Error{Error::Code::InvalidType, "Existing span lengths mismatch"});
            return ret;
        }
        payloads.push_back(std::move(payload));
    }

    std::vector<PodPayloadBase::ReservationSpan> reservations;
    reservations.reserve(payloads.size());

    for (std::size_t i = 0; i < payloads.size(); ++i) {
        auto reservation = payloads[i]->reserveSpan(*expectedCount);
        if (!reservation) {
            ret.errors.emplace_back(Error{Error::Code::NotSupported, "Span pack insert reservation failed"});
            for (std::size_t j = 0; j < reservations.size(); ++j) {
                payloads[j]->rollbackSpan(reservations[j].index, reservations[j].count);
            }
            return ret;
        }
        payloads[i]->markPackSpanStart(reservation->index);
        reservations.push_back(*reservation);
    }

    for (std::size_t i = 0; i < reservations.size(); ++i) {
        if (*expectedCount == 0) {
            continue;
        }
        auto bytes = specs[i].elementSize * (*expectedCount);
        std::memcpy(reservations[i].ptr, specs[i].span.data, bytes);
    }

    for (std::size_t i = 0; i < reservations.size(); ++i) {
        payloads[i]->publishSpan(reservations[i].index, reservations[i].count);
    }

    ret.nbrValuesInserted += static_cast<uint32_t>((*expectedCount) * specs.size());
    return ret;
}

} // namespace SP
