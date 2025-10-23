#pragma once

#include <cstddef>
#include <cstdint>

namespace SP::UI {

enum class LocalMouseButton : int {
    Left = 1,
    Right = 2,
    Middle = 3,
    Button4 = 4,
    Button5 = 5,
};

enum class LocalMouseEventType {
    Move,
    AbsoluteMove,
    ButtonDown,
    ButtonUp,
    Wheel,
};

struct LocalMouseEvent {
    LocalMouseEventType type = LocalMouseEventType::Move;
    LocalMouseButton button = LocalMouseButton::Left;
    int dx = 0;
    int dy = 0;
    int x = -1;
    int y = -1;
    int wheel = 0;
};

enum class LocalKeyEventType {
    KeyDown,
    KeyUp,
};

enum LocalKeyModifier : unsigned int {
    LocalKeyModifierShift = 1u << 0,
    LocalKeyModifierControl = 1u << 1,
    LocalKeyModifierAlt = 1u << 2,
    LocalKeyModifierCommand = 1u << 3,
    LocalKeyModifierFunction = 1u << 4,
    LocalKeyModifierCapsLock = 1u << 5,
};

struct LocalKeyEvent {
    LocalKeyEventType type = LocalKeyEventType::KeyDown;
    unsigned int keycode = 0;
    unsigned int modifiers = 0;
    char32_t character = U'\0';
    bool repeat = false;
};

struct LocalWindowCallbacks {
    void (*mouse_event)(LocalMouseEvent const&, void* user_data) = nullptr;
    void (*clear_mouse)(void* user_data) = nullptr;
    void* user_data = nullptr;
    void (*key_event)(LocalKeyEvent const&, void* user_data) = nullptr;
};

void SetLocalWindowCallbacks(LocalWindowCallbacks const& callbacks);

void RequestLocalWindowQuit();
auto LocalWindowQuitRequested() -> bool;
void ClearLocalWindowQuitRequest();

void ConfigureLocalWindow(int width, int height, char const* title);
void InitLocalWindow();
void InitLocalWindowWithSize(int width, int height, char const* title);
void PollLocalWindow();

void PresentLocalWindowFramebuffer(std::uint8_t const* data,
                                   int width,
                                   int height,
                                   int row_stride_bytes);

void PresentLocalWindowIOSurface(void* surface,
                                 int width,
                                 int height,
                                 int row_stride_bytes);

void GetLocalWindowContentSize(int* width, int* height);

} // namespace SP::UI
