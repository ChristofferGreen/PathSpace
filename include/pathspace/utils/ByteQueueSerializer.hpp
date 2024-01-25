#pragma once
#include "pathspace/utils/ByteQueue.hpp"

#include <cereal/archives/binary.hpp>
#include <sstream>

namespace SP {

template<typename T>
auto serialize_to_bytequeue(ByteQueue& bq, const T& obj) -> void {
    std::stringstream ss;
    {
        cereal::BinaryOutputArchive oarchive(ss);
        oarchive(obj);
    }
    auto str = ss.str();
    size_t size = str.size();
    // Serialize the size first
    for (size_t i = 0; i < sizeof(size); ++i) {
        bq.push_back(std::byte(reinterpret_cast<char*>(&size)[i]));
    }
    // Serialize the object
    for (char ch : str) {
        bq.push_back(std::byte(ch));
    }
}

template<typename T>
auto deserialize_from_bytequeue(ByteQueue &bq, T &obj) -> void{
    // Read the size first
    size_t size = 0;
    for (size_t i = 0; i < sizeof(size); ++i) {
        auto byte = bq.front();
        bq.pop_front();
        reinterpret_cast<char*>(&size)[i] = static_cast<char>(byte);
    }

    std::string str;
    str.reserve(size);
    for (size_t i = 0; i < size; ++i) {
        str.push_back(static_cast<char>(bq.front()));
        bq.pop_front();
    }

    std::stringstream ss(str);
    {
        cereal::BinaryInputArchive iarchive(ss);
        iarchive(obj);
    }
}

} // namespace SP