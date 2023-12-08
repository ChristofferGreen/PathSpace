#pragma once
#include "pathspace/core/GlobName.hpp"

#include <string_view>

namespace SP {

struct GlobPath {
    class Iterator {
    public:
        Iterator(std::string_view::const_iterator iter, std::string_view::const_iterator endIter);

        Iterator& operator++();
        auto operator==(Iterator const &other) const -> bool;
        auto operator*() const -> GlobName const;
        
        auto isAtEnd() const -> bool;

    private:
        std::string_view::const_iterator current;
        std::string_view::const_iterator end;
    };

    auto begin() const -> Iterator;
    auto end() const -> Iterator;

    GlobPath(std::string_view const &stringv);
    auto operator==(GlobPath const &other) const -> bool;
    auto operator==(char const * const other) const -> bool;

    auto validPath() const -> bool;
private:
    std::string_view stringv;
};

}