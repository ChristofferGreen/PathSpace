#include "PathSpace.hpp"
#include "pathspace/type/InputData.hpp"

namespace SP {

PathSpace::PathSpace(TaskPool* pool_) : pool(pool_ == nullptr ? &TaskPool::Instance() : pool_) {
}

/************************************************
******************** Insert *********************
*************************************************/

auto PathSpace::insertInternal(GlobPathIteratorStringView const& iter,
                               GlobPathIteratorStringView const& end,
                               InputData const& inputData,
                               InsertOptions const& options,
                               InsertReturn& ret) -> void {
    auto const pathComponent = *iter;
    if (std::next(iter) == end) {
        insertFinalComponent(pathComponent, inputData, options, ret);
    } else {
        insertIntermediateComponent(iter, end, pathComponent, inputData, options, ret);
    }
}

auto PathSpace::insertFinalComponent(GlobName const& pathComponent, InputData const& inputData, InsertOptions const& options, InsertReturn& ret) -> void {
    if (pathComponent.isGlob()) {
        insertGlobDataName(pathComponent, inputData, options, ret);
    } else {
        insertConcreteDataName(pathComponent.getName(), inputData, options, ret);
    }
}

auto PathSpace::insertIntermediateComponent(GlobPathIteratorStringView const& iter,
                                            GlobPathIteratorStringView const& end,
                                            GlobName const& pathComponent,
                                            InputData const& inputData,
                                            InsertOptions const& options,
                                            InsertReturn& ret) -> void {
    if (pathComponent.isGlob()) {
        insertGlobPathComponent(iter, end, pathComponent, inputData, options, ret);
    } else {
        insertConcretePathComponent(iter, end, pathComponent.getName(), inputData, options, ret);
    }
}

auto PathSpace::insertConcreteDataName(ConcreteName const& concreteName, InputData const& inputData, InsertOptions const& options, InsertReturn& ret) -> void {
    auto const appendDataIfNameExists = [&inputData, &options](auto& nodePair) { std::get<NodeData>(nodePair.second).serialize(inputData, options); };
    auto const createNodeDataAndAppendDataToItIfNameDoesNotExists = [&concreteName, &inputData, &options, &ret](NodeDataHashMap::constructor const& constructor) {
        NodeData nodeData{};
        nodeData.serialize(inputData, options);
        if (inputData.metadata.serialize == nullptr) {
            ret.errors.emplace_back(Error::Code::UnserializableType, "Serialization function is null");
            return;
        }
        constructor(concreteName, std::move(nodeData));
    };
    this->nodeDataMap.lazy_emplace_l(concreteName, appendDataIfNameExists, createNodeDataAndAppendDataToItIfNameDoesNotExists);
    ret.nbrValuesInserted++;
}

auto PathSpace::insertGlobDataName(GlobName const& globName, InputData const& inputData, InsertOptions const& options, InsertReturn& ret) -> void {
    for (auto const& val : this->nodeDataMap) {
        if (std::get<0>(globName.match(val.first))) {
            insertConcreteDataName(val.first, inputData, options, ret);
        }
    }
}

auto PathSpace::insertGlobPathComponent(GlobPathIteratorStringView const& iter,
                                        GlobPathIteratorStringView const& end,
                                        GlobName const& globName,
                                        InputData const& inputData,
                                        InsertOptions const& options,
                                        InsertReturn& ret) -> void {
    for (auto const& val : this->nodeDataMap) {
        if (std::get<0>(globName.match(val.first))) {
            if (std::holds_alternative<std::unique_ptr<PathSpace>>(val.second)) {
                std::get<std::unique_ptr<PathSpace>>(val.second)->insertInternal(std::next(iter), end, inputData, options, ret);
            }
        }
    }
}

auto PathSpace::insertConcretePathComponent(GlobPathIteratorStringView const& iter,
                                            GlobPathIteratorStringView const& end,
                                            ConcreteName const& concreteName,
                                            InputData const& inputData,
                                            InsertOptions const& options,
                                            InsertReturn& ret) -> void {
    auto const nextIter = std::next(iter);
    auto const appendDataIfNameExists = [&](auto& nodePair) {
        if (std::holds_alternative<std::unique_ptr<PathSpace>>(nodePair.second)) {
            std::get<std::unique_ptr<PathSpace>>(nodePair.second)->insertInternal(nextIter, end, inputData, options, ret);
        } else {
            ret.errors.emplace_back(Error::Code::InvalidPathSubcomponent, std::string("Sub-component name is data for ").append(concreteName.getName()));
        }
    };
    auto const createNodeDataAndAppendDataToItIfNameDoesNotExists = [&](NodeDataHashMap::constructor const& constructor) {
        auto space = std::make_unique<PathSpace>();
        space->insertInternal(nextIter, end, inputData, options, ret);
        constructor(concreteName, std::move(space));
        ret.nbrSpacesInserted++;
    };
    this->nodeDataMap.lazy_emplace_l(concreteName, appendDataIfNameExists, createNodeDataAndAppendDataToItIfNameDoesNotExists);
}

/************************************************
******************** Read ***********************
*************************************************/

auto PathSpace::readInternal(ConcretePathIteratorStringView const& iter,
                             ConcretePathIteratorStringView const& end,
                             InputMetadata const& inputMetadata,
                             void* obj,
                             ReadOptions const& options) const -> Expected<int> {
    auto const nextIter = std::next(iter);
    auto const pathComponent = *iter;
    if (nextIter == end) {
        return readDataName(pathComponent, nextIter, end, inputMetadata, obj, options);
    }
    return readConcretePathComponent(nextIter, end, pathComponent.getName(), inputMetadata, obj, options);
}

auto PathSpace::readDataName(ConcreteName const& concreteName,
                             ConcretePathIteratorStringView const& nextIter,
                             ConcretePathIteratorStringView const& end,
                             InputMetadata const& inputMetadata,
                             void* obj,
                             ReadOptions const& options) const -> Expected<int> {
    Expected<int> expected = std::unexpected(Error{Error::Code::NoSuchPath, "Path not found"});
    this->nodeDataMap.if_contains(concreteName,
                                  [&](auto const& nodePair) { expected = std::get<NodeData>(nodePair.second).deserialize(obj, inputMetadata, this->pool, options.execution); });
    return expected;
}

auto PathSpace::readConcretePathComponent(ConcretePathIteratorStringView const& nextIter,
                                          ConcretePathIteratorStringView const& end,
                                          ConcreteName const& concreteName,
                                          InputMetadata const& inputMetadata,
                                          void* obj,
                                          ReadOptions const& options) const -> Expected<int> {
    Expected<int> expected = std::unexpected(Error{Error::Code::NoSuchPath, "Path not found"});
    this->nodeDataMap.if_contains(concreteName, [&](auto const& nodePair) {
        expected = std::holds_alternative<std::unique_ptr<PathSpace>>(nodePair.second)
                           ? std::get<std::unique_ptr<PathSpace>>(nodePair.second)->readInternal(nextIter, end, inputMetadata, obj, options)
                           : std::unexpected(Error{Error::Code::InvalidPathSubcomponent, "Sub-component name is data"});
    });
    return expected;
}

/************************************************
******************** Grab ***********************
*************************************************/

auto PathSpace::grabInternal(ConcretePathIteratorStringView const& iter,
                             ConcretePathIteratorStringView const& end,
                             InputMetadata const& inputMetadata,
                             void* obj,
                             Capabilities const& capabilities) -> Expected<int> {
    auto const nextIter = std::next(iter);
    auto const pathComponent = *iter;
    if (nextIter == end) {
        return grabDataName(pathComponent, nextIter, end, inputMetadata, obj, capabilities);
    }
    return grabConcretePathComponent(nextIter, end, pathComponent.getName(), inputMetadata, obj, capabilities);
}

auto PathSpace::grabDataName(ConcreteName const& concreteName,
                             ConcretePathIteratorStringView const& nextIter,
                             ConcretePathIteratorStringView const& end,
                             InputMetadata const& inputMetadata,
                             void* obj,
                             Capabilities const& capabilities) -> Expected<int> {
    Expected<int> expected = std::unexpected(Error{Error::Code::NoSuchPath, "Path not found"});
    this->nodeDataMap.modify_if(concreteName, [&](auto& nodePair) { expected = std::get<NodeData>(nodePair.second).deserializePop(obj, inputMetadata); });
    return expected;
}

auto PathSpace::grabConcretePathComponent(ConcretePathIteratorStringView const& nextIter,
                                          ConcretePathIteratorStringView const& end,
                                          ConcreteName const& concreteName,
                                          InputMetadata const& inputMetadata,
                                          void* obj,
                                          Capabilities const& capabilities) -> Expected<int> {
    Expected<int> expected = std::unexpected(Error{Error::Code::NoSuchPath, "Path not found"});
    this->nodeDataMap.if_contains(concreteName, [&](auto const& nodePair) {
        expected = std::holds_alternative<std::unique_ptr<PathSpace>>(nodePair.second)
                           ? std::get<std::unique_ptr<PathSpace>>(nodePair.second)->grabInternal(nextIter, end, inputMetadata, obj, capabilities)
                           : std::unexpected(Error{Error::Code::InvalidPathSubcomponent, "Sub-component name is data"});
    });
    return expected;
}

auto PathSpace::subscribe(GlobPathStringView const& path, std::function<void(const GlobPathStringView&)> callback, Capabilities const& capabilities) -> Expected<void> {
    // TODO: Implement subscription logic
    return std::unexpected(Error{Error::Code::UnknownError, "Subscription not implemented yet"});
}

template <typename DataType>
auto PathSpace::visit(GlobPathStringView const& path, std::function<void(DataType&)> visitor, Capabilities const& capabilities) -> Expected<void> {
    // TODO: Implement visitation logic
    return std::unexpected(Error{Error::Code::UnknownError, "Visitation not implemented yet"});
}

auto PathSpace::toJSON(bool const isHumanReadable) const -> std::string {
    // TODO: Implement JSON conversion
    return "{}"; // Return empty JSON object for now
}

} // namespace SP