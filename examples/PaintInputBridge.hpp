#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

namespace PaintInput {

enum class MouseButton : int {
    Left = 1,
    Right = 2,
    Middle = 3,
    Button4 = 4,
    Button5 = 5,
};

enum class MouseEventType {
    Move,
    AbsoluteMove,
    ButtonDown,
    ButtonUp,
    Wheel,
};

struct MouseEvent {
    MouseEventType type = MouseEventType::Move;
    MouseButton button = MouseButton::Left;
    int dx = 0;
    int dy = 0;
    int x = -1;
    int y = -1;
    int wheel = 0;
};

inline std::mutex gMouseMutex;
inline std::deque<MouseEvent> gMouseQueue;

inline void enqueue_mouse(MouseEvent const& ev) {
    std::lock_guard<std::mutex> lock(gMouseMutex);
    gMouseQueue.push_back(ev);
}

inline std::optional<MouseEvent> try_pop_mouse() {
    std::lock_guard<std::mutex> lock(gMouseMutex);
    if (gMouseQueue.empty()) {
        return std::nullopt;
    }
    MouseEvent ev = gMouseQueue.front();
    gMouseQueue.pop_front();
    return ev;
}

inline void clear_mouse() {
    std::lock_guard<std::mutex> lock(gMouseMutex);
    gMouseQueue.clear();
}

} // namespace PaintInput

