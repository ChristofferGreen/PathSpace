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
        nodeDataMap.for_each([&](const auto& item) {
            const auto& key = item.first;
            if (std::get<0>(pathComponent.match(key))) {
                if (const auto* nodeData = std::get_if<NodeData>(&item.second)) {
                    if (auto error = const_cast<NodeData*>(nodeData)->serialize(path, inputData, options, ret); error.has_value()) {
                        ret.errors.emplace_back(error.value());
                    }
                    ret.nbrValuesInserted++;
                }
            }
        });
    } else {
        auto [it, inserted] = nodeDataMap.try_emplace(pathComponent.getName(), NodeData{});
        if (auto* nodeData = std::get_if<NodeData>(&it->second)) {
            if (auto error = nodeData->serialize(path, inputData, options, ret); error.has_value()) {
                ret.errors.emplace_back(error.value());
            }
            ret.nbrValuesInserted++;
        }
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
    Expected<int> expected = std::unexpected(Error{Error::Code::NoSuchPath, "Path not found"});
    if (options.doPop) { // ToDo: If it's the last item then should we erase the whole final path component (erase_if)?
        this->nodeDataMap.modify_if(concreteName.getName(), [&](auto& nodePair) {
            expected = std::get<NodeData>(nodePair.second).deserializePop(obj, inputMetadata);
        });
    } else {
        this->nodeDataMap.if_contains(concreteName.getName(), [&](auto const& nodePair) {
            expected = std::get<NodeData>(nodePair.second).deserialize(obj, inputMetadata, options.execution);
        });
    }
    return expected;
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