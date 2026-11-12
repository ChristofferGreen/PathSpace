#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace SP {

template <typename T>
concept StringConvertible = requires(T t) {
    { std::string(t) };
};

auto match_paths(std::string_view const pathA, std::string_view const pathB) -> bool;
auto match_names(std::string_view const nameA, std::string_view const nameB) -> bool;

// Matches one path component against a glob pattern ('*', '?', '[...]', '\\' escapes).
// `supermatch` is set when a '**' is reached with input left over: it matches across the
// component edge. Both match_names and GlobName::match are built on this.
struct GlobComponentMatch {
    bool match      = false;
    bool supermatch = false;
};
auto matchGlobComponent(std::string_view pattern, std::string_view name) -> GlobComponentMatch;
auto is_concrete(std::string_view path) -> bool;
auto is_glob(std::string_view path) -> bool;

struct IndexedComponent {
    std::string_view base;
    std::optional<std::size_t> index;
    bool malformed = false;
};

// Component-aware prefix test on canonical absolute paths: "/a" is a prefix of "/a" and "/a/b",
// but not of "/ab". An empty prefix or "/" is a prefix of every path; a trailing '/' on the
// prefix is ignored.
auto isPathPrefix(std::string_view prefix, std::string_view path) -> bool;
// Removes `prefix` from `path` on a component boundary and returns the absolute remainder
// ("/" when they are equal), or std::nullopt when `path` is not under `prefix`.
auto stripPathPrefix(std::string_view path, std::string_view prefix) -> std::optional<std::string>;

auto parse_indexed_component(std::string_view component) -> IndexedComponent;
auto append_index_suffix(std::string_view base, std::size_t index) -> std::string;

} // namespace SP
