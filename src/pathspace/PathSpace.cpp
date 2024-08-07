#include "PathSpace.hpp"
#include "pathspace/type/InputData.hpp"

namespace SP {

/************************************************
******************** Insert *********************
*************************************************/

auto PathSpace::insertInternal(GlobPathIteratorStringView const &iter,
                               GlobPathIteratorStringView const &end,
                               InputData const &inputData,
                               InsertOptions const &options,
                               InsertReturn &ret) -> void {
    auto const pathComponent = *iter;
    if(std::next(iter) == end) // This is the end of the path, attempt to insert the data
        return pathComponent.isGlob() ? this->insertGlobDataName(pathComponent, inputData, options, ret) :
                                        this->insertConcreteDataName(pathComponent.getName(), inputData, options, ret);
    return pathComponent.isGlob() ? this->insertGlobPathComponent(iter, end, pathComponent, inputData, options, ret) : // Send along down the line to all matching the glob expression
                                    this->insertConcretePathComponent(iter, end, pathComponent.getName(), inputData, options, ret); // This sub-component is a concrete path
}

auto PathSpace::insertConcreteDataName(ConcreteName const &concreteName,
                               InputData const &inputData,
                               InsertOptions const &options,
                               InsertReturn &ret) -> void {
    auto const appendDataIfNameExists = [&inputData](auto &nodePair){
        std::get<NodeData>(nodePair.second).serialize(inputData);
    };
    auto const createNodeDataAndAppendDataToItIfNameDoesNotExists = [&concreteName, &inputData](NodeDataHashMap::constructor const &constructor){
        NodeData nodeData{};
        nodeData.serialize(inputData);
        assert(inputData.metadata.serialize != nullptr);
        constructor(concreteName, std::move(nodeData));
    };
    this->nodeDataMap.lazy_emplace_l(concreteName, appendDataIfNameExists, createNodeDataAndAppendDataToItIfNameDoesNotExists);
    ret.nbrValuesInserted++;
}

/*
The behaviour of only inserting partially when an error happens is a bit crap.
Perhaps better to insert as many as possible and also report how many could not be inserted due to error as well as which could not be inserted (path).
*/
auto PathSpace::insertGlobDataName(GlobName const &globName,
                                   InputData const &inputData,
                                   InsertOptions const &options,
                                   InsertReturn &ret) -> void {
    for (auto const &val : this->nodeDataMap)
        if(std::get<0>(globName.match(val.first)))
            this->insertConcreteDataName(val.first, inputData, options, ret);
}

auto PathSpace::insertGlobPathComponent(GlobPathIteratorStringView const &iter,
                                        GlobPathIteratorStringView const &end,
                                        GlobName const &globName,
                                        InputData const &inputData,
                                        InsertOptions const &options,
                                        InsertReturn &ret) -> void {
    InsertReturn expected;
    for (auto const &val : this->nodeDataMap)
        if(std::get<0>(globName.match(val.first)))
            if(std::holds_alternative<std::unique_ptr<PathSpace>>(val.second))
                std::get<std::unique_ptr<PathSpace>>(val.second)->insertInternal(std::next(iter), end, inputData, options, ret);
}

auto PathSpace::insertConcretePathComponent(GlobPathIteratorStringView const &iter,
                                            GlobPathIteratorStringView const &end,
                                            ConcreteName const &concreteName,
                                            InputData const &inputData,
                                            InsertOptions const &options,
                                            InsertReturn &ret) -> void {
    auto const nextIter = std::next(iter);
    auto const appendDataIfNameExists = [&](auto &nodePair){
        if(std::holds_alternative<std::unique_ptr<PathSpace>>(nodePair.second))
            std::get<std::unique_ptr<PathSpace>>(nodePair.second)->insertInternal(nextIter, end, inputData, options, ret);
        else
            ret.errors.emplace_back(Error::Code::InvalidPathSubcomponent, std::string("Sub-component name is data for ").append(concreteName.getName()));
    };
    auto const createNodeDataAndAppendDataToItIfNameDoesNotExists = [&](NodeDataHashMap::constructor const &constructor){
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

auto PathSpace::readInternal(ConcretePathIteratorStringView const &iter,
                             ConcretePathIteratorStringView const &end,
                             InputMetadata const &inputMetadata,
                             void *obj,
                             Capabilities const &capabilities) const -> Expected<int> {
    auto const nextIter = std::next(iter);
    auto const pathComponent = *iter;
    if(nextIter == end) // This is the end of the path, attempt to insert the data
        return this->readDataName(pathComponent, nextIter, end, inputMetadata, obj, capabilities);
    return this->readConcretePathComponent(nextIter, end, pathComponent.getName(), inputMetadata, obj, capabilities); // This sub-component is a concrete path
}

auto PathSpace::readDataName(ConcreteName const &concreteName,
                             ConcretePathIteratorStringView const &nextIter,
                             ConcretePathIteratorStringView const &end,
                             InputMetadata const &inputMetadata,
                             void *obj,
                             Capabilities const &capabilities) const -> Expected<int> {
    Expected<int> expected;
    this->nodeDataMap.if_contains(concreteName, [&](auto const &nodePair){
        expected = std::get<NodeData>(nodePair.second).deserialize(obj, inputMetadata);
    });
    return expected;
}

auto PathSpace::readConcretePathComponent(ConcretePathIteratorStringView const &nextIter,
                                          ConcretePathIteratorStringView const &end,
                                          ConcreteName const &concreteName,
                                          InputMetadata const &inputMetadata,
                                          void *obj,
                                          Capabilities const &capabilities) const -> Expected<int> {
    Expected<int> expected;
    this->nodeDataMap.if_contains(concreteName, [&](auto const &nodePair){
        expected = std::holds_alternative<std::unique_ptr<PathSpace>>(nodePair.second) ?
            std::get<std::unique_ptr<PathSpace>>(nodePair.second)->readInternal(nextIter, end, inputMetadata, obj, capabilities) :
            std::unexpected(Error{Error::Code::InvalidPathSubcomponent, "Sub-component name is data"});
    });
    return expected;
}

/************************************************
******************** Grab ***********************
*************************************************/

auto PathSpace::grabInternal(ConcretePathIteratorStringView const &iter,
                             ConcretePathIteratorStringView const &end,
                             InputMetadata const &inputMetadata,
                             void *obj,
                             Capabilities const &capabilities) -> Expected<int> {
    auto const nextIter = std::next(iter);
    auto const pathComponent = *iter;
    if(nextIter == end) // This is the end of the path, attempt to insert the data
        return this->grabDataName(pathComponent, nextIter, end, inputMetadata, obj, capabilities);
    return this->grabConcretePathComponent(nextIter, end, pathComponent.getName(), inputMetadata, obj, capabilities); // This sub-component is a concrete path
}

auto PathSpace::grabDataName(ConcreteName const &concreteName,
                             ConcretePathIteratorStringView const &nextIter,
                             ConcretePathIteratorStringView const &end,
                             InputMetadata const &inputMetadata,
                             void *obj,
                             Capabilities const &capabilities) -> Expected<int> {
    Expected<int> expected;
    this->nodeDataMap.modify_if(concreteName, [&](auto &nodePair){
        expected = std::get<NodeData>(nodePair.second).deserializePop(obj, inputMetadata);
    });
    return 1;
}

auto PathSpace::grabConcretePathComponent(ConcretePathIteratorStringView const &nextIter,
                                          ConcretePathIteratorStringView const &end,
                                          ConcreteName const &concreteName,
                                          InputMetadata const &inputMetadata,
                                          void *obj,
                                          Capabilities const &capabilities) -> Expected<int> {
    Expected<int> expected;
    this->nodeDataMap.if_contains(concreteName, [&](auto const &nodePair){
        expected = std::holds_alternative<std::unique_ptr<PathSpace>>(nodePair.second) ?
            std::get<std::unique_ptr<PathSpace>>(nodePair.second)->grabInternal(nextIter, end, inputMetadata, obj, capabilities) :
            std::unexpected(Error{Error::Code::InvalidPathSubcomponent, "Sub-component name is data"});
    });
    return expected;
}

auto PathSpace::toJSON(bool const isHumanReadable) const -> std::string {
    return "";
}

} // namespace SP