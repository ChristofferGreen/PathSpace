#pragma once
#include <string>
#include <string_view>

namespace SP {

struct ConcreteName {
    ConcreteName(char const * const ptr);
    ConcreteName(std::string::const_iterator const &iter, std::string::const_iterator const &endIter);
    ConcreteName(std::string_view::const_iterator const &iter, std::string_view::const_iterator const &endIter);

    auto operator<=>(ConcreteName const &other) const -> std::strong_ordering;
    auto operator==(char const * const other) const -> bool;

private:
    std::string_view name;
};

}