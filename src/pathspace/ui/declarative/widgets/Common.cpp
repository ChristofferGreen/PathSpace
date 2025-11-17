#include "Common.hpp"

#include <pathspace/path/ConcretePath.hpp>
#include <pathspace/ui/Builders.hpp>

#include <atomic>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

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
            .event_name = std::string(event_name),
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
        std::string event_name;
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

public:
    auto rebind(std::string const& from_root,
                std::string const& to_root)
        -> std::vector<std::pair<std::string, HandlerBinding>> {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::pair<std::string, HandlerBinding>> updates;
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->second.widget_root != from_root) {
                ++it;
                continue;
            }
            auto entry = HandlerEntry{
                .widget_root = to_root,
                .event_name = it->second.event_name,
                .kind = it->second.kind,
                .handler = std::move(it->second.handler),
            };
            it = entries_.erase(it);

            auto new_key = compose_id(to_root, entry.event_name);
            HandlerBinding binding{
                .registry_key = new_key,
                .kind = entry.kind,
            };
            entries_.emplace(new_key, std::move(entry));
            updates.emplace_back(entry.event_name, binding);
        }
        return updates;
    }

    auto rebind_by_key(std::string const& registry_key,
                       std::string const& new_root)
        -> std::optional<HandlerBinding> {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(registry_key);
        if (it == entries_.end()) {
            return std::nullopt;
        }

        auto entry = HandlerEntry{
            .widget_root = new_root,
            .event_name = it->second.event_name,
            .kind = it->second.kind,
            .handler = std::move(it->second.handler),
        };
        entries_.erase(it);

        auto new_key = compose_id(new_root, entry.event_name);
        HandlerBinding binding{
            .registry_key = new_key,
            .kind = entry.kind,
        };
        entries_.emplace(new_key, std::move(entry));
        return binding;
    }

    auto resolve(std::string const& registry_key) -> std::optional<HandlerVariant> {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(registry_key);
        if (it == entries_.end()) {
            return std::nullopt;
        }
        return it->second.handler;
    }
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
    return mark_render_dirty(space, root);
}

auto mark_render_dirty(PathSpace& space,
                       std::string const& root) -> SP::Expected<void> {
    if (auto status = write_value(space, root + "/render/dirty", true); !status) {
        return status;
    }
    auto event_path = root + "/render/events/dirty";
    auto inserted = space.insert(event_path, root);
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    return {};
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

auto rebind_handlers(PathSpace& space,
                     std::string const& old_root,
                     std::string const& new_root) -> SP::Expected<void> {
    (void)old_root;
    auto events_base = new_root + "/events";
    auto events = space.listChildren(SP::ConcretePathStringView{events_base});
    for (auto const& event : events) {
        auto handler_path = make_path(make_path(new_root, "events"), event) + "/handler";
        auto binding = space.read<HandlerBinding, std::string>(handler_path);
        if (!binding) {
            continue;
        }
        auto updated = CallbackRegistry::instance().rebind_by_key(binding->registry_key, new_root);
        if (!updated) {
            continue;
        }
        if (auto status = write_value(space, handler_path, *updated); !status) {
            return status;
        }
    }
    return {};
}

auto resolve_handler(std::string const& registry_key) -> std::optional<HandlerVariant> {
    return CallbackRegistry::instance().resolve(registry_key);
}

} // namespace SP::UI::Declarative::Detail
