#pragma once
#include "type/InputMetadata.hpp"
#include "task/Future.hpp"
#include "task/Executor.hpp"
#include "task/IFutureAny.hpp"

#include <memory>

namespace SP {
struct Task;

struct InputData {
    template <typename T>
    InputData(T&& in)
        : metadata(InputMetadataT<T>{}) {
        if constexpr (std::is_function_v<std::remove_pointer_t<std::decay_t<T>>> || std::is_member_function_pointer_v<std::decay_t<T>>)
            this->obj = reinterpret_cast<void*>(+in);
        else
            this->obj = const_cast<void*>(static_cast<const void*>(&in));
    }

    void*                 obj = nullptr;
    std::shared_ptr<Task> task;
    Future                future;    // Optional: future handle for legacy task results
    FutureAny             anyFuture; // Optional: type-erased future for typed task results
    Executor*             executor = nullptr; // Optional: injected executor for task scheduling
    InputMetadata         metadata;
};

} // namespace SP