#pragma once

#include "pathspace/core/GlobName.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace SP {

struct GlobPath {
    class Iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = GlobName;
        using difference_type = std::ptrdiff_t;
        using pointer = const value_type*;
        using reference = const value_type&;

        Iterator(std::string_view::const_iterator iter, std::string_view::const_iterator endIter);

        Iterator& operator++();
        auto operator==(const Iterator& other) const -> bool;
        auto operator!=(const Iterator& other) const -> bool;
        auto operator*() const -> GlobName const;

    private:
        std::string_view::const_iterator current;
        std::string_view::const_iterator end;
    };

    auto begin() const -> Iterator;
    auto end() const -> Iterator;

    GlobPath(std::string_view const &stringv);

    auto validPath() const -> bool;
    auto toString() const -> std::string;
private:
    std::string_view stringv;
    std::string_view::const_iterator currentNameIterator;
};

}