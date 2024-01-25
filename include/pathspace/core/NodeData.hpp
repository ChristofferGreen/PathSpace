#pragma once
#include "pathspace/utils/ByteQueue.hpp"

namespace SP {

struct NodeData {
    template <class Archive>
    void serialize(Archive &ar) {
        ar(this->data);
    }  

    ByteQueue data;
};

}