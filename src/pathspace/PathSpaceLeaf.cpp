#include "PathSpaceLeaf.hpp"
#include "core/BlockOptions.hpp"
#include "core/Capabilities.hpp"
#include "core/Error.hpp"
#include "core/InOptions.hpp"
#include "core/InsertReturn.hpp"
#include "core/OutOptions.hpp"
#include "path/ConstructiblePath.hpp"
#include "pathspace/type/InputData.hpp"
#include "type/InputData.hpp"

namespace SP {

auto PathSpaceLeaf::clear() -> void {
    this->nodeDataMap.clear();
}

/*
    ############# In #############
*/

auto PathSpaceLeaf::in(ConstructiblePath& path,
                       GlobPathIteratorStringView const& iter,
                       GlobPathIteratorStringView const& end,
                       InputData const& inputData,
                       InOptions const& options,
                       InsertReturn& ret) -> void {
    std::next(iter) == end ? inFinalComponent(path, *iter, inputData, options, ret)
                           : inIntermediateComponent(path, iter, end, *iter, inputData, options, ret);
}

auto PathSpaceLeaf::inFinalComponent(ConstructiblePath& path,
                                     GlobName const& pathComponent,
                                     InputData const& inputData,
                                     InOptions const& options,
                                     InsertReturn& ret) -> void {
    path.append(pathComponent.getName());

    if (pathComponent.isGlob()) {
        // Create a vector to store the keys that match before modification
        std::vector<ConcreteNameString> matchingKeys;

        // First pass: Collect all matching keys without holding locks
        nodeDataMap.for_each([&](auto& item) {
            const auto& key = item.first;
            if (std::get<0>(pathComponent.match(key))) {
                matchingKeys.push_back(key);
            }
        });

        // Second pass: Modify matching nodes with proper locking
        for (const auto& key : matchingKeys) {
            nodeDataMap.modify_if(key, [&](auto& nodePair) {
                if (auto* nodeData = std::get_if<NodeData>(&nodePair.second)) {
                    if (auto error = nodeData->serialize(inputData, options, ret); error.has_value()) {
                        ret.errors.emplace_back(error.value());
                    }
                    ret.nbrValuesInserted++;
                    return true; // Indicate that modification occurred
                }
                return false; // No modification if it's not a NodeData
            });
        }
    } else {
        nodeDataMap.try_emplace_l(
                pathComponent.getName(),
                [&](auto& value) {
                    if (auto* nodeData = std::get_if<NodeData>(&value.second)) {
                        if (auto error = nodeData->serialize(inputData, options, ret); error.has_value()) {
                            ret.errors.emplace_back(error.value());
                        }
                    }
                },
                NodeData{inputData, options, ret});
        ret.nbrValuesInserted++;
    }
}

auto PathSpaceLeaf::inIntermediateComponent(ConstructiblePath& path,
                                            GlobPathIteratorStringView const& iter,
                                            GlobPathIteratorStringView const& end,
                                            GlobName const& pathComponent,
                                            InputData const& inputData,
                                            InOptions const& options,
                                            InsertReturn& ret) -> void {
    path.append(pathComponent.getName());
    auto nextIter = std::next(iter);

    if (pathComponent.isGlob()) {
        nodeDataMap.for_each([&](const auto& item) {
            const auto& key = item.first;
            if (std::get<0>(pathComponent.match(key))) {
                if (const auto* leaf = std::get_if<std::unique_ptr<PathSpaceLeaf>>(&item.second)) {
                    (*leaf)->in(path, nextIter, end, inputData, options, ret);
                }
            }
        });
    } else {
        auto [it, inserted] = nodeDataMap.try_emplace(pathComponent.getName(), std::make_unique<PathSpaceLeaf>());
        if (auto* leaf = std::get_if<std::unique_ptr<PathSpaceLeaf>>(&it->second)) {
            (*leaf)->in(path, nextIter, end, inputData, options, ret);
        }
    }
}

/*
    ############# Out #############
*/

auto PathSpaceLeaf::out(ConcretePathIteratorStringView const& iter,
                        ConcretePathIteratorStringView const& end,
                        InputMetadata const& inputMetadata,
                        void* obj,
                        OutOptions const& options,
                        Capabilities const& capabilities) -> Expected<int> {
    auto const nextIter = std::next(iter);
    auto const pathComponent = *iter;
    return nextIter == end ? outDataName(pathComponent, nextIter, end, inputMetadata, obj, options, capabilities)
                           : outConcretePathComponent(nextIter, end, pathComponent, inputMetadata, obj, options, capabilities);
}

auto PathSpaceLeaf::outDataName(ConcreteNameStringView const& concreteName,
                                ConcretePathIteratorStringView const& nextIter,
                                ConcretePathIteratorStringView const& end,
                                InputMetadata const& inputMetadata,
                                void* obj,
                                OutOptions const& options,
                                Capabilities const& capabilities) -> Expected<int> {
    Expected<int> result = std::unexpected(Error{Error::Code::NoSuchPath, "Path not found"});

    nodeDataMap.modify_if(concreteName.getName(), [&](auto& nodePair) {
        if (auto* nodeData = std::get_if<NodeData>(&nodePair.second)) {
            if (options.doPop) {
                result = nodeData->deserializePop(obj, inputMetadata);
            } else {
                result = nodeData->deserialize(obj, inputMetadata, options.execution);
            }
            return options.doPop; // Only modify (remove) if it's a pop operation
        }
        return false;
    });

    return result;
}

auto PathSpaceLeaf::outConcretePathComponent(ConcretePathIteratorStringView const& nextIter,
                                             ConcretePathIteratorStringView const& end,
                                             ConcreteNameStringView const& concreteName,
                                             InputMetadata const& inputMetadata,
                                             void* obj,
                                             OutOptions const& options,
                                             Capabilities const& capabilities) -> Expected<int> {
    Expected<int> expected = std::unexpected(Error{Error::Code::NoSuchPath, "Path not found"});
    this->nodeDataMap.if_contains(concreteName.getName(), [&](auto const& nodePair) {
        expected = std::holds_alternative<std::unique_ptr<PathSpaceLeaf>>(nodePair.second)
                           ? std::get<std::unique_ptr<PathSpaceLeaf>>(nodePair.second)
                                     ->out(nextIter, end, inputMetadata, obj, options, capabilities)
                           : std::unexpected(Error{Error::Code::InvalidPathSubcomponent, "Sub-component name is data"});
    });
    return expected;
}

} // namespace SP