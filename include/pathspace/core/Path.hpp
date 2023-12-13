#pragma once
#include <string_view>

namespace SP {

struct Path {
    struct Iterator {
        Iterator(std::string_view::const_iterator iter, std::string_view::const_iterator endIter);

        Iterator& operator++();
        auto operator==(Iterator const &other) const -> bool;
        auto operator*() const -> std::string_view;
        auto operator->() const -> std::string_view;
        
        auto isAtEnd() const -> bool;
    private:
        auto skipSlashes() -> void;
        std::string_view::const_iterator current;
        std::string_view::const_iterator end;
    };

    auto begin() const -> Iterator;
    auto end() const -> Iterator;

    Path() = default;
    Path(std::string_view const &str);
    auto operator<(Path const &other) const -> bool;
    auto operator==(Path const &other) const -> bool;
    auto operator==(char const * const other) const -> bool;

    auto isValidPath() const -> bool;
private:
    std::string_view stringv;
};

}