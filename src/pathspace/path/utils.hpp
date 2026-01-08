#pragma once
#include <optional>
#include <string_view>

namespace SP {

template <typename T>
concept StringConvertible = requires(T t) {
    { std::string(t) };
};

auto match_paths(std::string_view const pathA, std::string_view const pathB) -> bool;
auto match_names(std::string_view const nameA, std::string_view const nameB) -> bool;
auto is_concrete(std::string_view path) -> bool;
auto is_glob(std::string_view path) -> bool;

struct IndexedComponent {
    std::string_view base;
    std::optional<std::size_t> index;
    bool malformed = false;
};

auto parse_indexed_component(std::string_view component) -> IndexedComponent;
auto append_index_suffix(std::string const& base, std::size_t index) -> std::string;

} // namespace SP
