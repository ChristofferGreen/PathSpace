#include "Common.hpp"

#include <pathspace/path/ConcretePath.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>

#include <atomic>
#include <cstdint>
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
        auto handler_path = handler_path_string(widget_root, event_name);
        entries_[id] = HandlerEntry{
            .widget_root = widget_root,
            .event_name = std::string(event_name),
            .kind = kind,
            .handler = std::move(handler),
            .handler_path = handler_path,
        };
        path_index_[handler_path] = id;
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
    auto handler_path_string(std::string const& widget_root,
                             std::string_view event_name) -> std::string {
        auto path = make_path(widget_root, "events");
        path = make_path(std::move(path), event_name);
        return make_path(std::move(path), "handler");
    }

    struct HandlerEntry {
        std::string widget_root;
        std::string event_name;
        HandlerKind kind = HandlerKind::None;
        HandlerVariant handler;
        std::string handler_path;
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
    std::unordered_map<std::string, std::string> path_index_;
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
                .handler_path = handler_path_string(to_root, it->second.event_name),
            };
            it = entries_.erase(it);

            auto new_key = compose_id(to_root, entry.event_name);
            HandlerBinding binding{
                .registry_key = new_key,
                .kind = entry.kind,
            };
            entries_.emplace(new_key, entry);
            path_index_[entry.handler_path] = new_key;
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
            .handler_path = handler_path_string(new_root, it->second.event_name),
        };
        entries_.erase(it);

        auto new_key = compose_id(new_root, entry.event_name);
        HandlerBinding binding{
            .registry_key = new_key,
            .kind = entry.kind,
        };
        entries_.emplace(new_key, entry);
        path_index_[entry.handler_path] = new_key;
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

    auto erase(std::string const& registry_key) -> void {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.erase(registry_key);
        for (auto it = path_index_.begin(); it != path_index_.end();) {
            if (it->second == registry_key) {
                it = path_index_.erase(it);
            } else {
                ++it;
            }
        }
    }

    auto unlink_path(std::string const& handler_path) -> std::optional<std::string> {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = path_index_.find(handler_path);
        if (it == path_index_.end()) {
            return std::nullopt;
        }
        auto key = it->second;
        path_index_.erase(it);
        return key;
    }

    auto register_path(std::string handler_path, std::string registry_key) -> void {
        std::lock_guard<std::mutex> lock(mutex_);
        path_index_[std::move(handler_path)] = std::move(registry_key);
    }
};

} // namespace

auto make_path(std::string base, std::string_view component) -> std::string {
    if (!base.empty() && base.back() != '/') {
        base.push_back('/');
    }
    base.append(component);
    return base;
}

auto handler_binding_path(std::string const& root, std::string_view event) -> std::string {
    auto path = make_path(root, "events");
    path = make_path(std::move(path), event);
    return make_path(std::move(path), "handler");
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
    if (auto status = write_value(space, root + "/render/dirty_version", std::uint64_t{0}); !status) {
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
    auto version_path = root + "/render/dirty_version";
    auto current_version = space.read<std::uint64_t, std::string>(version_path);
    std::uint64_t next_version = current_version ? (*current_version + 1) : 1;
    if (auto status = replace_single<std::uint64_t>(space, version_path, next_version); !status) {
        return status;
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
    auto path = handler_binding_path(root, event);
    if (auto previous = CallbackRegistry::instance().unlink_path(path); previous) {
        CallbackRegistry::instance().erase(*previous);
    }
    auto key = CallbackRegistry::instance().store(root, event, kind, std::move(handler));
    if (key.empty()) {
        return {};
    }
    HandlerBinding binding{
        .registry_key = key,
        .kind = kind,
    };
    return write_value(space, path, binding);
}

auto write_fragment_handlers(PathSpace& space,
                             std::string const& root,
                             std::vector<FragmentHandler> const& handlers) -> SP::Expected<void> {
    for (auto const& handler : handlers) {
        if (auto status = write_handler(space,
                                        root,
                                        handler.event,
                                        handler.kind,
                                        handler.handler);
            !status) {
            return status;
        }
    }
    return {};
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

auto read_handler_binding(PathSpace& space,
                          std::string const& root,
                          std::string_view event)
    -> SP::Expected<std::optional<HandlerBinding>> {
    auto path = handler_binding_path(root, event);
    auto binding = space.read<HandlerBinding, std::string>(path);
    if (!binding) {
        auto const& error = binding.error();
        if (error.code == SP::Error::Code::NoObjectFound
            || error.code == SP::Error::Code::NoSuchPath) {
            return std::optional<HandlerBinding>{};
        }
        return std::unexpected(error);
    }
    return std::optional<HandlerBinding>{*binding};
}

auto clear_handler_binding(PathSpace& space,
                           std::string const& root,
                           std::string_view event) -> SP::Expected<void> {
    auto path = handler_binding_path(root, event);
    if (auto previous = CallbackRegistry::instance().unlink_path(path); previous) {
        CallbackRegistry::instance().erase(*previous);
    }
    auto removed = space.take<HandlerBinding>(path);
    if (!removed) {
        auto const& error = removed.error();
        if (error.code == SP::Error::Code::NoObjectFound
            || error.code == SP::Error::Code::NoSuchPath) {
            return {};
        }
        return std::unexpected(error);
    }
    return {};
}

} // namespace SP::UI::Declarative::Detail
