#pragma once

#include "pathspace/core/GlobName.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace SP {

struct GlobPath {
    class Iterator {
    public:
        Iterator(std::string_view::const_iterator iter, std::string_view::const_iterator endIter);

        Iterator& operator++();
        auto operator==(const Iterator& other) const -> bool;
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
};

}