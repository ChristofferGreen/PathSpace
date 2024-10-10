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
                       WaitMap& waitMap) -> void {
    std::next(iter) == end ? inFinalComponent(path, *iter, inputData, options, ret, waitMap)
                           : inIntermediateComponent(path, iter, end, *iter, inputData, options, ret, waitMap);
}

auto PathSpaceLeaf::out(ConcretePathIteratorStringView const& iter,
                        ConcretePathIteratorStringView const& end,
                        InputMetadata const& inputMetadata,
                        void* obj,
                        OutOptions const& options,
                        Capabilities const& capabilities,
                        WaitMap& waitMap) -> Expected<int> {
    auto const nextIter = std::next(iter);
    auto const pathComponent = *iter;
    return nextIter == end
                   ? outDataName(pathComponent, nextIter, end, inputMetadata, obj, options, capabilities, waitMap)
                   : outConcretePathComponent(nextIter, end, pathComponent.getName(), inputMetadata, obj, options, capabilities, waitMap);
}

auto PathSpaceLeaf::inFinalComponent(ConstructiblePath& path,
                                     GlobName const& pathComponent,
                                     InputData const& inputData,
                                     InOptions const& options,
                                     InsertReturn& ret,
                                     WaitMap& waitMap) -> void {
    path.append(pathComponent.getName());
    pathComponent.isGlob() ? inGlobDataName(path, pathComponent, inputData, options, ret, waitMap)
                           : inConcreteDataName(path, pathComponent.getName(), inputData, options, ret, waitMap);
}

auto PathSpaceLeaf::inIntermediateComponent(ConstructiblePath& path,
                                            GlobPathIteratorStringView const& iter,
                                            GlobPathIteratorStringView const& end,
                                            GlobName const& pathComponent,
                                            InputData const& inputData,
                                            InOptions const& options,
                                            InsertReturn& ret,
                                            WaitMap& waitMap) -> void {
    path.append(pathComponent.getName());
    pathComponent.isGlob() ? inGlobPathComponent(path, iter, end, pathComponent, inputData, options, ret, waitMap)
                           : inConcretePathComponent(path, iter, end, pathComponent.getName(), inputData, options, ret, waitMap);
}

static auto process_waiters_for_insert(std::string_view const& path, WaitMap& waitMap) -> void {
    // Notify the waitMap that the nodeData is ready
    std::unique_ptr<WaitEntry> entry;
    std::unique_lock<std::mutex> lock;
    waitMap.erase_if(path, [&entry, &lock](auto& nodePair) {
        std::unique_lock<std::mutex> lockInner(nodePair.second->mutex);
        entry = std::move(nodePair.second);
        lock = std::move(lockInner);
        return true;
    });
    if (entry) {
        entry->cv.notify_all();
        entry->cv.wait(lock, [&entry] {
            return entry->active_threads == 0;
        }); // Must wait for all threads to wake up before erasing the condition variable
    }
}

auto PathSpaceLeaf::inConcreteDataName(ConstructiblePath& path,
                                       ConcreteName const& concreteName,
                                       InputData const& inputData,
                                       InOptions const& options,
                                       InsertReturn& ret,
                                       WaitMap& waitMap) -> void {
    path.append(concreteName.getName());
    auto const appendDataIfNameExists = [&inputData, &options, &ret, &waitMap, path, this](auto& nodePair) {
        if (auto const error = std::get<NodeData>(nodePair.second).serialize(path, inputData, options, ret); error.has_value())
            ret.errors.emplace_back(error.value());
    };

    auto const createNodeDataAndAppendDataToItIfNameDoesNotExists
            = [&concreteName, &inputData, &options, &ret, &path, &waitMap, this](NodeDataHashMap::constructor const& constructor) {
                  NodeData nodeData{};
                  if (auto const error = nodeData.serialize(path, inputData, options, ret); error.has_value()) {
                      ret.errors.emplace_back(error.value());
                      return;
                  }
                  constructor(concreteName, std::move(nodeData));
              };
    this->nodeDataMap.lazy_emplace_l(concreteName, appendDataIfNameExists, createNodeDataAndAppendDataToItIfNameDoesNotExists);
    process_waiters_for_insert(path.getPath(), waitMap);
    ret.nbrValuesInserted++;
}

auto PathSpaceLeaf::inGlobDataName(ConstructiblePath& path,
                                   GlobName const& globName,
                                   InputData const& inputData,
                                   InOptions const& options,
                                   InsertReturn& ret,
                                   WaitMap& waitMap) -> void {
    for (auto const& val : this->nodeDataMap) {
        if (std::get<0>(globName.match(val.first))) {
            ConstructiblePath pathCopy = path;
            inConcreteDataName(pathCopy, val.first, inputData, options, ret, waitMap);
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
                                        WaitMap& waitMap) -> void {
    auto const nextIter = std::next(iter);
    for (auto const& val : this->nodeDataMap) {
        if (std::get<0>(globName.match(val.first))) {
            if (std::holds_alternative<std::unique_ptr<PathSpaceLeaf>>(val.second)) {
                ConstructiblePath pathCopy = path;
                pathCopy.append(val.first.getName());
                std::get<std::unique_ptr<PathSpaceLeaf>>(val.second)->in(pathCopy, nextIter, end, inputData, options, ret, waitMap);
            }
        }
    }
}

auto PathSpaceLeaf::inConcretePathComponent(ConstructiblePath& path,
                                            GlobPathIteratorStringView const& iter,
                                            GlobPathIteratorStringView const& end,
                                            ConcreteName const& concreteName,
                                            InputData const& inputData,
                                            InOptions const& options,
                                            InsertReturn& ret,
                                            WaitMap& waitMap) -> void {
    auto const nextIter = std::next(iter);
    path.append(concreteName.getName());
    auto const appendDataIfNameExists = [&](auto& nodePair) {
        if (std::holds_alternative<std::unique_ptr<PathSpaceLeaf>>(nodePair.second))
            std::get<std::unique_ptr<PathSpaceLeaf>>(nodePair.second)->in(path, nextIter, end, inputData, options, ret, waitMap);
        else
            ret.errors.emplace_back(Error::Code::InvalidPathSubcomponent,
                                    std::string("Sub-component name is data for ").append(concreteName.getName()));
    };
    auto const createNodeDataAndAppendDataToItIfNameDoesNotExists = [&](NodeDataHashMap::constructor const& constructor) {
        auto space = std::make_unique<PathSpaceLeaf>();
        space->in(path, nextIter, end, inputData, options, ret, waitMap);
        constructor(concreteName, std::move(space));
        ret.nbrSpacesInserted++;
    };
    this->nodeDataMap.lazy_emplace_l(concreteName, appendDataIfNameExists, createNodeDataAndAppendDataToItIfNameDoesNotExists);
}

auto PathSpaceLeaf::outDataName(ConcreteName const& concreteName,
                                ConcretePathIteratorStringView const& nextIter,
                                ConcretePathIteratorStringView const& end,
                                InputMetadata const& inputMetadata,
                                void* obj,
                                OutOptions const& options,
                                Capabilities const& capabilities,
                                WaitMap& waitMap) -> Expected<int> {
    Expected<int> expected = std::unexpected(Error{Error::Code::NoSuchPath, "Path not found"});
    if (options.doPop) { // ToDo: If it's the last item then should we erase the whole final path component (erase_if)?
        this->nodeDataMap.modify_if(concreteName, [&](auto& nodePair) {
            expected = std::get<NodeData>(nodePair.second).deserializePop(obj, inputMetadata);
        });
    } else {
        this->nodeDataMap.if_contains(concreteName, [&](auto const& nodePair) {
            expected = std::get<NodeData>(nodePair.second).deserialize(obj, inputMetadata, options.execution);
        });
    }
    if (!expected.has_value() && options.block.has_value()
        && options.block.value().behavior != BlockOptions::Behavior::DontWait) { // ToDo: More fine grained waiting options
        /*std::unique_ptr<WaitEntry> entry = std::make_unique<WaitEntry>();
        entry->active_threads++;
        entry->mutex.lock();
        waitMap.emplace(std::make_pair(path, std::move(entry)));*/
    }
    return expected;
}

auto PathSpaceLeaf::outConcretePathComponent(ConcretePathIteratorStringView const& nextIter,
                                             ConcretePathIteratorStringView const& end,
                                             ConcreteName const& concreteName,
                                             InputMetadata const& inputMetadata,
                                             void* obj,
                                             OutOptions const& options,
                                             Capabilities const& capabilities,
                                             WaitMap& waitMap) -> Expected<int> {
    Expected<int> expected = std::unexpected(Error{Error::Code::NoSuchPath, "Path not found"});
    this->nodeDataMap.if_contains(concreteName, [&](auto const& nodePair) {
        expected = std::holds_alternative<std::unique_ptr<PathSpaceLeaf>>(nodePair.second)
                           ? std::get<std::unique_ptr<PathSpaceLeaf>>(nodePair.second)
                                     ->out(nextIter, end, inputMetadata, obj, options, capabilities, waitMap)
                           : std::unexpected(Error{Error::Code::InvalidPathSubcomponent, "Sub-component name is data"});
    });
    return expected;
}

} // namespace SP