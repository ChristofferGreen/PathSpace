#include "PathSpace.hpp"
#include "PathSpaceLeaf.hpp"
#include "pathspace/type/InputData.hpp"

namespace SP {

PathSpaceBase::PathSpaceBase(TaskPool* pool_) : pool(pool_ == nullptr ? &TaskPool::Instance() : pool_) {
}

/************************************************
******************** Insert *********************
*************************************************/

auto PathSpaceBase::inInternal(ConstructiblePath& path,
                               GlobPathIteratorStringView const& iter,
                               GlobPathIteratorStringView const& end,
                               InputData const& inputData,
                               InOptions const& options,
                               InsertReturn& ret) -> void {
    std::next(iter) == end ? inFinalComponent(path, *iter, inputData, options, ret)
                           : inIntermediateComponent(path, iter, end, *iter, inputData, options, ret);
}

auto PathSpaceBase::inFinalComponent(ConstructiblePath& path,
                                     GlobName const& pathComponent,
                                     InputData const& inputData,
                                     InOptions const& options,
                                     InsertReturn& ret) -> void {
    path.append(pathComponent.getName());
    pathComponent.isGlob() ? inGlobDataName(path, pathComponent, inputData, options, ret)
                           : inConcreteDataName(path, pathComponent.getName(), inputData, options, ret);
}

auto PathSpaceBase::inIntermediateComponent(ConstructiblePath& path,
                                            GlobPathIteratorStringView const& iter,
                                            GlobPathIteratorStringView const& end,
                                            GlobName const& pathComponent,
                                            InputData const& inputData,
                                            InOptions const& options,
                                            InsertReturn& ret) -> void {
    path.append(pathComponent.getName());
    pathComponent.isGlob() ? inGlobPathComponent(path, iter, end, pathComponent, inputData, options, ret)
                           : inConcretePathComponent(path, iter, end, pathComponent.getName(), inputData, options, ret);
}

auto PathSpaceBase::inConcreteDataName(ConstructiblePath& path,
                                       ConcreteName const& concreteName,
                                       InputData const& inputData,
                                       InOptions const& options,
                                       InsertReturn& ret) -> void {
    path.append(concreteName.getName());
    auto const appendDataIfNameExists = [&inputData, &options, &ret, path, this](auto& nodePair) {
        if (auto const error = std::get<NodeData>(nodePair.second).serialize(path, inputData, options, this->pool, ret); error.has_value())
            ret.errors.emplace_back(error.value());
    };
    auto const createNodeDataAndAppendDataToItIfNameDoesNotExists
            = [&concreteName, &inputData, &options, &ret, &path, this](NodeDataHashMap::constructor const& constructor) {
                  NodeData nodeData{};
                  if (auto const error = nodeData.serialize(path, inputData, options, this->pool, ret); error.has_value()) {
                      ret.errors.emplace_back(error.value());
                      return;
                  }
                  constructor(concreteName, std::move(nodeData));
              };
    this->nodeDataMap.lazy_emplace_l(concreteName, appendDataIfNameExists, createNodeDataAndAppendDataToItIfNameDoesNotExists);
    ret.nbrValuesInserted++;
}

auto PathSpaceBase::inGlobDataName(ConstructiblePath& path,
                                   GlobName const& globName,
                                   InputData const& inputData,
                                   InOptions const& options,
                                   InsertReturn& ret) -> void {
    for (auto const& val : this->nodeDataMap) {
        if (std::get<0>(globName.match(val.first))) {
            ConstructiblePath path2 = path;
            inConcreteDataName(path2, val.first, inputData, options, ret);
        }
    }
}

auto PathSpaceBase::inGlobPathComponent(ConstructiblePath& path,
                                        GlobPathIteratorStringView const& iter,
                                        GlobPathIteratorStringView const& end,
                                        GlobName const& globName,
                                        InputData const& inputData,
                                        InOptions const& options,
                                        InsertReturn& ret) -> void {
    for (auto const& val : this->nodeDataMap) {
        if (std::get<0>(globName.match(val.first))) {
            if (std::holds_alternative<std::unique_ptr<PathSpaceLeaf>>(val.second)) {
                ConstructiblePath path2 = path;
                path2.append(val.first.getName());
                std::get<std::unique_ptr<PathSpaceLeaf>>(val.second)->inInternal(path2, std::next(iter), end, inputData, options, ret);
            }
        }
    }
}

auto PathSpaceBase::inConcretePathComponent(ConstructiblePath& path,
                                            GlobPathIteratorStringView const& iter,
                                            GlobPathIteratorStringView const& end,
                                            ConcreteName const& concreteName,
                                            InputData const& inputData,
                                            InOptions const& options,
                                            InsertReturn& ret) -> void {
    auto const nextIter = std::next(iter);
    path.append(concreteName.getName());
    auto const appendDataIfNameExists = [&](auto& nodePair) {
        if (std::holds_alternative<std::unique_ptr<PathSpaceLeaf>>(nodePair.second))
            std::get<std::unique_ptr<PathSpaceLeaf>>(nodePair.second)->inInternal(path, nextIter, end, inputData, options, ret);
        else
            ret.errors.emplace_back(Error::Code::InvalidPathSubcomponent,
                                    std::string("Sub-component name is data for ").append(concreteName.getName()));
    };
    auto const createNodeDataAndAppendDataToItIfNameDoesNotExists = [&](NodeDataHashMap::constructor const& constructor) {
        auto space = std::make_unique<PathSpaceLeaf>(this->pool);
        space->inInternal(path, nextIter, end, inputData, options, ret);
        constructor(concreteName, std::move(space));
        ret.nbrSpacesInserted++;
    };
    this->nodeDataMap.lazy_emplace_l(concreteName, appendDataIfNameExists, createNodeDataAndAppendDataToItIfNameDoesNotExists);
}

/************************************************
******************** Extract ********************
*************************************************/

auto PathSpaceBase::outInternal(ConcretePathIteratorStringView const& iter,
                                ConcretePathIteratorStringView const& end,
                                InputMetadata const& inputMetadata,
                                void* obj,
                                OutOptions const& options,
                                Capabilities const& capabilities) -> Expected<int> {
    auto const nextIter = std::next(iter);
    auto const pathComponent = *iter;
    return nextIter == end ? outDataName(pathComponent, nextIter, end, inputMetadata, obj, options, capabilities)
                           : outConcretePathComponent(nextIter, end, pathComponent.getName(), inputMetadata, obj, options, capabilities);
}

auto PathSpaceBase::outDataName(ConcreteName const& concreteName,
                                ConcretePathIteratorStringView const& nextIter,
                                ConcretePathIteratorStringView const& end,
                                InputMetadata const& inputMetadata,
                                void* obj,
                                OutOptions const& options,
                                Capabilities const& capabilities) -> Expected<int> {
    Expected<int> expected = std::unexpected(Error{Error::Code::NoSuchPath, "Path not found"});
    if (options.doPop) {
        this->nodeDataMap.modify_if(concreteName, [&](auto& nodePair) {
            expected = std::get<NodeData>(nodePair.second).deserializePop(obj, inputMetadata);
        });
    } else {
        this->nodeDataMap.if_contains(concreteName, [&](auto const& nodePair) {
            expected = std::get<NodeData>(nodePair.second).deserialize(obj, inputMetadata, this->pool, options.execution);
        });
    }
    return expected;
}

auto PathSpaceBase::outConcretePathComponent(ConcretePathIteratorStringView const& nextIter,
                                             ConcretePathIteratorStringView const& end,
                                             ConcreteName const& concreteName,
                                             InputMetadata const& inputMetadata,
                                             void* obj,
                                             OutOptions const& options,
                                             Capabilities const& capabilities) -> Expected<int> {
    Expected<int> expected = std::unexpected(Error{Error::Code::NoSuchPath, "Path not found"});
    if (options.doPop) {
        this->nodeDataMap.if_contains(concreteName, [&](auto const& nodePair) {
            expected = std::holds_alternative<std::unique_ptr<PathSpaceLeaf>>(nodePair.second)
                               ? std::get<std::unique_ptr<PathSpaceLeaf>>(nodePair.second)
                                         ->outInternal(nextIter, end, inputMetadata, obj, options, capabilities)
                               : std::unexpected(Error{Error::Code::InvalidPathSubcomponent, "Sub-component name is data"});
        });
    } else {
        this->nodeDataMap.if_contains(concreteName, [&](auto const& nodePair) {
            expected = std::holds_alternative<std::unique_ptr<PathSpaceLeaf>>(nodePair.second)
                               ? std::get<std::unique_ptr<PathSpaceLeaf>>(nodePair.second)
                                         ->outInternal(nextIter, end, inputMetadata, obj, options, capabilities)
                               : std::unexpected(Error{Error::Code::InvalidPathSubcomponent, "Sub-component name is data"});
        });
    }
    return expected;
}

} // namespace SP