#pragma once

#include "core/Error.hpp"
#include "core/Capabilities.hpp"
#include "core/TimeToLive.hpp"
#include "core/ExecutionOptions.hpp"
#include "type/InputData.hpp"
#include "core/NodeData.hpp"
#include "type/Helper.hpp"

#include <chrono>
#include <expected>
#include <functional>
#include <iterator>
#include <memory>
#include <queue>
#include <string>
#include <vector>

namespace SP {

template<typename T>
using Expected = std::expected<T, Error>;

struct PathSpace {
    template<typename T>
    auto insert(GlobPathStringView const &path,
                T const &data,
                Capabilities const &capabilities = Capabilities::All(),
                TimeToLive const &ttl = {}) -> Expected<int> {
        // Check if path is valid.
        // Check if path actually has a final name subcomponent
        // Check capabilities
        // Implement TTL functionality
        return this->insertInternal(path.begin(), path.end(), InputData{data}, capabilities, ttl);
    }

    template<typename T>
    auto read(ConcretePathStringView const &path,
              Capabilities const &capabilities = Capabilities::All()) const -> Expected<T> {
        Expected<T> obj;
        obj.emplace();
        auto ret = this->readInternal(path.begin(), path.end(), InputMetadataT<T>{}, &obj.value(), capabilities);
        if(!ret)
            return std::unexpected{ret.error()};
        return obj;
    }

    template<typename T>
    auto grab(ConcretePathStringView const &path,
              Capabilities const &capabilities = Capabilities::All()) -> Expected<T> {
        Expected<T> obj;
        obj.emplace();
        auto ret = this->grabInternal(path.begin(), path.end(), InputMetadataT<T>{}, &obj.value(), capabilities);
        if(!ret)
            return std::unexpected{ret.error()};
        return obj;
    }

    auto subscribe(GlobPathStringView const &path,
                   std::function<void(const GlobPathStringView&)> callback,
                   Capabilities const &capabilities = Capabilities::All()) -> Expected<void>;

    template<typename T>
    auto visit(GlobPathStringView const &path,
               std::function<void(T&)> visitor,
               Capabilities const &capabilities = Capabilities::All()) -> Expected<void>;

    auto toJSON(bool const isHumanReadable) const -> std::string;

    template <class Archive>
    void serialize(Archive &ar) {
        ar(this->nodeDataMap);
    }    
private:
    auto insertInternal(GlobPathIteratorStringView const &iter,
                        GlobPathIteratorStringView const &end,
                        InputData const &inputData,
                        Capabilities const &capabilities,
                        TimeToLive const &ttl) -> Expected<int>;
    auto insertDataName(ConcreteName const &name,
                        InputData const &inputData,
                        Capabilities const &capabilities,
                        TimeToLive const &ttl) -> Expected<int>;
    auto insertConcretePathComponent(GlobPathIteratorStringView const &iter,
                                     GlobPathIteratorStringView const &end,
                                     ConcreteName const &name,
                                     InputData const &inputData,
                                     Capabilities const &capabilities,
                                     TimeToLive const &ttl) -> Expected<int>;
    auto insertGlobPathComponent(GlobPathIteratorStringView const &iter,
                                 GlobPathIteratorStringView const &end,
                                 GlobName const &name,
                                 InputData const &inputData,
                                 Capabilities const &capabilities,
                                 TimeToLive const &ttl) -> Expected<int>;

    auto readInternal(ConcretePathIteratorStringView const &iter,
                      ConcretePathIteratorStringView const &end,
                      InputMetadata const &inputMetadata,
                      void *obj,
                      Capabilities const &capabilities) const -> Expected<int>;
    auto readDataName(ConcreteName const &concreteName,
                      ConcretePathIteratorStringView const &nextIter,
                      ConcretePathIteratorStringView const &end,
                      InputMetadata const &inputMetadata,
                      void *obj,
                      Capabilities const &capabilities) const -> Expected<int>;
    auto readConcretePathComponent(ConcretePathIteratorStringView const &nextIter,
                                   ConcretePathIteratorStringView const &end,
                                   ConcreteName const &concreteName,
                                   InputMetadata const &inputMetadata,
                                   void *obj,
                                   Capabilities const &capabilities) const -> Expected<int>;
    auto grabInternal(ConcretePathIteratorStringView const &iter,
                      ConcretePathIteratorStringView const &end,
                      InputMetadata const &inputMetadata,
                      void *obj,
                      Capabilities const &capabilities) -> Expected<int>;
    auto grabDataName(ConcreteName const &concreteName,
                      ConcretePathIteratorStringView const &nextIter,
                      ConcretePathIteratorStringView const &end,
                      InputMetadata const &inputMetadata,
                      void *obj,
                      Capabilities const &capabilities) -> Expected<int>;
    NodeDataHashMap nodeDataMap;
};
/*
inline void to_json(nlohmann::json &json, PathSpace const &space) {
    //j = json{{"name", p.name}, {"address", p.address}, {"age", p.age}};
    //json = nlohmann::json{{"name", p.name}};
}

inline void from_json(nlohmann::json const &j, PathSpace &space) {
    //j.at("name").get_to(p.name);
    //j.at("address").get_to(p.address);
    //j.at("age").get_to(p.age);
}*/

} // namespace SP



        /*else if(pathComponent.isGlob()) { // Send along down the line to all matching the glob expression
            for(auto &nodePair : this->nodes) {
                if(pathComponent == nodePair.first) {
                    auto returnedValue = nodePair.second->insertInternal(nextIter, end, inputData, capabilities, ttl); //bugbug multithread write while readlock
                    if(!returnedValue.has_value())
                        return std::unexpected(returnedValue.error());
                    nbrInserted += returnedValue.value();
                }
            }
        } else { // If any local name matches, send down the line to that one
            auto const concreteName = ConcreteName{pathComponent.getName()};
            auto const updateIfExists = [&](auto &nodePair){
                auto returnedValue = nodePair.second->insertInternal(nextIter, end, inputData, capabilities, ttl);
                if(!returnedValue.has_value())
                    return std::unexpected(returnedValue.error());
                nbrInserted += returnedValue.value();
            };
            auto const emplaceIfNotExists = [&concreteName](const NodeHashMap::constructor& ctor){
                ctor(concreteName, std::make_unique<PathSpace>());
            };
            if(this->nodes.lazy_emplace_l(concreteName, updateIfExists, emplaceIfNotExists)) {
                this->nodes.if_contains(concreteName, [&](auto &nodePair){
                    auto returnedValue = nodePair.second->insertInternal(nextIter, end, inputData, capabilities, ttl);
                    if(!returnedValue.has_value())
                        return std::unexpected(returnedValue.error());
                    nbrInserted += returnedValue.value();
                });
            }
        }*/