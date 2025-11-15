#include "Common.hpp"

#include <pathspace/ui/Builders.hpp>

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace SP::UI::Declarative::Detail {
namespace {

class CallbackRegistry {
public:
    static auto instance() -> CallbackRegistry& {
        static CallbackRegistry registry;
        return registry;
    }

    auto store(std::string const& widget_root,
               std::string_view event_name,
               HandlerKind kind,
               HandlerVariant handler) -> std::string {
        std::lock_guard<std::mutex> lock(mutex_);
        if (std::holds_alternative<std::monostate>(handler)) {
            return {};
        }
        auto id = compose_id(widget_root, event_name);
        entries_[id] = HandlerEntry{
            .widget_root = widget_root,
            .kind = kind,
            .handler = std::move(handler),
        };
        return id;
    }

    auto erase_prefix(std::string const& widget_root) -> void {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->second.widget_root == widget_root) {
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    struct HandlerEntry {
        std::string widget_root;
        HandlerKind kind = HandlerKind::None;
        HandlerVariant handler;
    };

    auto compose_id(std::string const& widget_root,
                    std::string_view event_name) -> std::string {
        std::string id = widget_root;
        id.push_back('#');
        id.append(event_name);
        id.push_back('#');
        id.append(std::to_string(counter_.fetch_add(1, std::memory_order_relaxed) + 1));
        return id;
    }

    std::mutex mutex_;
    std::unordered_map<std::string, HandlerEntry> entries_;
    std::atomic<std::uint64_t> counter_{0};
};

} // namespace

auto make_error(std::string message,
                SP::Error::Code code) -> SP::Error {
    return BuilderDetail::make_error(std::move(message), code);
}

auto ensure_widget_name(std::string_view name) -> SP::Expected<void> {
    return BuilderDetail::ensure_identifier(name, "widget name");
}

auto ensure_child_name(std::string_view name) -> SP::Expected<void> {
    return BuilderDetail::ensure_identifier(name, "child name");
}

auto make_path(std::string base, std::string_view component) -> std::string {
    if (!base.empty() && base.back() != '/') {
        base.push_back('/');
    }
    base.append(component);
    return base;
}

auto mount_base(std::string_view parent,
                MountOptions const& options) -> std::string {
    if (!options.slot_override.empty()) {
        return make_path(std::string(parent), options.slot_override);
    }
    if (options.policy == MountPolicy::WindowWidgets) {
        return make_path(std::string(parent), "widgets");
    }
    if (options.policy == MountPolicy::WidgetChildren) {
        return make_path(std::string(parent), "children");
    }
    std::string path{parent};
    auto windows_pos = path.find("/windows/");
    auto widgets_pos = path.find("/widgets/");
    if (widgets_pos == std::string::npos && windows_pos != std::string::npos) {
        return make_path(path, "widgets");
    }
    return make_path(path, "children");
}

auto write_kind(PathSpace& space,
                std::string const& root,
                std::string const& kind) -> SP::Expected<void> {
    return write_value(space, root + "/meta/kind", kind);
}

auto initialize_render(PathSpace& space,
                       std::string const& root,
                       WidgetKind kind) -> SP::Expected<void> {
    if (auto status = write_value(space,
                                  root + "/render/synthesize",
                                  RenderDescriptor{kind});
        !status) {
        return status;
    }
    return write_value(space, root + "/render/dirty", true);
}

auto write_handler(PathSpace& space,
                   std::string const& root,
                   std::string_view event,
                   HandlerKind kind,
                   HandlerVariant handler) -> SP::Expected<void> {
    if (std::holds_alternative<std::monostate>(handler)) {
        return {};
    }
    auto key = CallbackRegistry::instance().store(root, event, kind, std::move(handler));
    if (key.empty()) {
        return {};
    }
    HandlerBinding binding{
        .registry_key = std::move(key),
        .kind = kind,
    };
    auto path = make_path(make_path(root, "events"), event);
    path.append("/handler");
    return write_value(space, path, binding);
}

auto clear_handlers(std::string const& widget_root) -> void {
    CallbackRegistry::instance().erase_prefix(widget_root);
}

} // namespace SP::UI::Declarative::Detail

