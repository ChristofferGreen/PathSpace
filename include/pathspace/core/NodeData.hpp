#pragma once
#include <queue>

namespace SP {

struct NodeData {
    template <class Archive>
    void serialize(Archive &ar) {
        ar(this->data);
    }  

    std::queue<std::byte> data;
};

}