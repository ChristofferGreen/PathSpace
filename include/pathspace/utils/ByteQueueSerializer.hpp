#pragma once
#include "pathspace/utils/ByteQueue.hpp"

#include <cereal/archives/binary.hpp>
#include <sstream>

namespace SP {

template<typename T>
auto serialize_to_bytequeue(ByteQueue& bq, const T& obj) -> void {
    std::stringstream objSS;
    {
        cereal::BinaryOutputArchive oarchive(objSS);
        oarchive(obj);
    }
    std::string const objStr = objSS.str();
    uint64_t const objStrSize = static_cast<uint64_t>(objStr.size());

    std::stringstream sizeSS;
    {
        cereal::BinaryOutputArchive sizeArchive(sizeSS);
        sizeArchive(objStrSize);
    }

    for (char const ch : sizeSS.str())
        bq.push_back(std::byte(ch));

    for (char const ch : objStr)
        bq.push_back(std::byte(ch));
}

template<typename T>
auto deserialize_from_bytequeue(ByteQueue &bq, T &obj) -> void {
    std::string sizeStr;
    sizeStr.resize(sizeof(uint64_t));
    for (size_t i = 0; i < sizeof(uint64_t); ++i) {
        sizeStr[i] = static_cast<char>(bq.front());
        bq.pop_front();
    }

    std::stringstream sizeStream(sizeStr);
    uint64_t size;
    {
        cereal::BinaryInputArchive sizeArchive(sizeStream);
        sizeArchive(size);
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

template<typename T>
auto deserialize_from_const_bytequeue(ByteQueue const &bq, T &obj) -> void {
    std::string sizeStr;
    sizeStr.resize(sizeof(uint64_t));
    for (size_t i = 0; i < sizeof(uint64_t); ++i)
        sizeStr[i] = static_cast<char>(bq[i]);

    std::stringstream sizeStream(sizeStr);
    uint64_t size;
    {
        cereal::BinaryInputArchive sizeArchive(sizeStream);
        sizeArchive(size);
    }

    std::string str;
    str.reserve(size);
    for (size_t i = 0; i < size; ++i)
        str.push_back(static_cast<char>(bq[sizeof(uint64_t)+i]));

    std::stringstream ss(str);
    {
        cereal::BinaryInputArchive iarchive(ss);
        iarchive(obj);
    }
}

} // namespace SP