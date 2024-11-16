#include "PathSpaceLeaf.hpp"
#include "core/Error.hpp"
#include "core/InOptions.hpp"
#include "core/InsertReturn.hpp"
#include "core/OutOptions.hpp"
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

auto PathSpaceLeaf::in(PathViewGlob const& iter, InputData const& inputData, InOptions const& options, InsertReturn& ret) -> void {
    sp_log("PathSpaceLeaf::in Processing path component: " + std::string(iter.currentComponent().getName()), "PathSpaceLeaf");
    if (iter.isFinalComponent())
        inFinalComponent(iter.currentComponent(), inputData, options, ret);
    else
        inIntermediateComponent(iter.current(), iter.end(), iter.currentComponent(), inputData, options, ret);
}

auto PathSpaceLeaf::inFinalComponent(GlobName const& pathComponent, InputData const& inputData, InOptions const& options, InsertReturn& ret) -> void {
    if (pathComponent.isGlob()) {
        // Create a vector to store the keys that match before modification.
        std::vector<ConcreteNameString> matchingKeys;

        // First pass: Collect all matching keys without holding write locks
        nodeDataMap.for_each([&](auto& item) {
            const auto& key = item.first;
            if (std::get<0>(pathComponent.match(key)))
                matchingKeys.push_back(key);
        });

        // Second pass: Modify matching nodes with proper locking
        for (const auto& key : matchingKeys) {
            nodeDataMap.modify_if(key, [&](auto& nodePair) {
                if (auto* nodeData = std::get_if<NodeData>(&nodePair.second)) {
                    if (auto error = nodeData->serialize(inputData, options); error.has_value())
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
                        if (auto error = nodeData->serialize(inputData, options); error.has_value())
                            ret.errors.emplace_back(error.value());
                },
                NodeData{inputData, options});
        if (inputData.taskCreator)
            ret.nbrTasksInserted++;
        else
            ret.nbrValuesInserted++;
    }
}

auto PathSpaceLeaf::inIntermediateComponent(GlobPathIteratorStringView const& iter, GlobPathIteratorStringView const& end, GlobName const& pathComponent, InputData const& inputData, InOptions const& options, InsertReturn& ret) -> void {
    auto nextIter = std::next(iter);

    if (pathComponent.isGlob()) {
        nodeDataMap.for_each([&](const auto& item) {
            const auto& key = item.first;
            if (std::get<0>(pathComponent.match(key))) {
                if (const auto* leaf = std::get_if<std::unique_ptr<PathSpaceLeaf>>(&item.second)) {
                    (*leaf)->in(PathViewGlob(nextIter, end), inputData, options, ret);
                }
            }
        });
    } else {
        auto [it, inserted] = nodeDataMap.try_emplace(pathComponent.getName(), std::make_unique<PathSpaceLeaf>());
        if (auto* leaf = std::get_if<std::unique_ptr<PathSpaceLeaf>>(&it->second)) {
            (*leaf)->in(PathViewGlob(nextIter, end), inputData, options, ret);
        }
    }
}

/*
    ############# Out #############
*/

auto PathSpaceLeaf::out(ConcretePathIteratorStringView const& iter, ConcretePathIteratorStringView const& end, InputMetadata const& inputMetadata, void* obj, OutOptions const& options, bool const isExtract) -> Expected<int> {
    auto const nextIter      = std::next(iter);
    auto const pathComponent = *iter;
    return nextIter == end ? outDataName(pathComponent, end, inputMetadata, obj, options, isExtract) : outConcretePathComponent(nextIter, end, pathComponent, inputMetadata, obj, options, isExtract);
}

auto PathSpaceLeaf::outDataName(ConcreteNameStringView const& concreteName, ConcretePathIteratorStringView const& end, InputMetadata const& inputMetadata, void* obj, OutOptions const& options, bool const isExtract) -> Expected<int> {
    Expected<int> result      = std::unexpected(Error{Error::Code::NoSuchPath, "Path not found"});
    bool          shouldErase = false;

    // First pass: modify data and check if we need to erase
    this->nodeDataMap.modify_if(concreteName.getName(), [&](auto& nodePair) {
        if (auto* nodeData = std::get_if<NodeData>(&nodePair.second)) {
            if (isExtract) {
                result      = nodeData->deserializePop(obj, inputMetadata);
                shouldErase = nodeData->empty();
            } else {
                result = nodeData->deserialize(obj, inputMetadata, options);
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

auto PathSpaceLeaf::outConcretePathComponent(ConcretePathIteratorStringView const& nextIter,
                                             ConcretePathIteratorStringView const& end,
                                             ConcreteNameStringView const&         concreteName,
                                             InputMetadata const&                  inputMetadata,
                                             void*                                 obj,
                                             OutOptions const&                     options,
                                             bool const                            isExtract) -> Expected<int> {
    Expected<int> expected = std::unexpected(Error{Error::Code::NoSuchPath, "Path not found"});
    this->nodeDataMap.if_contains(concreteName.getName(), [&](auto const& nodePair) {
        expected = std::holds_alternative<std::unique_ptr<PathSpaceLeaf>>(nodePair.second) ? std::get<std::unique_ptr<PathSpaceLeaf>>(nodePair.second)->out(nextIter, end, inputMetadata, obj, options, isExtract)
                                                                                           : std::unexpected(Error{Error::Code::InvalidPathSubcomponent, "Sub-component name is data"});
    });
    return expected;
}

} // namespace SP