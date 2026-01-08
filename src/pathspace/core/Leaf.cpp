#include "Leaf.hpp"
#include "PathSpaceBase.hpp"
#include "PathSpace.hpp"

#include "core/Error.hpp"
#include "core/InsertReturn.hpp"
#include "path/Iterator.hpp"
#include "path/utils.hpp"
#include "type/InputData.hpp"
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <typeinfo>
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

auto Leaf::ensureNodeData(Node& n, InputData const& inputData, InsertReturn& ret) -> NodeData* {
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

} // namespace

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
            parentHasValue = (node.data != nullptr) && !node.data->hasNestedSpaces();
        }
        auto const& childKey = parsed.base;
        Node& child = node.getOrCreateChild(childKey);
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
                {
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

        if (child->hasData()) {
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

} // namespace SP
