#pragma once
#include <string>
#include <string_view>

namespace SP2 {

template<typename T>
struct BasePath {
    struct Iterator {
        Iterator(T::const_iterator iter, T::const_iterator endIter);

        Iterator& operator++();
        auto operator==(Iterator const &other) const -> bool;
        auto operator*() const -> T;
        auto operator->() const -> T;
        
        auto isAtEnd() const -> bool;
    private:
        auto skipSlashes() -> void;
        T::const_iterator current;
        T::const_iterator end;
    };

    auto begin() const -> Iterator;
    auto end() const -> Iterator;

    BasePath() = default;
    BasePath(char const * const &str);
    BasePath(T const &t);
    auto operator<(BasePath const &other) const -> bool;
    auto operator==(BasePath const &other) const -> bool;
    auto operator==(char const * const other) const -> bool;

    auto isValidPath() const -> bool;
protected:
    T path;
};
using BasePathString = BasePath<std::string>;
using BasePathStringView = BasePath<std::string_view>;

}