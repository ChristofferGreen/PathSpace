#include "Leaf.hpp"
#include "PathSpaceBase.hpp"
#include "core/Error.hpp"
#include "core/InsertReturn.hpp"
#include "path/Iterator.hpp"
#include "path/utils.hpp"
#include "type/InputData.hpp"
#include <memory>

namespace SP {

auto Leaf::clear() -> void {
    this->nodeDataMap.clear();
}

/*
    ############# In #############
*/

auto Leaf::in(Iterator const& iter, InputData const& inputData, InsertReturn& ret) -> void {
    sp_log("Leaf::in2 Processing path component: " + std::string(iter.currentComponent()), "Leaf");
    if (iter.isAtFinalComponent())
        inFinalComponent(iter, inputData, ret);
    else
        inIntermediateComponent(iter, inputData, ret);
}

auto Leaf::inFinalComponent(Iterator const& iter, InputData const& inputData, InsertReturn& ret) -> void {
    auto const pathComponent = iter.currentComponent();
    if (is_glob(pathComponent)) {
        if (inputData.metadata.dataCategory == DataCategory::UniquePtr) {
            ret.errors.emplace_back(Error::Code::InvalidType, "PathSpaces cannot be added in glob expressions.");
            return;
        }
        // Create a vector to store the keys that match before modification.
        std::vector<std::string_view> matchingKeys;

        // First pass: Collect all matching keys without holding write locks
        nodeDataMap.for_each([&](auto const& item) {
            auto const& key = item.first;
            if (match_names(pathComponent, key))
                matchingKeys.push_back(key);
        });

        // Second pass: Modify matching nodes with proper locking
        for (const auto& key : matchingKeys) {
            nodeDataMap.modify_if(key, [&](auto& nodePair) {
                if (auto* nodeData = std::get_if<NodeData>(&nodePair.second)) {
                    if (auto error = nodeData->serialize(inputData); error.has_value())
                        ret.errors.emplace_back(error.value());
                    if (inputData.task)
                        ret.nbrTasksInserted++;
                    else
                        ret.nbrValuesInserted++;
                }
            });
        }
    } else {
        bool success = true;
        if (inputData.metadata.dataCategory == DataCategory::UniquePtr) {
            nodeDataMap.try_emplace_l(
                    pathComponent,
                    [&](auto& value) {
                        ret.errors.emplace_back(Error::Code::InvalidType, "PathSpaces cannot be added in glob expressions.");
                        success = false;
                    },
                    std::move(*static_cast<std::unique_ptr<PathSpaceBase>*>(inputData.obj)));
        } else {
            nodeDataMap.try_emplace_l(
                    pathComponent,
                    [&](auto& value) {
                        if (auto* nodeData = std::get_if<NodeData>(&value.second)) {
                            if (auto error = nodeData->serialize(inputData); error.has_value())
                                ret.errors.emplace_back(error.value());
                        }
                    },
                    NodeData(inputData));
        }
        if (inputData.task)
            ret.nbrTasksInserted++;
        else if (success)
            ret.nbrValuesInserted++;
    }
}

auto Leaf::inIntermediateComponent(Iterator const& iter, InputData const& inputData, InsertReturn& ret) -> void {
    auto const pathComponent = iter.currentComponent();
    auto const nextIter      = iter.next();
    if (is_glob(pathComponent)) {
        nodeDataMap.for_each([&](const auto& item) {
            const auto& key = item.first;
            if (match_names(pathComponent, key)) {
                if (const auto* leaf = std::get_if<std::unique_ptr<Leaf>>(&item.second)) {
                    (*leaf)->in(nextIter, inputData, ret);
                }
            }
        });
    } else {
        auto [it, inserted] = nodeDataMap.try_emplace(pathComponent, std::make_unique<Leaf>());
        if (auto* leaf = std::get_if<std::unique_ptr<Leaf>>(&it->second)) { // ToDo Is this really thread safe?
            (*leaf)->in(nextIter, inputData, ret);
        }
    }
}

/*
    ############# Out #############
*/
auto Leaf::out(Iterator const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> std::optional<Error> {
    if (iter.isAtFinalComponent())
        return outFinalComponent(iter, inputMetadata, obj, doExtract);
    else
        return outIntermediateComponent(iter, inputMetadata, obj, doExtract);
}

auto Leaf::outFinalComponent(Iterator const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> std::optional<Error> {
    std::optional<Error> result        = Error{Error::Code::NoSuchPath, "Path not found"};
    bool                 shouldErase   = false;
    auto                 componentName = iter.currentComponent();
    if (is_glob(componentName)) {
        bool found = false;
        nodeDataMap.for_each([&](const auto& item) {
            const auto& key = item.first;
            if (!found && match_names(componentName, key)) {
                componentName = key;
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
        if (auto* nodeData = std::get_if<std::unique_ptr<PathSpaceBase>>(&nodePair.second)) {
            int a = 0;
            ++a;
        }
    });

    // Second pass: if needed, erase the empty node
    if (shouldErase)
        this->nodeDataMap.erase(componentName);

    return result;
}

auto Leaf::outIntermediateComponent(Iterator const& iter, InputMetadata const& inputMetadata, void* obj, bool const doExtract) -> std::optional<Error> {
    std::optional<Error> result = Error{Error::Code::NoSuchPath, "Path not found"};
    this->nodeDataMap.if_contains(iter.currentComponent(), [&](auto const& nodePair) {
        if (std::holds_alternative<std::unique_ptr<Leaf>>(nodePair.second)) {
            result = std::get<std::unique_ptr<Leaf>>(nodePair.second)->out(iter.next(), inputMetadata, obj, doExtract);
        } else if (std::holds_alternative<std::unique_ptr<PathSpaceBase>>(nodePair.second)) {
            result = std::get<std::unique_ptr<PathSpaceBase>>(nodePair.second)->outMinimal(iter.next(), inputMetadata, obj, doExtract);
        } else {
            result = Error{Error::Code::InvalidPathSubcomponent, "Sub-component name is data"};
        }
    });
    return result;
}
} // namespace SP