#pragma once

#include "core/Error.hpp"
#include "core/In.hpp"
#include "core/InsertReturn.hpp"
#include "core/Out.hpp"
#include "pathspace/PathSpace.hpp"
#include "type/InputMetadata.hpp"
#include "type/InputMetadataT.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SP {

class PathSpace;

struct TypeOperations {
    using InsertFn = Expected<InsertReturn>(*)(PathSpace&, std::string_view, void*, In const&);
    using TakeFn   = Expected<void>(*)(PathSpace&, std::string_view, Out const&, void*);

    std::size_t size      = 0;
    std::size_t alignment = alignof(std::max_align_t);
    void (*construct)(void*) = nullptr;
    void (*destroy)(void*)   = nullptr;
    InsertFn insert          = nullptr;
    TakeFn   take            = nullptr;
};

struct TypeMetadataView {
    std::string_view type_name;
    InputMetadata const& metadata;
    TypeOperations const& operations;
};

class TypeMetadataRegistry {
public:
    static TypeMetadataRegistry& instance();

    template <typename T>
    [[nodiscard]] bool registerType(std::string_view type_name_override = {});

    [[nodiscard]] std::optional<TypeMetadataView> findByName(std::string_view type_name) const;
    [[nodiscard]] std::optional<TypeMetadataView> findByType(std::type_index type) const;

private:
    struct Entry {
        std::string  type_name;
        InputMetadata metadata;
        TypeOperations operations;
    };

    static auto make_view(Entry const* entry) -> std::optional<TypeMetadataView>;

    struct TypeIndexHash {
        auto operator()(std::type_index index) const noexcept -> std::size_t {
            return index.hash_code();
        }
    };

    struct TypeIndexEqual {
        auto operator()(std::type_index lhs, std::type_index rhs) const noexcept -> bool {
            return lhs == rhs;
        }
    };

    struct TypeNameHash {
        using is_transparent = void;
        auto operator()(std::string_view value) const noexcept -> std::size_t {
            return std::hash<std::string_view>{}(value);
        }
    };

    struct TypeNameEqual {
        using is_transparent = void;
        auto operator()(std::string_view lhs, std::string_view rhs) const noexcept -> bool {
            return lhs == rhs;
        }
    };

    template <typename T>
    [[nodiscard]] static TypeOperations makeOperations();

    template <typename T>
    static void constructValue(void* ptr);

    template <typename T>
    static void destroyValue(void* ptr);

    template <typename T>
    [[nodiscard]] static Expected<InsertReturn> insertValue(PathSpace& space,
                                                           std::string_view path,
                                                           void*            obj,
                                                           In const&        options);

    template <typename T>
    [[nodiscard]] static Expected<void> takeValue(PathSpace& space,
                                                  std::string_view path,
                                                  Out const&      options,
                                                  void*           obj);

    [[nodiscard]] bool registerEntry(std::type_index type_index,
                                     std::string type_name,
                                     InputMetadata metadata,
                                     TypeOperations operations);

    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Entry>> entries_;
    std::unordered_map<std::string, Entry*, TypeNameHash, TypeNameEqual>      by_name_;
    std::unordered_map<std::type_index, Entry*, TypeIndexHash, TypeIndexEqual> by_type_;
};

template <typename T>
bool TypeMetadataRegistry::registerType(std::string_view type_name_override) {
    InputMetadata metadata{InputMetadataT<T>{}};
    if (metadata.typeInfo == nullptr) {
        return false;
    }
    std::string resolved_name = type_name_override.empty() ? std::string(metadata.typeInfo->name())
                                                           : std::string(type_name_override);
    if (resolved_name.empty()) {
        return false;
    }
    auto operations = makeOperations<T>();
    return registerEntry(std::type_index(typeid(T)), std::move(resolved_name), metadata, operations);
}

template <typename T>
TypeOperations TypeMetadataRegistry::makeOperations() {
    TypeOperations ops;
    ops.size       = sizeof(T);
    ops.alignment  = alignof(T);
    if constexpr (std::is_default_constructible_v<T>) {
        ops.construct = &constructValue<T>;
    }
    if constexpr (std::is_destructible_v<T>) {
        ops.destroy = &destroyValue<T>;
    }
    ops.insert = &insertValue<T>;
    ops.take   = &takeValue<T>;
    return ops;
}

template <typename T>
void TypeMetadataRegistry::constructValue(void* ptr) {
    std::construct_at(static_cast<T*>(ptr));
}

template <typename T>
void TypeMetadataRegistry::destroyValue(void* ptr) {
    std::destroy_at(static_cast<T*>(ptr));
}

template <typename T>
Expected<InsertReturn> TypeMetadataRegistry::insertValue(PathSpace& space,
                                                         std::string_view path,
                                                         void*            obj,
                                                         In const&        options) {
    auto& value = *static_cast<T*>(obj);
    return space.insert(path, value, options);
}

template <typename T>
Expected<void> TypeMetadataRegistry::takeValue(PathSpace& space,
                                               std::string_view path,
                                               Out const&      options,
                                               void*           obj) {
    auto taken = space.take<T>(path, options);
    if (!taken) {
        return std::unexpected(taken.error());
    }
    *static_cast<T*>(obj) = std::move(*taken);
    return {};
}

} // namespace SP

#define PATHSPACE_INTERNAL_TYPE_METADATA_CONCAT(a, b) a##b
#define PATHSPACE_TYPE_METADATA_CONCAT(a, b) PATHSPACE_INTERNAL_TYPE_METADATA_CONCAT(a, b)

#define PATHSPACE_REGISTER_TYPE_METADATA(Type)                                                   \
    namespace {                                                                                  \
    struct PATHSPACE_TYPE_METADATA_CONCAT(TypeMetadataAutoRegister_, __LINE__) {                  \
        PATHSPACE_TYPE_METADATA_CONCAT(TypeMetadataAutoRegister_, __LINE__)() {                   \
            ::SP::TypeMetadataRegistry::instance().registerType<Type>();                          \
        }                                                                                        \
    } PATHSPACE_TYPE_METADATA_CONCAT(TypeMetadataAutoRegisterInstance_, __LINE__);               \
    }
