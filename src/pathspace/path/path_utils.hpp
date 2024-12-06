#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace SP {

struct PathSV {
    PathSV(std::string_view path)
        : path(path) {}

    auto pathStringView() const -> std::string_view {
        return path;
    }

    auto pathString() const -> std::string {
        return std::string(path);
    }

private:
    std::string_view path;
};

auto match_paths(std::string_view const pathA, std::string_view const pathB) -> bool;
auto match_names(std::string_view const nameA, std::string_view const nameB) -> bool;
auto is_concrete(std::string_view path) -> bool;
auto is_glob(std::string_view path) -> bool;

} // namespace SP