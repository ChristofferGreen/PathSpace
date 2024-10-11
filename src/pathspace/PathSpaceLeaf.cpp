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

auto PathSpaceLeaf::in(ConstructiblePath& path,
                       GlobPathIteratorStringView const& iter,
                       GlobPathIteratorStringView const& end,
                       InputData const& inputData,
                       InOptions const& options,
                       InsertReturn& ret,
                       std::mutex& mutex) -> void {
    std::next(iter) == end ? inFinalComponent(path, *iter, inputData, options, ret, mutex)
                           : inIntermediateComponent(path, iter, end, *iter, inputData, options, ret, mutex);
}

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

auto PathSpaceLeaf::inFinalComponent(ConstructiblePath& path,
                                     GlobName const& pathComponent,
                                     InputData const& inputData,
                                     InOptions const& options,
                                     InsertReturn& ret,
                                     std::mutex& mutex) -> void {
    path.append(pathComponent.getName());
    pathComponent.isGlob() ? inGlobDataName(path, pathComponent, inputData, options, ret, mutex)
                           : inConcreteDataName(path, pathComponent.getName(), inputData, options, ret, mutex);
}

auto PathSpaceLeaf::inIntermediateComponent(ConstructiblePath& path,
                                            GlobPathIteratorStringView const& iter,
                                            GlobPathIteratorStringView const& end,
                                            GlobName const& pathComponent,
                                            InputData const& inputData,
                                            InOptions const& options,
                                            InsertReturn& ret,
                                            std::mutex& mutex) -> void {
    path.append(pathComponent.getName());
    pathComponent.isGlob() ? inGlobPathComponent(path, iter, end, pathComponent, inputData, options, ret, mutex)
                           : inConcretePathComponent(path, iter, end, pathComponent.getName(), inputData, options, ret, mutex);
}

auto PathSpaceLeaf::inConcreteDataName(ConstructiblePath& path,
                                       ConcreteNameStringView const& concreteName,
                                       InputData const& inputData,
                                       InOptions const& options,
                                       InsertReturn& ret,
                                       std::mutex& mutex) -> void {
    path.append(concreteName.getName());
    auto const appendDataIfNameExists = [&inputData, &options, &ret, path, this](auto& nodePair) {
        if (auto const error = std::get<NodeData>(nodePair.second).serialize(path, inputData, options, ret); error.has_value())
            ret.errors.emplace_back(error.value());
    };

    auto const createNodeDataAndAppendDataToItIfNameDoesNotExists
            = [&concreteName, &inputData, &options, &ret, &path, this](NodeDataHashMap::constructor const& constructor) {
                  NodeData nodeData{};
                  if (auto const error = nodeData.serialize(path, inputData, options, ret); error.has_value()) {
                      ret.errors.emplace_back(error.value());
                      return;
                  }
                  constructor(concreteName.getName(), std::move(nodeData));
              };
    std::unique_lock<std::mutex> lock(mutex);
    this->nodeDataMap.lazy_emplace_l(concreteName.getName(), appendDataIfNameExists, createNodeDataAndAppendDataToItIfNameDoesNotExists);
    ret.nbrValuesInserted++;
}

auto PathSpaceLeaf::inGlobDataName(ConstructiblePath& path,
                                   GlobName const& globName,
                                   InputData const& inputData,
                                   InOptions const& options,
                                   InsertReturn& ret,
                                   std::mutex& mutex) -> void {
    for (auto const& val : this->nodeDataMap) {
        if (std::get<0>(globName.match(val.first))) {
            ConstructiblePath pathCopy = path;
            inConcreteDataName(pathCopy, val.first.getName(), inputData, options, ret, mutex);
        }
    }
}

auto PathSpaceLeaf::inGlobPathComponent(ConstructiblePath& path,
                                        GlobPathIteratorStringView const& iter,
                                        GlobPathIteratorStringView const& end,
                                        GlobName const& globName,
                                        InputData const& inputData,
                                        InOptions const& options,
                                        InsertReturn& ret,
                                        std::mutex& mutex) -> void {
    auto const nextIter = std::next(iter);
    for (auto const& val : this->nodeDataMap) {
        if (std::get<0>(globName.match(val.first))) {
            if (std::holds_alternative<std::unique_ptr<PathSpaceLeaf>>(val.second)) {
                ConstructiblePath pathCopy = path;
                pathCopy.append(val.first.getName());
                std::get<std::unique_ptr<PathSpaceLeaf>>(val.second)->in(pathCopy, nextIter, end, inputData, options, ret, mutex);
            }
        }
    }
}

auto PathSpaceLeaf::inConcretePathComponent(ConstructiblePath& path,
                                            GlobPathIteratorStringView const& iter,
                                            GlobPathIteratorStringView const& end,
                                            ConcreteNameStringView const& concreteName,
                                            InputData const& inputData,
                                            InOptions const& options,
                                            InsertReturn& ret,
                                            std::mutex& mutex) -> void {
    auto const nextIter = std::next(iter);
    path.append(concreteName.getName());
    auto const appendDataIfNameExists = [&](auto& nodePair) {
        if (std::holds_alternative<std::unique_ptr<PathSpaceLeaf>>(nodePair.second))
            std::get<std::unique_ptr<PathSpaceLeaf>>(nodePair.second)->in(path, nextIter, end, inputData, options, ret, mutex);
        else
            ret.errors.emplace_back(Error::Code::InvalidPathSubcomponent,
                                    std::string("Sub-component name is data for ").append(concreteName.getName()));
    };
    auto const createNodeDataAndAppendDataToItIfNameDoesNotExists = [&](NodeDataHashMap::constructor const& constructor) {
        auto space = std::make_unique<PathSpaceLeaf>();
        space->in(path, nextIter, end, inputData, options, ret, mutex);
        constructor(concreteName.getName(), std::move(space));
        ret.nbrSpacesInserted++;
    };
    this->nodeDataMap.lazy_emplace_l(concreteName.getName(), appendDataIfNameExists, createNodeDataAndAppendDataToItIfNameDoesNotExists);
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