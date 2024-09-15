#include "PathSpace.hpp"
#include "path/ConcretePath.hpp"
#include "pathspace/type/InputData.hpp"

namespace SP {

PathSpace::PathSpace(TaskPool* pool_)
    : pool(pool_ == nullptr ? &TaskPool::Instance() : pool_) {
}

/************************************************
******************** Insert *********************
*************************************************/

auto PathSpace::insertInternal(ConstructiblePath& path,
                               GlobPathIteratorStringView const& iter,
                               GlobPathIteratorStringView const& end,
                               InputData const& inputData,
                               InsertOptions const& options,
                               InsertReturn& ret) -> void {
    std::next(iter) == end
            ? insertFinalComponent(path, *iter, inputData, options, ret)
            : insertIntermediateComponent(path, iter, end, *iter, inputData, options, ret);
}

auto PathSpace::insertFinalComponent(ConstructiblePath& path, GlobName const& pathComponent, InputData const& inputData, InsertOptions const& options, InsertReturn& ret)
        -> void {
    path.append(pathComponent.getName());
    pathComponent.isGlob()
            ? insertGlobDataName(path, pathComponent, inputData, options, ret)
            : insertConcreteDataName(path, pathComponent.getName(), inputData, options, ret);
}

auto PathSpace::insertIntermediateComponent(ConstructiblePath& path,
                                            GlobPathIteratorStringView const& iter,
                                            GlobPathIteratorStringView const& end,
                                            GlobName const& pathComponent,
                                            InputData const& inputData,
                                            InsertOptions const& options,
                                            InsertReturn& ret) -> void {
    path.append(pathComponent.getName());
    pathComponent.isGlob()
            ? insertGlobPathComponent(path, iter, end, pathComponent, inputData, options, ret)
            : insertConcretePathComponent(path, iter, end, pathComponent.getName(), inputData, options, ret);
}

auto PathSpace::insertConcreteDataName(ConstructiblePath& path, ConcreteName const& concreteName, InputData const& inputData, InsertOptions const& options, InsertReturn& ret)
        -> void {
    path.append(concreteName.getName());
    auto const appendDataIfNameExists = [&inputData, &options, &ret, path, this](auto& nodePair) {
        if (auto const error = std::get<NodeData>(nodePair.second).serialize(path, inputData, options, this->pool, ret); error.has_value())
            ret.errors.emplace_back(error.value());
    };
    auto const createNodeDataAndAppendDataToItIfNameDoesNotExists = [&concreteName, &inputData, &options, &ret, &path, this](NodeDataHashMap::constructor const& constructor) {
        NodeData nodeData{};
        if (auto const error = nodeData.serialize(path, inputData, options, this->pool, ret); error.has_value())
            ret.errors.emplace_back(error.value());
        constructor(concreteName, std::move(nodeData));
    };
    this->nodeDataMap.lazy_emplace_l(concreteName, appendDataIfNameExists, createNodeDataAndAppendDataToItIfNameDoesNotExists);
    ret.nbrValuesInserted++;
}

auto PathSpace::insertGlobDataName(ConstructiblePath& path, GlobName const& globName, InputData const& inputData, InsertOptions const& options, InsertReturn& ret) -> void {
    for (auto const& val : this->nodeDataMap) {
        if (std::get<0>(globName.match(val.first))) {
            ConstructiblePath path2 = path;
            insertConcreteDataName(path2, val.first, inputData, options, ret);
        }
    }
}

auto PathSpace::insertGlobPathComponent(ConstructiblePath& path,
                                        GlobPathIteratorStringView const& iter,
                                        GlobPathIteratorStringView const& end,
                                        GlobName const& globName,
                                        InputData const& inputData,
                                        InsertOptions const& options,
                                        InsertReturn& ret) -> void {
    for (auto const& val : this->nodeDataMap) {
        if (std::get<0>(globName.match(val.first))) {
            if (std::holds_alternative<std::unique_ptr<PathSpace>>(val.second)) {
                ConstructiblePath path2 = path;
                path2.append(val.first.getName());
                std::get<std::unique_ptr<PathSpace>>(val.second)
                        ->insertInternal(path2, std::next(iter), end, inputData, options, ret);
            }
        }
    }
}

auto PathSpace::insertConcretePathComponent(ConstructiblePath& path,
                                            GlobPathIteratorStringView const& iter,
                                            GlobPathIteratorStringView const& end,
                                            ConcreteName const& concreteName,
                                            InputData const& inputData,
                                            InsertOptions const& options,
                                            InsertReturn& ret) -> void {
    auto const nextIter = std::next(iter);
    path.append(concreteName.getName());
    auto const appendDataIfNameExists = [&](auto& nodePair) {
        if (std::holds_alternative<std::unique_ptr<PathSpace>>(nodePair.second))
            std::get<std::unique_ptr<PathSpace>>(nodePair.second)->insertInternal(path, nextIter, end, inputData, options, ret);
        else
            ret.errors.emplace_back(Error::Code::InvalidPathSubcomponent, std::string("Sub-component name is data for ").append(concreteName.getName()));
    };
    auto const createNodeDataAndAppendDataToItIfNameDoesNotExists = [&](NodeDataHashMap::constructor const& constructor) {
        auto space = std::make_unique<PathSpace>(this->pool);
        space->insertInternal(path, nextIter, end, inputData, options, ret);
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
        expected = std::holds_alternative<std::unique_ptr<PathSpace>>(nodePair.second) ? std::get<std::unique_ptr<PathSpace>>(nodePair.second)->readInternal(nextIter, end, inputMetadata, obj, options) : std::unexpected(Error{Error::Code::InvalidPathSubcomponent, "Sub-component name is data"});
    });
    return expected;
}

/************************************************
******************** Extract ***********************
*************************************************/

auto PathSpace::extractInternal(ConcretePathIteratorStringView const& iter,
                                ConcretePathIteratorStringView const& end,
                                InputMetadata const& inputMetadata,
                                void* obj,
                                Capabilities const& capabilities) -> Expected<int> {
    auto const nextIter = std::next(iter);
    auto const pathComponent = *iter;
    if (nextIter == end) {
        return extractDataName(pathComponent, nextIter, end, inputMetadata, obj, capabilities);
    }
    return extractConcretePathComponent(nextIter, end, pathComponent.getName(), inputMetadata, obj, capabilities);
}

auto PathSpace::extractDataName(ConcreteName const& concreteName,
                                ConcretePathIteratorStringView const& nextIter,
                                ConcretePathIteratorStringView const& end,
                                InputMetadata const& inputMetadata,
                                void* obj,
                                Capabilities const& capabilities) -> Expected<int> {
    Expected<int> expected = std::unexpected(Error{Error::Code::NoSuchPath, "Path not found"});
    this->nodeDataMap.modify_if(concreteName, [&](auto& nodePair) { expected = std::get<NodeData>(nodePair.second).deserializePop(obj, inputMetadata); });
    return expected;
}

auto PathSpace::extractConcretePathComponent(ConcretePathIteratorStringView const& nextIter,
                                             ConcretePathIteratorStringView const& end,
                                             ConcreteName const& concreteName,
                                             InputMetadata const& inputMetadata,
                                             void* obj,
                                             Capabilities const& capabilities) -> Expected<int> {
    Expected<int> expected = std::unexpected(Error{Error::Code::NoSuchPath, "Path not found"});
    this->nodeDataMap.if_contains(concreteName, [&](auto const& nodePair) {
        expected = std::holds_alternative<std::unique_ptr<PathSpace>>(nodePair.second) ? std::get<std::unique_ptr<PathSpace>>(nodePair.second)->extractInternal(nextIter, end, inputMetadata, obj, capabilities) : std::unexpected(Error{Error::Code::InvalidPathSubcomponent, "Sub-component name is data"});
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