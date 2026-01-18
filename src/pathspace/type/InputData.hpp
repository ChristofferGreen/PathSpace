#pragma once
#include "type/InputMetadata.hpp"
#include "task/Future.hpp"
#include "task/Executor.hpp"
#include "task/IFutureAny.hpp"
#include "core/PodPayload.hpp"

#include <memory>
#include <type_traits>

namespace SP {
struct Task;

struct InputData {
    InputData(void const* objIn, InputMetadata const& md)
        : obj(const_cast<void*>(objIn))
        , metadata(md) {}

    template <typename T>
    InputData(T&& in)
        : metadata(InputMetadataT<T>{}) {
        if constexpr (std::is_function_v<std::remove_pointer_t<std::decay_t<T>>> || std::is_member_function_pointer_v<std::decay_t<T>>)
            this->obj = reinterpret_cast<void*>(+in);
        else
            this->obj = const_cast<void*>(static_cast<const void*>(&in));
        if constexpr (InputMetadataT<T>::podPreferred) {
            if (metadata.createPodPayload == nullptr) {
                metadata.createPodPayload = &PodPayload<std::remove_cvref_t<T>>::CreateShared;
            }
        }
    }

    void*                 obj = nullptr;
    std::shared_ptr<Task> task;
    Future                future;    // Optional: future handle for legacy task results
    FutureAny             anyFuture; // Optional: type-erased future for typed task results
    Executor*             executor = nullptr; // Optional: injected executor for task scheduling
    bool                  replaceExistingPayload = false; // Optional: clear existing payload before insert
    InputMetadata         metadata;
};

} // namespace SP
