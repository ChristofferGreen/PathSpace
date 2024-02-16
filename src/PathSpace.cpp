#include "pathspace/PathSpace.hpp"

#include <cereal/archives/json.hpp>
#include <sstream>

namespace SP {

auto PathSpace::insertInternal(GlobPathIteratorStringView const &iter,
                               GlobPathIteratorStringView const &end,
                               InputData const &inputData,
                               Capabilities const &capabilities,
                               TimeToLive const &ttl) -> Expected<int> {
    auto const nextIter = std::next(iter);
    auto const pathComponent = *iter;
    if(nextIter == end) // This is the end of the path, attempt to insert the data
        return this->insertDataName(pathComponent.getName(), inputData, capabilities, ttl);
    else if(pathComponent.isGlob()) // Send along down the line to all matching the glob expression
        return this->insertGlobPathComponent(iter, end, pathComponent, inputData, capabilities, ttl);
    return this->insertConcretePathComponent(iter, end, pathComponent.getName(), inputData, capabilities, ttl); // This sub-component is a concrete path
}

auto PathSpace::insertDataName(ConcreteName const &concreteName,
                               InputData const &inputData,
                               Capabilities const &capabilities,
                               TimeToLive const &ttl) -> Expected<int> {
    auto const appendDataIfNameExists = [&inputData](auto &nodePair){
        inputData.serialize(std::get<NodeData>(nodePair.second).data);
    };
    auto const createNodeDataAndAppendDataToItIfNameDoesNotExists = [&concreteName, &inputData](NodeDataHashMap::constructor const &constructor){
        NodeData nodeData{};
        inputData.serialize(nodeData.data);
        constructor(concreteName, std::move(nodeData));
    };
    this->nodeDataMap.lazy_emplace_l(concreteName, appendDataIfNameExists, createNodeDataAndAppendDataToItIfNameDoesNotExists);
    return 1;
}

auto PathSpace::insertConcretePathComponent(GlobPathIteratorStringView const &iter,
                                            GlobPathIteratorStringView const &end,
                                            ConcreteName const &concreteName,
                                            InputData const &inputData,
                                            Capabilities const &capabilities,
                                            TimeToLive const &ttl) -> Expected<int> {
    auto const nextIter = std::next(iter);
    Expected<int> expected;
    auto const appendDataIfNameExists = [&](auto &nodePair){
        expected = std::holds_alternative<std::unique_ptr<PathSpace>>(nodePair.second) ?
            std::get<std::unique_ptr<PathSpace>>(nodePair.second)->insertInternal(nextIter, end, inputData, capabilities, ttl) :
            std::unexpected(Error{Error::Code::InvalidPathSubcomponent, "Sub-component name is data"});
    };
    auto const createNodeDataAndAppendDataToItIfNameDoesNotExists = [&](NodeDataHashMap::constructor const &constructor){
        auto space = std::make_unique<PathSpace>();
        expected = space->insertInternal(nextIter, end, inputData, capabilities, ttl);
        constructor(concreteName, std::move(space));
    };
    this->nodeDataMap.lazy_emplace_l(concreteName, appendDataIfNameExists, createNodeDataAndAppendDataToItIfNameDoesNotExists);
    return expected;
}

auto PathSpace::insertGlobPathComponent(GlobPathIteratorStringView const &iter,
                                        GlobPathIteratorStringView const &end,
                                        GlobName const &name,
                                        InputData const &inputData,
                                        Capabilities const &capabilities,
                                        TimeToLive const &ttl) -> Expected<int> {
    auto const nextIter = std::next(iter);
    int nbrInserted{};
    return nbrInserted;
}

auto PathSpace::readInternal(ConcretePathIteratorStringView const &iter,
                             ConcretePathIteratorStringView const &end,
                             InputMetadata const &inputMetadata,
                             void *obj,
                             Capabilities const &capabilities) const -> Expected<int> {
    auto const nextIter = std::next(iter);
    auto const pathComponent = *iter;
    if(nextIter == end) // This is the end of the path, attempt to insert the data
        return this->readDataName(pathComponent, nextIter, end, inputMetadata, obj, capabilities);
    /*return this->readComponent(iter, nextIter, end, pathComponent.getName(), inputData, capabilities, ttl); // This sub-component is a concrete path
    */
   return 0;
}

auto PathSpace::readDataName(ConcreteName const &concreteName,
                             ConcretePathIteratorStringView const &nextIter,
                             ConcretePathIteratorStringView const &end,
                             InputMetadata const &inputMetadata,
                             void *obj,
                             Capabilities const &capabilities) const -> Expected<int> {
    this->nodeDataMap.if_contains(concreteName, [&](auto const &nodePair){
        // if type matches
        inputMetadata.deserializationFuncPtr2(obj, std::get<NodeData>(nodePair.second).data);
    });
    return 0;
}

/*
                this->nodes.if_contains(concreteName, [&](auto &nodePair){
                    auto returnedValue = nodePair.second->insertInternal(nextIter, end, inputData, capabilities, ttl);
                    if(!returnedValue.has_value())
                        return std::unexpected(returnedValue.error());
                    nbrInserted += returnedValue.value();
                });
*/

/*
    for(auto &nodePair : this->nodeDataMap) { // Is this multi threading safe?
        if(name == nodePair.first) {
            auto const returnedValue = std::get<std::unique_ptr<PathSpace>>(nodePair.second)->insertInternal(nextIter, end, inputData, capabilities, ttl); // what if its not a space??
            if(!returnedValue.has_value())
                return std::unexpected(returnedValue.error());
            nbrInserted += returnedValue.value();
        }
    }
*/

auto PathSpace::toJSON(bool const isHumanReadable) const -> std::string {
    /*std::stringstream ss;
    cereal::JSONOutputArchive archive(ss, isHumanReadable ? cereal::JSONOutputArchive::Options::Default() : 
                                                            cereal::JSONOutputArchive::Options::NoIndent());
    archive(cereal::make_nvp("PathSpace", *this));
    auto json = ss.str();
    if(!isHumanReadable)
        json.erase(std::remove(json.begin(), json.end(), '\n'), json.cend());
    return json;*/
    return "";
}

} // namespace SP