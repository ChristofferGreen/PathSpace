#pragma once
#include "pathspace/type/InputMetadata.hpp"

#include <utility>

namespace SP {

struct InputData {
    template<typename T>
    InputData(T&& obj) : obj(const_cast<void*>(static_cast<const void*>(&obj))), metadata(InputMetadataT<T>{}) {}

    void *obj = nullptr;
    InputMetadata metadata;
};

} // namespace SP