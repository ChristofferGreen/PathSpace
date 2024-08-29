#pragma once

#include "core/Capabilities.hpp"
#include "core/Error.hpp"
#include "core/InsertOptions.hpp"
#include "core/InsertReturn.hpp"
#include "core/ReadOptions.hpp"
#include "taskpool/TaskPool.hpp"
#include "type/Helper.hpp"
#include "type/InputData.hpp"

#include <functional>
#include <string>

namespace SP {

struct PathSpace {
    PathSpace(TaskPool* pool_ = nullptr) : pool(pool_ == nullptr ? &TaskPool::Instance() : pool_) {
    }

    template <typename T>
    auto insert(GlobPathStringView const& path, T const& data, InsertOptions const& options = {}) -> InsertReturn {
        // Check capabilities
        // Implement TTL functionality
        InsertReturn ret;
        if (!path.isValid())
            ret.errors.emplace_back(Error::Code::InvalidPath, std::string("The path was not valid: ").append(path.getPath()));
        else
            this->insertInternal(path.begin(), path.end(), InputData{data}, options, ret);
        return ret;
    }

    template <typename T>
    auto read(ConcretePathStringView const& path, ReadOptions const& options = {}) const -> Expected<T> {
        Expected<T> obj(T{});
        if (auto ret = this->readInternal(path.begin(), path.end(), InputMetadataT<T>{}, &obj.value(), options); !ret)
            return std::unexpected(ret.error());
        return obj;
    }

    template <typename T>
    auto grab(ConcretePathStringView const& path, Capabilities const& capabilities = Capabilities::All()) -> Expected<T> {
        Expected<T> obj(T{});
        if (auto ret = this->grabInternal(path.begin(), path.end(), InputMetadataT<T>{}, &obj.value(), capabilities); !ret)
            return std::unexpected{ret.error()};
        return obj;
    }

    auto
    subscribe(GlobPathStringView const& path, std::function<void(const GlobPathStringView&)> callback, Capabilities const& capabilities = Capabilities::All()) -> Expected<void>;

    template <typename T>
    auto visit(GlobPathStringView const& path, std::function<void(T&)> visitor, Capabilities const& capabilities = Capabilities::All()) -> Expected<void>;

    auto toJSON(bool const isHumanReadable) const -> std::string;

    template <class Archive>
    void serialize(Archive& ar) {
        ar(this->nodeDataMap);
    }

private:
    auto insertInternal(GlobPathIteratorStringView const& iter, GlobPathIteratorStringView const& end, InputData const& inputData, InsertOptions const& options, InsertReturn& ret)
            -> void;
    auto insertConcreteDataName(ConcreteName const& name, InputData const& inputData, InsertOptions const& options, InsertReturn& ret) -> void;
    auto insertGlobDataName(GlobName const& globName, InputData const& inputData, InsertOptions const& options, InsertReturn& ret) -> void;
    auto insertConcretePathComponent(GlobPathIteratorStringView const& iter,
                                     GlobPathIteratorStringView const& end,
                                     ConcreteName const& name,
                                     InputData const& inputData,
                                     InsertOptions const& options,
                                     InsertReturn& ret) -> void;
    auto insertGlobPathComponent(GlobPathIteratorStringView const& iter,
                                 GlobPathIteratorStringView const& end,
                                 GlobName const& name,
                                 InputData const& inputData,
                                 InsertOptions const& options,
                                 InsertReturn& ret) -> void;

    auto readInternal(ConcretePathIteratorStringView const& iter,
                      ConcretePathIteratorStringView const& end,
                      InputMetadata const& inputMetadata,
                      void* obj,
                      ReadOptions const& options) const -> Expected<int>;
    auto readDataName(ConcreteName const& concreteName,
                      ConcretePathIteratorStringView const& nextIter,
                      ConcretePathIteratorStringView const& end,
                      InputMetadata const& inputMetadata,
                      void* obj,
                      ReadOptions const& options) const -> Expected<int>;
    auto readConcretePathComponent(ConcretePathIteratorStringView const& nextIter,
                                   ConcretePathIteratorStringView const& end,
                                   ConcreteName const& concreteName,
                                   InputMetadata const& inputMetadata,
                                   void* obj,
                                   ReadOptions const& options) const -> Expected<int>;
    auto grabInternal(ConcretePathIteratorStringView const& iter,
                      ConcretePathIteratorStringView const& end,
                      InputMetadata const& inputMetadata,
                      void* obj,
                      Capabilities const& capabilities) -> Expected<int>;
    auto grabDataName(ConcreteName const& concreteName,
                      ConcretePathIteratorStringView const& nextIter,
                      ConcretePathIteratorStringView const& end,
                      InputMetadata const& inputMetadata,
                      void* obj,
                      Capabilities const& capabilities) -> Expected<int>;
    auto grabConcretePathComponent(ConcretePathIteratorStringView const& nextIter,
                                   ConcretePathIteratorStringView const& end,
                                   ConcreteName const& concreteName,
                                   InputMetadata const& inputMetadata,
                                   void* obj,
                                   Capabilities const& capabilities) -> Expected<int>;
    NodeDataHashMap nodeDataMap;
    TaskPool* pool = nullptr;
};

} // namespace SP