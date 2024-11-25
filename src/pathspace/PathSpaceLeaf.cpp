#include "PathSpaceLeaf.hpp"
#include "core/Error.hpp"
#include "core/In.hpp"
#include "core/InsertReturn.hpp"
#include "core/Out.hpp"
#include "path/PathView.hpp"
#include "pathspace/type/InputData.hpp"
#include "type/InputData.hpp"

namespace SP {

auto PathSpaceLeaf::clear() -> void {
    this->nodeDataMap.clear();
}

/*
    ############# In #############
*/
auto PathSpaceLeaf::in(PathViewGlob const& iter, InputData const& inputData, In const& options, InsertReturn& ret) -> void {
    sp_log("PathSpaceLeaf::in Processing path component: " + std::string(iter.currentComponent().getName()), "PathSpaceLeaf");
    if (iter.isFinalComponent())
        inFinalComponent(iter, inputData, options, ret);
    else
        inIntermediateComponent(iter, inputData, options, ret);
}

auto PathSpaceLeaf::inFinalComponent(PathViewGlob const& iter, InputData const& inputData, In const& options, InsertReturn& ret) -> void {
    auto const pathComponent = iter.currentComponent();
    if (pathComponent.isGlob()) {
        // Create a vector to store the keys that match before modification.
        std::vector<ConcreteNameString> matchingKeys;

        // First pass: Collect all matching keys without holding write locks
        nodeDataMap.for_each([&](auto& item) {
            const auto& key = item.first;
            if (pathComponent.match(key))
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
                pathComponent.getName(),
                [&](auto& value) {
                    if (auto* nodeData = std::get_if<NodeData>(&value.second))
                        if (auto error = nodeData->serialize(inputData); error.has_value())
                            ret.errors.emplace_back(error.value());
                },
                NodeData{inputData}); // ToDo: Find a way not to always create a NodeData here
        if (inputData.taskCreator)
            ret.nbrTasksInserted++;
        else
            ret.nbrValuesInserted++;
    }
}

auto PathSpaceLeaf::inIntermediateComponent(PathViewGlob const& iter, InputData const& inputData, In const& options, InsertReturn& ret) -> void {
    GlobName const& pathComponent = iter.currentComponent();
    auto const      nextIter      = iter.next();
    if (pathComponent.isGlob()) {
        nodeDataMap.for_each([&](const auto& item) {
            const auto& key = item.first;
            if (pathComponent.match(key)) {
                if (const auto* leaf = std::get_if<std::unique_ptr<PathSpaceLeaf>>(&item.second)) {
                    (*leaf)->in(nextIter, inputData, options, ret);
                }
            }
        });
    } else {
        auto [it, inserted] = nodeDataMap.try_emplace(pathComponent.getName(), std::make_unique<PathSpaceLeaf>());
        if (auto* leaf = std::get_if<std::unique_ptr<PathSpaceLeaf>>(&it->second)) { // ToDo Is this really thread safe?
            (*leaf)->in(nextIter, inputData, options, ret);
        }
    }
}

/*
    ############# Out #############
*/
auto PathSpaceLeaf::out(PathViewConcrete const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> Expected<int> {
    if (iter.isFinalComponent())
        return outFinalComponent(iter, inputMetadata, obj, doExtract);
    else
        return outIntermediateComponent(iter, inputMetadata, obj, doExtract);
}

auto PathSpaceLeaf::outFinalComponent(PathViewConcrete const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> Expected<int> {
    Expected<int> result       = std::unexpected(Error{Error::Code::NoSuchPath, "Path not found"});
    bool          shouldErase  = false;
    auto const    concreteName = iter.currentComponent();

    // First pass: modify data and check if we need to erase
    this->nodeDataMap.modify_if(concreteName.getName(), [&](auto& nodePair) {
        if (auto* nodeData = std::get_if<NodeData>(&nodePair.second)) {
            if (doExtract) {
                result      = nodeData->deserializePop(obj, inputMetadata);
                shouldErase = nodeData->empty();
            } else {
                result = nodeData->deserialize(obj, inputMetadata);
            }
            return true; // modification was successful
        }
        return false; // no modification needed
    });

    // Second pass: if needed, erase the empty node
    if (shouldErase)
        this->nodeDataMap.erase(concreteName.getName());

    return result;
}

auto PathSpaceLeaf::outIntermediateComponent(PathViewConcrete const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> Expected<int> {
    Expected<int> expected = std::unexpected(Error{Error::Code::NoSuchPath, "Path not found"});
    this->nodeDataMap.if_contains(iter.currentComponent().getName(), [&](auto const& nodePair) {
        bool const isLeaf = std::holds_alternative<std::unique_ptr<PathSpaceLeaf>>(nodePair.second);
        expected          = isLeaf
                                    ? std::get<std::unique_ptr<PathSpaceLeaf>>(nodePair.second)->out(iter.next(), inputMetadata, obj, doExtract)
                                    : std::unexpected(Error{Error::Code::InvalidPathSubcomponent, "Sub-component name is data"});
    });
    return expected;
}

} // namespace SP