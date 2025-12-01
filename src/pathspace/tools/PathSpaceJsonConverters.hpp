#pragma once

#include "core/Error.hpp"
#include "type/InputMetadata.hpp"
#include "type/InputMetadataT.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <typeinfo>
#include <utility>

#include <nlohmann/json.hpp>

namespace SP {

namespace detail {

class PathSpaceJsonValueReader {
public:
    virtual ~PathSpaceJsonValueReader() = default;

    template <typename T>
    auto pop(T& value) -> std::optional<Error> {
        InputMetadataT<T> metadata{};
        return this->popImpl(&value, metadata);
    }

private:
    virtual auto popImpl(void* destination, InputMetadata const& metadata) -> std::optional<Error> = 0;
};

using PathSpaceJsonConverterFn = std::function<std::optional<nlohmann::json>(PathSpaceJsonValueReader&)>;

void RegisterPathSpaceJsonConverter(std::type_index type, std::string_view typeName, PathSpaceJsonConverterFn fn);

auto ConvertWithRegisteredConverter(std::type_index type, PathSpaceJsonValueReader& reader)
    -> std::optional<nlohmann::json>;

auto DescribeRegisteredType(std::type_index type) -> std::string;

} // namespace detail

template <typename T, typename Converter>
auto PathSpaceJsonRegisterConverterAs(std::string_view typeName, Converter&& converter) -> void {
    detail::RegisterPathSpaceJsonConverter(
        std::type_index(typeid(T)),
        typeName,
        [fn = std::forward<Converter>(converter)](detail::PathSpaceJsonValueReader& reader)
            -> std::optional<nlohmann::json> {
            T value{};
            if (auto err = reader.pop(value)) {
                return std::nullopt;
            }
            return fn(value);
        });
}

template <typename T, typename Converter>
auto PathSpaceJsonRegisterConverter(Converter&& converter) -> void {
    PathSpaceJsonRegisterConverterAs<T>(typeid(T).name(), std::forward<Converter>(converter));
}

#define PATHSPACE_INTERNAL_JSON_CONCAT(a, b) PATHSPACE_INTERNAL_JSON_CONCAT_IMPL(a, b)
#define PATHSPACE_INTERNAL_JSON_CONCAT_IMPL(a, b) a##b

#define PATHSPACE_REGISTER_JSON_CONVERTER(Type, Lambda)                                                    \
    namespace {                                                                                            \
    struct PATHSPACE_INTERNAL_JSON_CONCAT(PathSpaceJsonConverterAutoRegister_, __LINE__) {                  \
        PATHSPACE_INTERNAL_JSON_CONCAT(PathSpaceJsonConverterAutoRegister_, __LINE__)() {                   \
            ::SP::PathSpaceJsonRegisterConverter<Type>(Lambda);                                            \
        }                                                                                                  \
    } PATHSPACE_INTERNAL_JSON_CONCAT(PathSpaceJsonConverterAutoRegisterInstance_, __LINE__);               \
    }

} // namespace SP
