#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/core/Error.hpp>
#include <pathspace/ui/DetailShared.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/WidgetSharedTypes.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace SP::UI::Declarative::Detail {

inline auto make_error(std::string message,
                       SP::Error::Code code = SP::Error::Code::UnknownError) -> SP::Error {
    return SP::Error{code, std::move(message)};
}

inline auto ensure_non_empty(std::string_view value,
                             std::string_view what) -> SP::Expected<void> {
    if (value.empty()) {
        return std::unexpected(
            make_error(std::string(what) + " must not be empty", SP::Error::Code::InvalidPath));
    }
    return {};
}

inline auto ensure_identifier(std::string_view value,
                              std::string_view what) -> SP::Expected<void> {
    if (auto status = ensure_non_empty(value, what); !status) {
        return status;
    }
    if (value == "." || value == "..") {
        return std::unexpected(
            make_error(std::string(what) + " must not be '.' or '..'",
                       SP::Error::Code::InvalidPathSubcomponent));
    }
    if (value.find('/') != std::string_view::npos) {
        return std::unexpected(
            make_error(std::string(what) + " must not contain '/' characters",
                       SP::Error::Code::InvalidPathSubcomponent));
    }
    return {};
}

template <typename T>
inline auto drain_queue(PathSpace& space, std::string const& path) -> SP::Expected<void> {
    while (true) {
        auto taken = space.take<T>(path);
        if (taken) {
            continue;
        }
        auto const& error = taken.error();
        if (error.code == SP::Error::Code::NoObjectFound
            || error.code == SP::Error::Code::NoSuchPath) {
            break;
        }
        return std::unexpected(error);
    }
    return {};
}

template <typename T>
inline auto replace_single(PathSpace& space,
                           std::string const& path,
                           T const& value) -> SP::Expected<void> {
    if (auto cleared = drain_queue<T>(space, path); !cleared) {
        return cleared;
    }
    auto result = space.insert(path, value);
    if (!result.errors.empty()) {
        return std::unexpected(result.errors.front());
    }
    return {};
}

template <typename T>
inline auto read_optional(PathSpace const& space,
                          std::string const& path) -> SP::Expected<std::optional<T>> {
    auto value = space.read<T, std::string>(path);
    if (value) {
        return std::optional<T>{*value};
    }
    auto const& error = value.error();
    if (error.code == SP::Error::Code::NoObjectFound
        || error.code == SP::Error::Code::NoSuchPath) {
        return std::optional<T>{};
    }
    return std::unexpected(error);
}

inline auto derive_app_root_for(SP::App::ConcretePathView absolute)
    -> SP::Expected<SP::App::AppRootPath> {
    return SP::App::derive_app_root(absolute);
}

inline auto window_component_for(std::string_view absolute) -> SP::Expected<std::string> {
    auto path = std::string{absolute};
    auto windows_pos = path.find("/windows/");
    if (windows_pos == std::string::npos) {
        return std::unexpected(
            make_error("path '" + path + "' missing '/windows/<id>' segment",
                       SP::Error::Code::InvalidPath));
    }
    windows_pos += std::strlen("/windows/");
    auto next_slash = path.find('/', windows_pos);
    if (next_slash == std::string::npos) {
        next_slash = path.size();
    }
    return std::string(path.substr(windows_pos, next_slash - windows_pos));
}

inline auto to_epoch_ns(std::chrono::system_clock::time_point tp) -> std::uint64_t {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count());
}

inline std::atomic<std::uint64_t>& g_widget_op_sequence =
    SP::UI::DetailShared::widget_op_sequence();

namespace Widgets = SP::UI::Builders::Widgets;

auto write_stack_metadata(PathSpace& space,
                          std::string const& rootPath,
                          Widgets::StackLayoutStyle const& style,
                          std::vector<Widgets::StackChildSpec> const& children,
                          Widgets::StackLayoutState const& layout) -> SP::Expected<void>;

auto compute_stack_layout_state(PathSpace& space,
                                Widgets::StackLayoutParams const& params)
    -> SP::Expected<Widgets::StackLayoutState>;

void translate_bucket(SP::UI::Scene::DrawableBucketSnapshot& bucket, float x, float y);
void append_bucket(SP::UI::Scene::DrawableBucketSnapshot& target,
                   SP::UI::Scene::DrawableBucketSnapshot const& source);

auto build_text_field_bucket(Widgets::TextFieldStyle const& style,
                             Widgets::TextFieldState const& state,
                             std::string_view authoring_root,
                             bool pulsing_highlight = false) -> SP::UI::Scene::DrawableBucketSnapshot;


} // namespace SP::UI::Declarative::Detail
