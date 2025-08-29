#include "Leaf.hpp"
#include "PathSpaceBase.hpp"

#include "core/Error.hpp"
#include "core/InsertReturn.hpp"
#include "path/Iterator.hpp"
#include "path/utils.hpp"
#include "type/InputData.hpp"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace SP {

// Forward declarations of internal helpers (defined below)
static auto ensureNodeData(Node& n, InputData const& inputData, InsertReturn& ret) -> NodeData*;
static void mergeInsertReturn(InsertReturn& into, InsertReturn const& from);
static auto inAtNode(Node& node, Iterator const& iter, InputData const& inputData, InsertReturn& ret) -> void;
static auto outAtNode(Node& node,
                      Iterator const& iter,
                      InputMetadata const& inputMetadata,
                      void* obj,
                      bool const doExtract,
                      Node* parent,
                      std::string_view keyInParent) -> std::optional<Error>;

auto Leaf::ensureNodeData(Node& n, InputData const& inputData, InsertReturn& ret) -> NodeData* {
    {
        std::lock_guard<std::mutex> lg(n.payloadMutex);
        if (!n.data) {
            n.data = std::make_unique<NodeData>(inputData);
        } else {
            if (auto err = n.data->serialize(inputData); err.has_value())
                ret.errors.emplace_back(err.value());
        }
    }
    return n.data.get();
}

void Leaf::mergeInsertReturn(InsertReturn& into, InsertReturn const& from) {
    into.nbrValuesInserted += from.nbrValuesInserted;
    into.nbrSpacesInserted += from.nbrSpacesInserted;
    into.nbrTasksInserted  += from.nbrTasksInserted;
    if (!from.errors.empty()) {
        into.errors.insert(into.errors.end(), from.errors.begin(), from.errors.end());
    }
}

auto Leaf::inAtNode(Node& node, Iterator const& iter, InputData const& inputData, InsertReturn& ret) -> void {
    auto const name = iter.currentComponent();

    if (iter.isAtFinalComponent()) {
        if (is_glob(name)) {
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
                    ensureNodeData(child, inputData, ret);
                    if (inputData.task)
                        ret.nbrTasksInserted++;
                    else
                        ret.nbrValuesInserted++;
                }
            }
            return;
        }

        // Concrete final component
        Node& child = node.getOrCreateChild(name);
        if (inputData.metadata.dataCategory == DataCategory::UniquePtr) {
            // Move nested PathSpaceBase into place and adopt parent context/prefix when available
            {
                std::lock_guard<std::mutex> lg(child.payloadMutex);
                child.nested = std::move(*static_cast<std::unique_ptr<PathSpaceBase>*>(inputData.obj));
            }
            // Compute mount prefix for the nested space: <startToCurrent>/<name>
            std::string mountPrefix = std::string(iter.startToCurrent());
            if (mountPrefix.empty() || mountPrefix == "/")
                mountPrefix = "/" + std::string(name);
            else
                mountPrefix += "/" + std::string(name);

            ret.nbrSpacesInserted++;
        } else {
            ensureNodeData(child, inputData, ret);
            if (inputData.task)
                ret.nbrTasksInserted++;
            else
                ret.nbrValuesInserted++;
        }
        return;
    }

    // Intermediate component
    auto const nextIter = iter.next();
    if (is_glob(name)) {
        // Recurse into all matching existing children
        node.children.for_each([&](auto const& kv) {
            auto const& key = kv.first;
            if (match_names(name, key)) {
                Node& child = *kv.second;
                PathSpaceBase* nestedRaw = nullptr;
                bool           hasData   = false;
                {
                    std::lock_guard<std::mutex> lg(child.payloadMutex);
                    if (child.nested)
                        nestedRaw = child.nested.get();
                    hasData = (child.data != nullptr);
                }
                if (nestedRaw) {
                    // Forward to nested space
                    InsertReturn nestedRet = nestedRaw->in(nextIter, inputData);
                    mergeInsertReturn(ret, nestedRet);
                } else if (hasData) {
                    // Do not create subkeys under a node that already holds data
                    // (maintain invariant: leaf-with-data blocks deeper structure)
                } else {
                    inAtNode(child, nextIter, inputData, ret);
                }
            }
        });
        return;
    }

    // If child exists and holds data, do not create subkeys under it
    if (Node* existing = node.getChild(name); existing) {
        PathSpaceBase* nestedRaw = nullptr;
        bool           hasData   = false;
        {
            std::lock_guard<std::mutex> lg(existing->payloadMutex);
            if (existing->nested)
                nestedRaw = existing->nested.get();
            hasData = (existing->data != nullptr);
        }
        if (nestedRaw) {
            InsertReturn nestedRet = nestedRaw->in(nextIter, inputData);
            mergeInsertReturn(ret, nestedRet);
        } else if (hasData) {
            // Maintain invariant: no subkeys under data node
            return;
        } else {
            inAtNode(*existing, nextIter, inputData, ret);
        }
    } else {
        Node& created = node.getOrCreateChild(name);
        inAtNode(created, nextIter, inputData, ret);
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

    if (iter.isAtFinalComponent()) {
        // Support glob at final component by selecting the smallest matching key
        if (is_glob(name)) {
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

        Node* child = node.getChild(name);
        if (!child) {
            sp_log("Leaf::outAtNode(final) no such child: " + std::string(name), "Leaf");
            return Error{Error::Code::NoSuchPath, "Path not found"};
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
    if (is_glob(name)) {
        return Error{Error::Code::NoSuchPath, "Path not found"};
    }

    Node* child = node.getChild(name);
    if (!child)
        return Error{Error::Code::NoSuchPath, "Path not found"};

    // If this node stores data and no deeper structure, it's an invalid subcomponent
    if (child->hasData() && !child->hasChildren() && !child->hasNestedSpace())
        return Error{Error::Code::InvalidPathSubcomponent, "Sub-component name is data"};

    auto const nextIter = iter.next();
    if (child->hasNestedSpace()) {
        return child->nested->out(nextIter, inputMetadata, Out{.doPop = doExtract, .isMinimal = true}, obj);
    }

    return outAtNode(*child, nextIter, inputMetadata, obj, doExtract, &node, nextIter.currentComponent());
}



auto Leaf::clear() -> void {
    this->root.clearRecursive();
}

/*
    ############# In #############
*/
auto Leaf::in(Iterator const& iter, InputData const& inputData, InsertReturn& ret) -> void {
    this->inAtNode(this->root, iter, inputData, ret);
}

auto Leaf::inFinalComponent(Iterator const& iter, InputData const& inputData, InsertReturn& ret) -> void {
    // Kept for compatibility with existing calls; redirect to generic handler.
    this->inAtNode(this->root, iter, inputData, ret);
}

auto Leaf::inIntermediateComponent(Iterator const& iter, InputData const& inputData, InsertReturn& ret) -> void {
    // Kept for compatibility with existing calls; redirect to generic handler.
    this->inAtNode(this->root, iter, inputData, ret);
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