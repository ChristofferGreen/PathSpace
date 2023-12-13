#pragma once
#include "pathspace/core/GlobName.hpp"

#include <string_view>

namespace SP {

struct Path;
struct GlobPath {
    struct Iterator {
        Iterator(std::string_view::const_iterator iter, std::string_view::const_iterator endIter);

        Iterator& operator++();
        auto operator==(Iterator const &other) const -> bool;
        auto operator*() const -> GlobName;
        auto operator->() const -> GlobName;
        
        auto isAtEnd() const -> bool;
    private:
        auto skipSlashes() -> void;
        std::string_view::const_iterator current;
        std::string_view::const_iterator end;
    };

    auto begin() const -> Iterator;
    auto end() const -> Iterator;

    GlobPath() = default;
    GlobPath(char const * const ptr);
    explicit GlobPath(std::string_view const &stringv);
    auto operator<=>(GlobPath const &other) const -> std::strong_ordering;
    auto operator==(GlobPath const &other) const -> bool;
    auto operator==(Path const &other) const -> bool;
    auto operator==(char const * const other) const -> bool;
    auto operator<=>(char const * const other) const -> std::strong_ordering;

    auto isValidPath() const -> bool;
private:
    std::string_view stringv;
};

}