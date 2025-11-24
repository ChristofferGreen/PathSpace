#include "PaintExampleNewUI.hpp"

#include "../../../../examples/declarative_example_shared.hpp"

#include <pathspace/layer/io/PathIOMouse.hpp>
#include <pathspace/layer/io/PathIOKeyboard.hpp>
#include <pathspace/ui/declarative/StackReadiness.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <span>

namespace PathSpaceExamples::PaintExampleNew {
namespace {
constexpr std::string_view kPointerDevice = "/system/devices/in/pointer/default";
constexpr std::string_view kKeyboardDevice = "/system/devices/in/text/default";

auto ensure_pointer_device(SP::PathSpace& space) -> SP::Expected<void> {
    auto roots = space.listChildren(SP::ConcretePathStringView{"/system/devices/in/pointer"});
    auto has_default = std::find(roots.begin(), roots.end(), "default") != roots.end();
    if (!has_default) {
        auto device = std::make_unique<SP::PathIOMouse>(SP::PathIOMouse::BackendMode::Off);
        auto inserted = space.insert<"/system/devices/in/pointer/default">(std::move(device));
        if (!inserted.errors.empty()) {
            return std::unexpected(inserted.errors.front());
        }
    }
    return {};
}

auto ensure_keyboard_device(SP::PathSpace& space) -> SP::Expected<void> {
    auto roots = space.listChildren(SP::ConcretePathStringView{"/system/devices/in/text"});
    auto has_default = std::find(roots.begin(), roots.end(), "default") != roots.end();
    if (!has_default) {
        auto device = std::make_unique<SP::PathIOKeyboard>(SP::PathIOKeyboard::BackendMode::Off);
        auto inserted = space.insert<"/system/devices/in/text/default">(std::move(device));
        if (!inserted.errors.empty()) {
            return std::unexpected(inserted.errors.front());
        }
    }
    return {};
}
}

auto EnsureInputDevices(SP::PathSpace& space) -> SP::Expected<void> {
    if (auto ensure_pointer = ensure_pointer_device(space); !ensure_pointer) {
        return std::unexpected(ensure_pointer.error());
    }
    if (auto ensure_keyboard = ensure_keyboard_device(space); !ensure_keyboard) {
        return std::unexpected(ensure_keyboard.error());
    }
    return {};
}

auto MountButtonUI(SP::PathSpace& space,
                   SP::App::ConcretePathView window_view,
                   int window_width,
                   int window_height,
                   SP::UI::Declarative::Button::Args button_args)
    -> SP::Expected<ButtonUiResult> {
    auto button_width = button_args.style.width;
    auto button_height = button_args.style.height;

    SP::UI::Declarative::Stack::Args layout_args{};
    layout_args.style.axis = SP::UI::Builders::Widgets::StackAxis::Vertical;
    layout_args.style.align_main = SP::UI::Builders::Widgets::StackAlignMain::Center;
    layout_args.style.align_cross = SP::UI::Builders::Widgets::StackAlignCross::Center;
    layout_args.style.width = static_cast<float>(window_width);
    layout_args.style.height = static_cast<float>(window_height);

    auto vertical_padding = (layout_args.style.height - button_height) * 0.5f;
    auto horizontal_padding = (layout_args.style.width - button_width) * 0.5f;
    layout_args.style.padding_main_start = std::max(vertical_padding, 0.0f);
    layout_args.style.padding_main_end = std::max(vertical_padding, 0.0f);
    layout_args.style.padding_cross_start = std::max(horizontal_padding, 0.0f);
    layout_args.style.padding_cross_end = std::max(horizontal_padding, 0.0f);

    layout_args.panels.push_back(SP::UI::Declarative::Stack::Panel{
        .id = "button_panel",
        .fragment = SP::UI::Declarative::Button::Fragment(std::move(button_args)),
    });
    layout_args.active_panel = "button_panel";

    auto layout = SP::UI::Declarative::Stack::Create(space,
                                                     window_view,
                                                     "button_panel_root",
                                                     std::move(layout_args));
    if (!layout) {
        return std::unexpected(layout.error());
    }
    if (auto activate = SP::UI::Declarative::Stack::SetActivePanel(space, *layout, "button_panel"); !activate) {
        return std::unexpected(activate.error());
    }

    ButtonUiResult result{};
    result.stack_path = *layout;
    result.button_path = result.stack_path.getPath();
    result.button_path.append("/children/button_panel");
    result.layout_width = static_cast<float>(window_width);
    result.layout_height = static_cast<float>(window_height);

    SP::UI::Declarative::StackReadinessOptions readiness_options{};
    readiness_options.timeout = std::chrono::milliseconds{1500};
    readiness_options.poll_interval = std::chrono::milliseconds{25};
    auto required_children = std::array<std::string_view, 1>{"button_panel"};
    auto ready = SP::UI::Declarative::WaitForStackChildren(space,
                                                           result.stack_path.getPath(),
                                                           std::span<const std::string_view>(required_children));
    if (!ready) {
        return std::unexpected(ready.error());
    }
    return result;
}

auto EnableWindowInput(SP::PathSpace& space,
                       SP::Window::CreateResult const& window,
                       std::string_view telemetry_tag) -> SP::Expected<void> {
    if (auto ensured = EnsureInputDevices(space); !ensured) {
        return ensured;
    }

    ensure_device_push_config(space, std::string{kPointerDevice}, std::string(telemetry_tag));
    ensure_device_push_config(space, std::string{kKeyboardDevice}, std::string(telemetry_tag));

    auto pointer_devices = std::array<std::string, 1>{std::string{kPointerDevice}};
    auto keyboard_devices = std::array<std::string, 1>{std::string{kKeyboardDevice}};

    subscribe_window_devices(space,
                             window.path,
                             std::span<const std::string>(pointer_devices.data(), pointer_devices.size()),
                             std::span<const std::string>{},
                             std::span<const std::string>(keyboard_devices.data(), keyboard_devices.size()));
    return {};
}

} // namespace PathSpaceExamples::PaintExampleNew
