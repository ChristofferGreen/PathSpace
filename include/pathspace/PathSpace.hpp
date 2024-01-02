#pragma once

#include "core/Error.hpp"
#include "core/Capabilities.hpp"
#include "core/TimeToLive.hpp"
#include "core/ExecutionOptions.hpp"
#include "core/NodeData.hpp"
#include "serialization/InputData.hpp"
#include "serialization/Helper.hpp"

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
    auto read(GlobPathStringView const &path,
              Capabilities const &capabilities = Capabilities::All()) const -> Expected<T>;

    template<typename T>
    auto grab(GlobPathStringView const &path,
              Capabilities const &capabilities = Capabilities::All()) -> Expected<T>;

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
    auto insertInternal(const GlobPathIteratorStringView &iter,
                        const GlobPathIteratorStringView &end,
                        const InputData &inputData,
                        const Capabilities &capabilities,
                        const TimeToLive &ttl) -> Expected<int>;
    auto insertDataName(const ConcreteName &name,
                        const InputData &inputData,
                        const Capabilities &capabilities,
                        const TimeToLive &ttl) -> Expected<int>;
    auto insertConcretePathComponent(const GlobPathIteratorStringView &iter,
                             const GlobPathIteratorStringView &nextIter,
                             const GlobPathIteratorStringView &end,
                             const ConcreteName &name,
                             const InputData &inputData,
                             const Capabilities &capabilities,
                             const TimeToLive &ttl) -> Expected<int>;
    auto insertGlobPathComponent(const GlobPathIteratorStringView &iter,
                             const GlobPathIteratorStringView &nextIter,
                             const GlobPathIteratorStringView &end,
                             const GlobName &name,
                             const InputData &inputData,
                             const Capabilities &capabilities,
                             const TimeToLive &ttl) -> Expected<int>;

    NodeDataHashMap nodeDataMap;
};

}



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