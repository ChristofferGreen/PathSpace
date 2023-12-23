#include "pathspace/PathSpace.hpp"

namespace SP {

auto PathSpace::insertInternal(const GlobPathIteratorStringView &iter,
                               const GlobPathIteratorStringView &end,
                               const InputData &inputData,
                               const Capabilities &capabilities,
                               const TimeToLive &ttl) -> Expected<int> {
    int nbrInserted{};
    auto const nextIter = std::next(iter);
    auto const pathComponent = *iter;
    if(nextIter == end) {
        return this->insertDataName(pathComponent, inputData, capabilities, ttl);
    } else if(pathComponent.isGlob()) { // Send along down the line to all matching the glob expression
        return this->insertGlobName(iter, nextIter, end, pathComponent, inputData, capabilities, ttl);
    }
    return nbrInserted;
}

auto PathSpace::insertDataName(const GlobName &name,
                               const InputData &inputData,
                               const Capabilities &capabilities,
                               const TimeToLive &ttl) -> Expected<int> {
    auto const concreteName = ConcreteName{name.getName()};
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

auto PathSpace::insertGlobName(const GlobPathIteratorStringView &iter,
                               const GlobPathIteratorStringView &nextIter,
                               const GlobPathIteratorStringView &end,
                               const GlobName &name,
                               const InputData &inputData,
                               const Capabilities &capabilities,
                               const TimeToLive &ttl) -> Expected<int> {
    int nbrInserted{};
    for(auto &nodePair : this->nodeDataMap) { // Is this multi threading safe?
        if(name == nodePair.first) {
            auto const returnedValue = std::get<std::unique_ptr<PathSpace>>(nodePair.second)->insertInternal(nextIter, end, inputData, capabilities, ttl); // what if its not a space??
            if(!returnedValue.has_value())
                return std::unexpected(returnedValue.error());
            nbrInserted += returnedValue.value();
        }
    }
    return nbrInserted;
}

}