#pragma once
#include "Error.hpp"

#include <string>
#include <vector>

namespace SP {

struct PathSpaceBase;

struct InsertReturn {
    uint32_t           nbrValuesInserted = 0;
    uint32_t           nbrSpacesInserted = 0;
    uint32_t           nbrTasksInserted  = 0;
    uint32_t           nbrValuesSuppressed = 0;
    struct RetargetRequest {
        PathSpaceBase* space = nullptr;      // Non-owning pointer to the mounted space
        std::string    mountPrefix;          // Absolute path (within parent) for the nested space
    };
    std::vector<RetargetRequest> retargets;  // Nested spaces that need context/prefix adoption
    std::vector<Error> errors;
};

} // namespace SP
