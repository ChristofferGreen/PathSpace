#pragma once
#include "BasePath.hpp"

#include <string_view>

namespace SP2 {

template<typename T>
struct Path : public BasePath<T> {
    Path() = default;
    Path(T const &str);
    auto operator<(Path const &other) const -> bool;
    auto operator==(Path const &other) const -> bool;
    auto operator==(char const * const other) const -> bool;

    auto isValidPath() const -> bool;
private:
};
using PathString = Path<std::string>;
using PathStringView = Path<std::string_view>;


}