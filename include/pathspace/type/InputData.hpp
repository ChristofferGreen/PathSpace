#pragma once
#include "InputMetadata.hpp"
#include "InputDataSerialization.hpp"



#include <utility>

namespace SP {

struct InputData {
    template<typename T>
    InputData(T&& data) : metadata(std::forward<T>(data)), serialization(this->metadata, std::forward<T>(data)) {}
    
    InputMetadata metadata;
    InputDataSerialization serialization;
};

} // namespace SP