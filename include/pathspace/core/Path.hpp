#pragma once
#include <string>
#include <string_view>

namespace SP {

struct Path {
    struct Iterator {
        Iterator(std::string::const_iterator iter, std::string::const_iterator endIter);

        Iterator& operator++();
        auto operator==(Iterator const &other) const -> bool;
        auto operator*() const -> std::string_view;
        auto operator->() const -> std::string_view;
        
        auto isAtEnd() const -> bool;
    private:
        auto skipSlashes() -> void;
        std::string::const_iterator current;
        std::string::const_iterator end;
    };

    auto begin() const -> Iterator;
    auto end() const -> Iterator;

    Path() = default;
    Path(char const * const ptr);
    Path(std::string_view const &stringv);
    auto operator<(Path const &other) const -> bool;
    auto operator==(Path const &other) const -> bool;
    auto operator==(char const * const other) const -> bool;

    auto validPath() const -> bool;
private:
    std::string string;
};

}