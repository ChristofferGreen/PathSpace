#pragma once
#include "Error.hpp"

#include <variant>

namespace SP {

template<typename T>
struct InsertReturn {
    int nbrInserted = 0;
    int nbrErrors = 0;
    std::variant<T, std::vector<T>> values; /* concrete(T), glob(std::vector<T>)*/
    std::variant<Error, std::vector<Error>> errors; /* concrete(Error), glob(std::vector<Error>)*/
    auto value(int position = 0) const -> T;
    auto error(int position = 0) const -> Error;
};

}