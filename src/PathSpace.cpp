#include "pathspace/PathSpace.hpp"

#include <cereal/archives/json.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/variant.hpp>
#include <cereal/types/memory.hpp>
#include <cereal/types/queue.hpp>
#include <sstream>

namespace SP {

auto PathSpace::insertInternal(const GlobPathIteratorStringView &iter,
                               const GlobPathIteratorStringView &end,
                               const InputData &inputData,
                               const Capabilities &capabilities,
                               const TimeToLive &ttl) -> Expected<int> {
    auto const nextIter = std::next(iter);
    auto const pathComponent = *iter;
    if(nextIter == end) // This is the end of the path, attempt to insert the data
        return this->insertDataName(pathComponent.getName(), inputData, capabilities, ttl);
    else if(pathComponent.isGlob()) // Send along down the line to all matching the glob expression
        return this->insertGlobPathComponent(iter, nextIter, end, pathComponent, inputData, capabilities, ttl);
    return this->insertConcretePathComponent(iter, nextIter, end, pathComponent.getName(), inputData, capabilities, ttl); // This sub-component is a concrete path
}

auto PathSpace::insertDataName(const ConcreteName &concreteName,
                               const InputData &inputData,
                               const Capabilities &capabilities,
                               const TimeToLive &ttl) -> Expected<int> {
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

auto PathSpace::insertConcretePathComponent(const GlobPathIteratorStringView &iter,
                                            const GlobPathIteratorStringView &nextIter,
                                            const GlobPathIteratorStringView &end,
                                            const ConcreteName &concreteName,
                                            const InputData &inputData,
                                            const Capabilities &capabilities,
                                            const TimeToLive &ttl) -> Expected<int> {
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

auto PathSpace::insertGlobPathComponent(const GlobPathIteratorStringView &iter,
                                        const GlobPathIteratorStringView &nextIter,
                                        const GlobPathIteratorStringView &end,
                                        const GlobName &name,
                                        const InputData &inputData,
                                        const Capabilities &capabilities,
                                        const TimeToLive &ttl) -> Expected<int> {
    int nbrInserted{};
    return nbrInserted;
}

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
    std::stringstream ss;
    cereal::JSONOutputArchive archive(ss, isHumanReadable ? cereal::JSONOutputArchive::Options::Default() : 
                                                            cereal::JSONOutputArchive::Options::NoIndent());
    archive(cereal::make_nvp("PathSpace", *this));
    auto json = ss.str();
    if(!isHumanReadable)
        json.erase(std::remove(json.begin(), json.end(), '\n'), json.cend());
    return json;
}

} // namespace SP