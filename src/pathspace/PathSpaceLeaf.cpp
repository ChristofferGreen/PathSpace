#include "PathSpaceLeaf.hpp"
#include "core/Error.hpp"
#include "core/InsertReturn.hpp"
#include "path/GlobName.hpp"
#include "path/path_utils.hpp"
#include "pathspace/type/InputData.hpp"
#include "type/InputData.hpp"

namespace SP {

auto PathSpaceLeaf::clear() -> void {
    this->nodeDataMap.clear();
}

/*
    ############# In #############
*/

auto PathSpaceLeaf::in(PathIterator const& iter, InputData const& inputData, InsertReturn& ret) -> void {
    sp_log("PathSpaceLeaf::in2 Processing path component: " + std::string(iter.currentComponent()), "PathSpaceLeaf");
    if (iter.isAtFinalComponent())
        inFinalComponent(iter, inputData, ret);
    else
        inIntermediateComponent(iter, inputData, ret);
}

auto PathSpaceLeaf::inFinalComponent(PathIterator const& iter, InputData const& inputData, InsertReturn& ret) -> void {
    auto const pathComponent = iter.currentComponent();
    if (is_glob(pathComponent)) {
        // Create a vector to store the keys that match before modification.
        std::vector<ConcreteNameString> matchingKeys;

        // First pass: Collect all matching keys without holding write locks
        nodeDataMap.for_each([&](auto& item) {
            const auto& key = item.first;
            if (match_names(pathComponent, key.getName()))
                matchingKeys.push_back(key);
        });

        // Second pass: Modify matching nodes with proper locking
        for (const auto& key : matchingKeys) {
            nodeDataMap.modify_if(key, [&](auto& nodePair) {
                if (auto* nodeData = std::get_if<NodeData>(&nodePair.second)) {
                    if (auto error = nodeData->serialize(inputData); error.has_value())
                        ret.errors.emplace_back(error.value());
                    if (inputData.taskCreator)
                        ret.nbrTasksInserted++;
                    else
                        ret.nbrValuesInserted++;
                }
            });
        }
    } else {
        nodeDataMap.try_emplace_l(
                pathComponent,
                [&](auto& value) {
                    if (auto* nodeData = std::get_if<NodeData>(&value.second))
                        if (auto error = nodeData->serialize(inputData); error.has_value())
                            ret.errors.emplace_back(error.value());
                },
                inputData);
        if (inputData.taskCreator)
            ret.nbrTasksInserted++;
        else
            ret.nbrValuesInserted++;
    }
}

auto PathSpaceLeaf::inIntermediateComponent(PathIterator const& iter, InputData const& inputData, InsertReturn& ret) -> void {
    GlobName const& pathComponent = iter.currentComponent();
    auto const      nextIter      = iter.next();
    if (pathComponent.isGlob()) {
        nodeDataMap.for_each([&](const auto& item) {
            const auto& key = item.first;
            if (pathComponent.match(key)) {
                if (const auto* leaf = std::get_if<std::unique_ptr<PathSpaceLeaf>>(&item.second)) {
                    (*leaf)->in(nextIter, inputData, ret);
                }
            }
        });
    } else {
        auto [it, inserted] = nodeDataMap.try_emplace(pathComponent.getName(), std::make_unique<PathSpaceLeaf>());
        if (auto* leaf = std::get_if<std::unique_ptr<PathSpaceLeaf>>(&it->second)) { // ToDo Is this really thread safe?
            (*leaf)->in(nextIter, inputData, ret);
        }
    }
}

/*
    ############# Out #############
*/
auto PathSpaceLeaf::out(PathIterator const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> std::optional<Error> {
    if (iter.isAtFinalComponent())
        return outFinalComponent(iter, inputMetadata, obj, doExtract);
    else
        return outIntermediateComponent(iter, inputMetadata, obj, doExtract);
}

auto PathSpaceLeaf::outFinalComponent(PathIterator const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> std::optional<Error> {
    std::optional<Error> result        = Error{Error::Code::NoSuchPath, "Path not found"};
    bool                 shouldErase   = false;
    auto                 componentName = iter.currentComponent();
    if (is_glob(componentName)) {
        bool found = false;
        nodeDataMap.for_each([&](const auto& item) {
            const auto& key = item.first;
            if (!found && match_names(componentName, key.getName())) {
                componentName = key.getName();
                found         = true;
            }
        });
    }

    // First pass: modify data and check if we need to erase
    this->nodeDataMap.modify_if(componentName, [&](auto& nodePair) {
        if (auto* nodeData = std::get_if<NodeData>(&nodePair.second)) {
            if (doExtract) {
                result      = nodeData->deserializePop(obj, inputMetadata);
                shouldErase = nodeData->empty();
            } else {
                result = nodeData->deserialize(obj, inputMetadata);
            }
        }
    });

    // Second pass: if needed, erase the empty node
    if (shouldErase)
        this->nodeDataMap.erase(componentName);

    return result;
}

auto PathSpaceLeaf::outIntermediateComponent(PathIterator const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> std::optional<Error> {
    std::optional<Error> result = Error{Error::Code::NoSuchPath, "Path not found"};
    this->nodeDataMap.if_contains(iter.currentComponent(), [&](auto const& nodePair) {
        bool const isLeaf = std::holds_alternative<std::unique_ptr<PathSpaceLeaf>>(nodePair.second);
        result            = isLeaf
                                    ? std::get<std::unique_ptr<PathSpaceLeaf>>(nodePair.second)->out(iter.next(), inputMetadata, obj, doExtract)
                                    : Error{Error::Code::InvalidPathSubcomponent, "Sub-component name is data"};
    });
    return result;
}

} // namespace SP