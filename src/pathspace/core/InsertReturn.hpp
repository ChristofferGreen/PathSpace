#pragma once
#include "Error.hpp"

#include <cstdint>
#include <vector>

namespace SP {

struct InsertReturn {
    uint32_t nbrValuesInserted = 0;
    uint32_t nbrSpacesInserted = 0;
    uint32_t nbrTasksCreated = 0;
    std::vector<Error> errors;
};

} // namespace SP