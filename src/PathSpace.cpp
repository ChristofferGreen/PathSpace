#include "pathspace/PathSpace.hpp"

#include <sstream>

namespace SP {

/************************************************
******************** Insert *********************
*************************************************/

auto PathSpace::insertInternal(GlobPathIteratorStringView const &iter,
                               GlobPathIteratorStringView const &end,
                               InputData const &inputData,
                               InsertOptions const &options) -> Expected<int> {
    auto const pathComponent = *iter;
    if(std::next(iter) == end) // This is the end of the path, attempt to insert the data
        return pathComponent.isGlob() ? this->insertGlobDataName(pathComponent, inputData, options) :
                                        this->insertConcreteDataName(pathComponent.getName(), inputData, options);
    return pathComponent.isGlob() ? this->insertGlobPathComponent(iter, end, pathComponent, inputData, options) : // Send along down the line to all matching the glob expression
                                    this->insertConcretePathComponent(iter, end, pathComponent.getName(), inputData, options); // This sub-component is a concrete path
}

auto PathSpace::insertConcreteDataName(ConcreteName const &concreteName,
                               InputData const &inputData,
                               InsertOptions const &options) -> Expected<int> {
    auto const appendDataIfNameExists = [&inputData](auto &nodePair){
        std::get<NodeData>(nodePair.second).serialize(inputData);
    };
    auto const createNodeDataAndAppendDataToItIfNameDoesNotExists = [&concreteName, &inputData](NodeDataHashMap::constructor const &constructor){
        NodeData nodeData{};
        nodeData.serialize(inputData);
        constructor(concreteName, std::move(nodeData));
    };
    this->nodeDataMap.lazy_emplace_l(concreteName, appendDataIfNameExists, createNodeDataAndAppendDataToItIfNameDoesNotExists);
    return 1;
}

static auto parse_result(Expected<int> &expected, Expected<int> const &result) -> Expected<int> {
    assert(expected.has_value());
    if(result.has_value()) {
        expected.value() += result.value();
        return true;
    }    
    else {
        expected = result; // No support for multiple errors
        return false;
    }
}

/*
The behaviour of only inserting partially when an error happens is a bit crap.
Perhaps better to insert as many as possible and also report how many could not be inserted due to error as well as which could not be inserted (path).
*/
auto PathSpace::insertGlobDataName(GlobName const &globName,
                                   InputData const &inputData,
                                   InsertOptions const &options) -> Expected<int> {
    Expected<int> expected;
    for (auto const &val : this->nodeDataMap) {
        if(std::get<0>(globName.match(val.first))) {
            if(!parse_result(expected, this->insertConcreteDataName(val.first, inputData, options)))
                return expected; // Early exit due to error
        }
    }
    return expected;
}

auto PathSpace::insertGlobPathComponent(GlobPathIteratorStringView const &iter,
                                        GlobPathIteratorStringView const &end,
                                        GlobName const &globName,
                                        InputData const &inputData,
                                        InsertOptions const &options) -> Expected<int> {
    Expected<int> expected;
    for (auto const &val : this->nodeDataMap) {
        if(std::get<0>(globName.match(val.first))) {
            if(std::holds_alternative<std::unique_ptr<PathSpace>>(val.second)) {
                if(!parse_result(expected, std::get<std::unique_ptr<PathSpace>>(val.second)->insertInternal(std::next(iter), end, inputData, options)))
                    return expected; // Early exit due to error
            }
        }
    }
    return expected;
}

auto PathSpace::insertConcretePathComponent(GlobPathIteratorStringView const &iter,
                                            GlobPathIteratorStringView const &end,
                                            ConcreteName const &concreteName,
                                            InputData const &inputData,
                                            InsertOptions const &options) -> Expected<int> {
    auto const nextIter = std::next(iter);
    Expected<int> expected;
    auto const appendDataIfNameExists = [&](auto &nodePair){
        expected = std::holds_alternative<std::unique_ptr<PathSpace>>(nodePair.second) ?
            std::get<std::unique_ptr<PathSpace>>(nodePair.second)->insertInternal(nextIter, end, inputData, options) :
            std::unexpected(Error{Error::Code::InvalidPathSubcomponent, "Sub-component name is data"});
    };
    auto const createNodeDataAndAppendDataToItIfNameDoesNotExists = [&](NodeDataHashMap::constructor const &constructor){
        auto space = std::make_unique<PathSpace>();
        expected = space->insertInternal(nextIter, end, inputData, options);
        constructor(concreteName, std::move(space));
    };
    this->nodeDataMap.lazy_emplace_l(concreteName, appendDataIfNameExists, createNodeDataAndAppendDataToItIfNameDoesNotExists);
    return expected;
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