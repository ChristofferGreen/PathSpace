#pragma once
#include <any>
#include <vector>

#include "pathspace/utils/ByteQueue.hpp"

namespace SP {

struct NodeData {
    template <class Archive>
    void serialize(Archive &ar) {
        ar(this->data);
    }  

    std::vector<uint8_t> data;
    std::vector<std::any> datav;
};

}