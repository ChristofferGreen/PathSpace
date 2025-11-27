#pragma once

#include <atomic>
#include <cstdint>

namespace SP::UI::DetailShared {

inline auto widget_op_sequence() -> std::atomic<std::uint64_t>& {
    static std::atomic<std::uint64_t> sequence{0};
    return sequence;
}

inline auto scene_dirty_sequence() -> std::atomic<std::uint64_t>& {
    static std::atomic<std::uint64_t> sequence{0};
    return sequence;
}

} // namespace SP::UI::DetailShared
