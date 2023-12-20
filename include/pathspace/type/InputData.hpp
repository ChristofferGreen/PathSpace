#pragma once
#include "InputMetadata.hpp"
#include "InputDataSerialization.hpp"

namespace SP {

struct InputData {
    template<typename T>
    InputData(T&& data) : metadata(std::forward(data)), serialization(data) {}
    
    InputMetadata metadata;
    InputDataSerialization serialization;
};

} // namespace SP