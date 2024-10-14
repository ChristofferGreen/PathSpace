#pragma once
#include "pathspace/type/InputMetadata.hpp"

#include <functional>

namespace SP {
struct Task;

struct InputData {
    template <typename T>
    InputData(T&& in) : metadata(InputMetadataT<T>{}) {
        if constexpr (std::is_function_v<std::remove_pointer_t<std::decay_t<T>>> || std::is_member_function_pointer_v<std::decay_t<T>>) {
            this->obj = reinterpret_cast<void*>(+in);
        } else {
            this->obj = const_cast<void*>(static_cast<const void*>(&in));
        }
    }

    void* obj = nullptr;
    std::function<void(Task const& task)> taskExecutor;
    InputMetadata metadata;
};

} // namespace SP