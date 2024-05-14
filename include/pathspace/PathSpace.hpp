#pragma once

#include "core/Error.hpp"
#include "core/Capabilities.hpp"
#include "core/TimeToLive.hpp"
#include "core/ExecutionOptions.hpp"
#include "type/InputData.hpp"
#include "core/NodeData.hpp"
#include "type/Helper.hpp"

#include <chrono>
#include <functional>
#include <iterator>
#include <memory>
#include <queue>
#include <string>
#include <vector>

namespace SP {

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
    auto insertDataName(GlobName const &globName,
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
    auto grabConcretePathComponent(ConcretePathIteratorStringView const &nextIter,
                                   ConcretePathIteratorStringView const &end,
                                   ConcreteName const &concreteName,
                                   InputMetadata const &inputMetadata,
                                   void *obj,
                                   Capabilities const &capabilities) -> Expected<int>;
    NodeDataHashMap nodeDataMap;
};

}