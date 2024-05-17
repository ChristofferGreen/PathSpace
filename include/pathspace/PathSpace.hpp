#pragma once

#include "core/Error.hpp"
#include "core/Capabilities.hpp"
#include "core/TimeToLive.hpp"
#include "core/InsertOptions.hpp"
#include "core/InsertReturn.hpp"
#include "type/InputData.hpp"
#include "type/Helper.hpp"

#include <functional>
#include <string>

namespace SP {

struct PathSpace {
    template<typename T>
    auto insert(GlobPathStringView const &path,
                T const &data,
                InsertOptions const &options = {}) -> InsertReturn {
        // Check if path is valid.
        // Check if path actually has a final name subcomponent
        // Check capabilities
        // Implement TTL functionality
        InsertReturn ret;
        this->insertInternal(path.begin(), path.end(), InputData{data}, options, ret);
        return ret;
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
                        InsertOptions const &options,
                        InsertReturn &ret) -> void;
    auto insertConcreteDataName(ConcreteName const &name,
                                InputData const &inputData,
                                InsertOptions const &options,
                                InsertReturn &ret) -> void;
    auto insertGlobDataName(GlobName const &globName,
                                InputData const &inputData,
                                InsertOptions const &options,
                                InsertReturn &ret) -> void;
    auto insertConcretePathComponent(GlobPathIteratorStringView const &iter,
                                     GlobPathIteratorStringView const &end,
                                     ConcreteName const &name,
                                     InputData const &inputData,
                                     InsertOptions const &options,
                                     InsertReturn &ret) -> void;
    auto insertGlobPathComponent(GlobPathIteratorStringView const &iter,
                                 GlobPathIteratorStringView const &end,
                                 GlobName const &name,
                                 InputData const &inputData,
                                 InsertOptions const &options,
                                 InsertReturn &ret) -> void;

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