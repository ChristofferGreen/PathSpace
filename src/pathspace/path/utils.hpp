#pragma once
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

} // namespace SP