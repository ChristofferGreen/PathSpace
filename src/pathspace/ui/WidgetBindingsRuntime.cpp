#include "WidgetDetail.hpp"

#include <pathspace/ui/declarative/Widgets.hpp>
#include <pathspace/ui/declarative/widgets/Common.hpp>

#include <cmath>
#include <functional>
#include <limits>
#include <optional>

namespace SP::UI::Runtime::Widgets::Bindings {

using namespace Detail;
namespace DeclarativeDetail = SP::UI::Declarative::Detail;
using SP::UI::Declarative::ButtonContext;
using SP::UI::Declarative::ButtonHandler;
using SP::UI::Declarative::HandlerKind;
using SP::UI::Declarative::HandlerVariant;
using SP::UI::Declarative::InputFieldContext;
using SP::UI::Declarative::InputFieldHandler;
using SP::UI::Declarative::LabelContext;
using SP::UI::Declarative::LabelHandler;
using SP::UI::Declarative::ListChildContext;
using SP::UI::Declarative::ListChildHandler;
using SP::UI::Declarative::PaintSurfaceContext;
using SP::UI::Declarative::PaintSurfaceHandler;
using SP::UI::Declarative::SliderContext;
using SP::UI::Declarative::SliderHandler;
using SP::UI::Declarative::StackPanelContext;
using SP::UI::Declarative::StackPanelHandler;
using SP::UI::Declarative::ToggleContext;
using SP::UI::Declarative::ToggleHandler;
using SP::UI::Declarative::TreeNodeContext;
using SP::UI::Declarative::TreeNodeHandler;

namespace {

struct HandlerInvocationInfo {
    std::optional<std::string> target_id{};
    std::optional<float> value{};
};

auto read_list_items(PathSpace& space,
                     ListPaths const& paths) -> SP::Expected<std::vector<Widgets::ListItem>>;

auto write_widget_footprint(PathSpace& space,
                            WidgetPath const& root,
                            DirtyRectHint footprint) -> SP::Expected<void> {
    DirtyRectHint normalized = ensure_valid_hint(footprint);
    if (normalized.max_x <= normalized.min_x || normalized.max_y <= normalized.min_y) {
        return {};
    }
    auto path = std::string(root.getPath()) + "/meta/footprint";
    if (auto status = replace_single<DirtyRectHint>(space, path, normalized); !status) {
        return std::unexpected(status.error());
    }
    return {};
}

auto compute_ops_queue(WidgetPath const& root) -> ConcretePath {
    return ConcretePath{std::string(root.getPath()) + "/ops/inbox/queue"};
}

auto build_options(AppRootPathView appRoot,
                   WidgetPath const& root,
                   ConcretePathView targetPath,
                   DirtyRectHint hint,
                   bool auto_render) -> BindingOptions {
    BindingOptions options{
        .target = ConcretePath{std::string(targetPath.getPath())},
        .ops_queue = compute_ops_queue(root),
        .dirty_rect = ensure_valid_hint(hint),
        .auto_render = auto_render,
    };
    options.focus_state = Widgets::Focus::FocusStatePath(appRoot);
    options.focus_enabled = true;
    return options;
}

auto read_frame_index(PathSpace& space, std::string const& target) -> SP::Expected<std::uint64_t> {
    auto frame = read_optional<std::uint64_t>(space, target + "/output/v1/common/frameIndex");
    if (!frame) {
        return std::unexpected(frame.error());
    }
    if (frame->has_value()) {
        return **frame;
    }
    return std::uint64_t{0};
}

auto submit_dirty_hint(PathSpace& space,
                       BindingOptions const& options) -> SP::Expected<void> {
    auto const& rect = options.dirty_rect;
    if (rect.max_x <= rect.min_x || rect.max_y <= rect.min_y) {
        return {};
    }
    std::array<DirtyRectHint, 1> hints{rect};
    return Renderer::SubmitDirtyRects(space,
                                      SP::ConcretePathStringView{options.target.getPath()},
                                      std::span<const DirtyRectHint>(hints.data(), hints.size()));
}

auto emit_action_callbacks(BindingOptions const& options,
                           WidgetOp const& op) -> void {
    if (options.action_callbacks.empty()) {
        return;
    }

    auto callbacks = options.action_callbacks;
    auto action = Reducers::MakeWidgetAction(op);
    for (auto const& callback : callbacks) {
        if (!callback || !(*callback)) {
            continue;
        }
        (*callback)(action);
    }
}

auto set_widget_focus(PathSpace& space,
                      BindingOptions const& options,
                      WidgetPath const& widget) -> SP::Expected<bool> {
    if (!options.focus_enabled) {
        return false;
    }

    Widgets::Focus::Config config{
        .focus_state = options.focus_state,
        .auto_render_target = options.auto_render
            ? std::optional<ConcretePath>{options.target}
            : std::optional<ConcretePath>{},
    };
    auto result = Widgets::Focus::Set(space, config, widget);
    if (!result) {
        return std::unexpected(result.error());
    }
    return result->changed;
}

auto schedule_auto_render(PathSpace& space,
                          BindingOptions const& options,
                          std::string_view reason) -> SP::Expected<void> {
    if (!options.auto_render) {
        return {};
    }
    auto frame_index = read_frame_index(space, options.target.getPath());
    if (!frame_index) {
        return std::unexpected(frame_index.error());
    }
    return enqueue_auto_render_event(space,
                                     options.target.getPath(),
                                     reason,
                                     *frame_index);
}

auto invoke_handler_if_present(PathSpace& space,
                               WidgetPath const& widget,
                               std::string_view event,
                               HandlerInvocationInfo const& info = {})
    -> SP::Expected<void> {
    if (event.empty()) {
        return {};
    }

    auto binding = DeclarativeDetail::read_handler_binding(space, widget.getPath(), event);
    if (!binding) {
        auto const& error = binding.error();
        if (error.code == SP::Error::Code::NoObjectFound
            || error.code == SP::Error::Code::NoSuchPath) {
            return {};
        }
        return std::unexpected(error);
    }
    if (!binding->has_value()) {
        return {};
    }

    auto handler = DeclarativeDetail::resolve_handler(binding->value().registry_key);
    if (!handler || std::holds_alternative<std::monostate>(*handler)) {
        return {};
    }

    auto kind_mismatch = [&]() -> SP::Expected<void> {
        return std::unexpected(make_error("handler kind mismatch for event '" + std::string(event) + "'",
                                          SP::Error::Code::InvalidType));
    };

    switch (binding->value().kind) {
    case HandlerKind::ButtonPress: {
        auto fn = std::get_if<ButtonHandler>(&*handler);
        if (!fn) {
            return kind_mismatch();
        }
        ButtonContext ctx{space, widget};
        (*fn)(ctx);
        return {};
    }
    case HandlerKind::Toggle: {
        auto fn = std::get_if<ToggleHandler>(&*handler);
        if (!fn) {
            return kind_mismatch();
        }
        ToggleContext ctx{space, widget};
        (*fn)(ctx);
        return {};
    }
    case HandlerKind::Slider: {
        auto fn = std::get_if<SliderHandler>(&*handler);
        if (!fn) {
            return kind_mismatch();
        }
        SliderContext ctx{space, widget};
        if (info.value) {
            ctx.value = *info.value;
        } else {
            auto state = space.read<Widgets::SliderState, std::string>(
                WidgetSpacePath(widget.getPath(), "/state"));
            if (!state) {
                return std::unexpected(state.error());
            }
            ctx.value = state->value;
        }
        (*fn)(ctx);
        return {};
    }
    case HandlerKind::ListChild: {
        auto fn = std::get_if<ListChildHandler>(&*handler);
        if (!fn) {
            return kind_mismatch();
        }
        ListChildContext ctx{space, widget};
        if (info.target_id) {
            ctx.child_id = *info.target_id;
        } else {
            auto state = space.read<Widgets::ListState, std::string>(
                WidgetSpacePath(widget.getPath(), "/state"));
            if (!state) {
                return std::unexpected(state.error());
            }
            if (state->selected_index >= 0) {
                auto items = space.read<std::vector<Widgets::ListItem>, std::string>(
                    WidgetSpacePath(widget.getPath(), "/meta/items"));
                if (!items) {
                    return std::unexpected(items.error());
                }
                auto idx = static_cast<std::size_t>(state->selected_index);
                if (idx < items->size()) {
                    ctx.child_id = (*items)[idx].id;
                }
            }
        }
        (*fn)(ctx);
        return {};
    }
    case HandlerKind::TreeNode: {
        auto fn = std::get_if<TreeNodeHandler>(&*handler);
        if (!fn) {
            return kind_mismatch();
        }
        TreeNodeContext ctx{space, widget};
        if (info.target_id) {
            ctx.node_id = *info.target_id;
        }
        (*fn)(ctx);
        return {};
    }
    case HandlerKind::StackPanel: {
        auto fn = std::get_if<StackPanelHandler>(&*handler);
        if (!fn) {
            return kind_mismatch();
        }
        StackPanelContext ctx{space, widget};
        if (info.target_id) {
            ctx.panel_id = *info.target_id;
        }
        (*fn)(ctx);
        return {};
    }
    case HandlerKind::LabelActivate: {
        auto fn = std::get_if<LabelHandler>(&*handler);
        if (!fn) {
            return kind_mismatch();
        }
        LabelContext ctx{space, widget};
        (*fn)(ctx);
        return {};
    }
    case HandlerKind::InputChange:
    case HandlerKind::InputSubmit: {
        auto fn = std::get_if<InputFieldHandler>(&*handler);
        if (!fn) {
            return kind_mismatch();
        }
        InputFieldContext ctx{space, widget};
        (*fn)(ctx);
        return {};
    }
    case HandlerKind::PaintDraw: {
        auto fn = std::get_if<PaintSurfaceHandler>(&*handler);
        if (!fn) {
            return kind_mismatch();
        }
        PaintSurfaceContext ctx{space, widget};
        (*fn)(ctx);
        return {};
    }
    case HandlerKind::None:
        return {};
    }

    return kind_mismatch();
}

auto enqueue_widget_op(PathSpace& space,
                       BindingOptions const& options,
                       std::string const& widget_path,
                       WidgetOpKind kind,
                       PointerInfo const& pointer,
                       float value,
                       std::string_view target_id = {}) -> SP::Expected<void> {
    WidgetOp op{};
    op.kind = kind;
    op.widget_path = widget_path;
    op.target_id = target_id;
    op.pointer = pointer;
    op.value = value;
    op.sequence = g_widget_op_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    op.timestamp_ns = to_epoch_ns(std::chrono::system_clock::now());

    auto inserted = space.insert(options.ops_queue.getPath(), op);
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    emit_action_callbacks(options, op);
    return {};
}

auto is_utf8_continuation(char c) -> bool {
    return (static_cast<unsigned char>(c) & 0xC0u) == 0x80u;
}

auto clamp_index(std::string const& text, std::uint32_t index) -> std::uint32_t {
    return std::min<std::uint32_t>(index, static_cast<std::uint32_t>(text.size()));
}

auto utf8_next_index(std::string const& text, std::uint32_t index) -> std::uint32_t {
    index = clamp_index(text, index);
    if (index >= text.size()) {
        return static_cast<std::uint32_t>(text.size());
    }
    ++index;
    while (index < text.size() && is_utf8_continuation(text[index])) {
        ++index;
    }
    return clamp_index(text, index);
}

auto utf8_prev_index(std::string const& text, std::uint32_t index) -> std::uint32_t {
    index = clamp_index(text, index);
    if (index == 0) {
        return 0;
    }
    --index;
    while (index > 0 && is_utf8_continuation(text[index])) {
        --index;
    }
    return clamp_index(text, index);
}

template <typename State>
auto normalized_selection(State const& state) -> std::pair<std::uint32_t, std::uint32_t> {
    auto length = static_cast<std::uint32_t>(state.text.size());
    std::uint32_t start = std::min(state.selection_start, state.selection_end);
    std::uint32_t end = std::max(state.selection_start, state.selection_end);
    start = std::min(start, length);
    end = std::min(end, length);
    return {start, end};
}

template <typename State>
auto collapse_selection(State& state, std::uint32_t index) -> void {
    index = clamp_index(state.text, index);
    state.cursor = index;
    state.selection_start = index;
    state.selection_end = index;
    state.composition_start = index;
    state.composition_end = index;
}

template <typename State>
auto clear_composition(State& state) -> void {
    state.composition_active = false;
    state.composition_text.clear();
    state.composition_start = clamp_index(state.text, state.cursor);
    state.composition_end = state.composition_start;
}

template <typename State>
auto erase_selection(State& state) -> bool {
    auto [start, end] = normalized_selection(state);
    if (start == end) {
        return false;
    }
    state.text.erase(start, end - start);
    collapse_selection(state, start);
    clear_composition(state);
    return true;
}

auto encode_utf8(char32_t ch) -> std::string {
    std::string out;
    if (ch <= 0x7Fu) {
        out.push_back(static_cast<char>(ch));
        return out;
    }
    if (ch <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | ((ch >> 6) & 0x1Fu)));
        out.push_back(static_cast<char>(0x80u | (ch & 0x3Fu)));
        return out;
    }
    if (ch <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | ((ch >> 12) & 0x0Fu)));
        out.push_back(static_cast<char>(0x80u | ((ch >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (ch & 0x3Fu)));
        return out;
    }
    out.push_back(static_cast<char>(0xF0u | ((ch >> 18) & 0x07u)));
    out.push_back(static_cast<char>(0x80u | ((ch >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((ch >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (ch & 0x3Fu)));
    return out;
}

auto sanitize_single_line(std::string_view text) -> std::string {
    std::string filtered;
    filtered.reserve(text.size());
    for (char c : text) {
        if (c == '\n' || c == '\r') {
            continue;
        }
        filtered.push_back(c);
    }
    return filtered;
}

template <typename State>
auto insert_text(State& state, std::string_view text, bool allow_newlines) -> bool {
    bool removed = erase_selection(state);
    std::string value;
    if (allow_newlines) {
        value.assign(text.begin(), text.end());
    } else {
        value = sanitize_single_line(text);
    }
    if (value.empty()) {
        return removed;
    }
    auto insert_at = clamp_index(state.text, state.cursor);
    state.text.insert(insert_at, value);
    collapse_selection(state, insert_at + static_cast<std::uint32_t>(value.size()));
    clear_composition(state);
    return true;
}

template <typename State>
auto delete_single(State& state, bool forward) -> bool {
    if (erase_selection(state)) {
        return true;
    }
    auto length = static_cast<std::uint32_t>(state.text.size());
    if (forward) {
        if (state.cursor >= length) {
            return false;
        }
        auto next = utf8_next_index(state.text, state.cursor);
        if (next == state.cursor) {
            return false;
        }
        state.text.erase(state.cursor, next - state.cursor);
        collapse_selection(state, state.cursor);
        clear_composition(state);
        return true;
    }
    if (state.cursor == 0) {
        return false;
    }
    auto prev = utf8_prev_index(state.text, state.cursor);
    state.text.erase(prev, state.cursor - prev);
    collapse_selection(state, prev);
    clear_composition(state);
    return true;
}

template <typename State>
auto move_cursor(State& state, int delta) -> bool {
    if (delta == 0) {
        return false;
    }
    auto target = state.cursor;
    if (delta > 0) {
        for (int i = 0; i < delta; ++i) {
            auto next = utf8_next_index(state.text, target);
            if (next == target) {
                break;
            }
            target = next;
        }
    } else {
        for (int i = 0; i < -delta; ++i) {
            auto prev = utf8_prev_index(state.text, target);
            if (prev == target) {
                break;
            }
            target = prev;
        }
    }
    if (target == state.cursor) {
        return false;
    }
    collapse_selection(state, target);
    clear_composition(state);
    return true;
}

template <typename State>
auto apply_selection_payload(State& state, State const& payload) -> bool {
    auto length = static_cast<std::uint32_t>(state.text.size());
    std::uint32_t start = std::min(payload.selection_start, length);
    std::uint32_t end = std::min(payload.selection_end, length);
    if (start == state.selection_start && end == state.selection_end) {
        return false;
    }
    state.selection_start = start;
    state.selection_end = end;
    state.cursor = std::min(payload.cursor, length);
    state.composition_start = std::min(payload.composition_start, length);
    state.composition_end = std::min(payload.composition_end, length);
    return true;
}

template <typename State>
auto begin_composition(State& state) -> bool {
    auto [start, end] = normalized_selection(state);
    state.composition_start = start;
    state.composition_end = end;
    state.composition_text.clear();
    if (!state.composition_active) {
        state.composition_active = true;
        return true;
    }
    return false;
}

template <typename State>
auto update_composition(State& state,
                        std::string const& text,
                        std::uint32_t start,
                        std::uint32_t end,
                        bool allow_newlines) -> bool {
    auto length = static_cast<std::uint32_t>(state.text.size());
    state.composition_start = std::min(start, length);
    state.composition_end = std::min(end, length);
    std::string value = allow_newlines ? text : sanitize_single_line(text);
    bool changed = (state.composition_text != value) || !state.composition_active;
    state.composition_text = std::move(value);
    state.composition_active = true;
    return changed;
}

template <typename State>
auto commit_composition(State& state, bool allow_newlines) -> bool {
    if (!state.composition_active) {
        return false;
    }
    auto start = std::min(state.composition_start, state.composition_end);
    auto end = std::max(state.composition_start, state.composition_end);
    start = clamp_index(state.text, start);
    end = clamp_index(state.text, end);
    state.text.erase(start, end - start);
    std::string value = allow_newlines ? state.composition_text : sanitize_single_line(state.composition_text);
    state.text.insert(start, value);
    collapse_selection(state, start + static_cast<std::uint32_t>(value.size()));
    state.composition_text.clear();
    state.composition_active = false;
    return true;
}

template <typename State>
auto cancel_composition(State& state) -> bool {
    if (!state.composition_active) {
        return false;
    }
    collapse_selection(state, state.composition_start);
    state.composition_text.clear();
    state.composition_active = false;
    return true;
}

template <typename State>
auto selection_text(State const& state) -> std::string {
    auto [start, end] = normalized_selection(state);
    if (start >= end) {
        return {};
    }
    return state.text.substr(start, end - start);
}

auto clipboard_text_path(WidgetPath const& root) -> std::string {
    std::string path = std::string(root.getPath());
    path.append("/ops/clipboard/last_text");
    return path;
}

auto write_clipboard_text(PathSpace& space,
                          WidgetPath const& root,
                          std::string const& text) -> SP::Expected<void> {
    auto path = clipboard_text_path(root);
    if (auto status = replace_single<std::string>(space, path, text); !status) {
        return std::unexpected(status.error());
    }
    return {};
}

auto read_clipboard_text(PathSpace& space,
                         WidgetPath const& root) -> SP::Expected<std::string> {
    auto path = clipboard_text_path(root);
    auto stored = read_optional<std::string>(space, path);
    if (!stored) {
        return std::unexpected(stored.error());
    }
    if (!stored->has_value()) {
        return std::string{};
    }
    return **stored;
}

template <typename State>
auto copy_selection(PathSpace& space,
                    WidgetPath const& root,
                    State const& state) -> SP::Expected<void> {
    return write_clipboard_text(space, root, selection_text(state));
}

inline auto set_flag(bool& field, bool value) -> bool {
    if (field == value) {
        return false;
    }
    field = value;
    return true;
}

auto read_button_style(PathSpace& space,
                       ButtonPaths const& paths) -> SP::Expected<Widgets::ButtonStyle> {
    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    return space.read<Widgets::ButtonStyle, std::string>(stylePath);
}

auto read_toggle_style(PathSpace& space,
                       TogglePaths const& paths) -> SP::Expected<Widgets::ToggleStyle> {
    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    return space.read<Widgets::ToggleStyle, std::string>(stylePath);
}

auto read_slider_style(PathSpace& space,
                       SliderPaths const& paths) -> SP::Expected<Widgets::SliderStyle> {
    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    return space.read<Widgets::SliderStyle, std::string>(stylePath);
}

auto read_list_style(PathSpace& space,
                     ListPaths const& paths) -> SP::Expected<Widgets::ListStyle> {
    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    return space.read<Widgets::ListStyle, std::string>(stylePath);
}

auto read_list_items(PathSpace& space,
                     ListPaths const& paths) -> SP::Expected<std::vector<Widgets::ListItem>> {
    auto itemsPath = std::string(paths.root.getPath()) + "/meta/items";
    return space.read<std::vector<Widgets::ListItem>, std::string>(itemsPath);
}

auto read_tree_style(PathSpace& space,
                     TreePaths const& paths) -> SP::Expected<Widgets::TreeStyle> {
    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    return space.read<Widgets::TreeStyle, std::string>(stylePath);
}

auto read_tree_nodes(PathSpace& space,
                     TreePaths const& paths) -> SP::Expected<std::vector<Widgets::TreeNode>> {
    return space.read<std::vector<Widgets::TreeNode>, std::string>(paths.nodes.getPath());
}

auto read_text_field_style(PathSpace& space,
                           TextFieldPaths const& paths) -> SP::Expected<Widgets::TextFieldStyle> {
    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    return space.read<Widgets::TextFieldStyle, std::string>(stylePath);
}

auto read_text_area_style(PathSpace& space,
                          TextAreaPaths const& paths) -> SP::Expected<Widgets::TextAreaStyle> {
    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    return space.read<Widgets::TextAreaStyle, std::string>(stylePath);
}

} // namespace

auto PointerFromHit(Scene::HitTestResult const& hit) -> PointerInfo {
    auto pointer = PointerInfo::Make(hit.position.scene_x, hit.position.scene_y)
                        .WithInside(hit.hit);
    if (hit.position.has_local) {
        pointer.WithLocal(hit.position.local_x, hit.position.local_y);
    }
    return pointer;
}

auto CreateButtonBinding(PathSpace& space,
                         AppRootPathView appRoot,
                         ButtonPaths const& paths,
                         ConcretePathView targetPath,
                         DirtyRectHint footprint,
                         std::optional<DirtyRectHint> dirty_override,
                         bool auto_render) -> SP::Expected<ButtonBinding> {
    if (auto style = read_button_style(space, paths); !style) {
        return std::unexpected(style.error());
    }
    DirtyRectHint hint = ensure_valid_hint(dirty_override.value_or(footprint));
    if (auto status = write_widget_footprint(space, paths.root, hint); !status) {
        return std::unexpected(status.error());
    }
    ButtonBinding binding{
        .widget = paths,
        .options = build_options(appRoot,
                                 paths.root,
                                 targetPath,
                                 hint,
                                 auto_render),
    };
    return binding;
}

auto CreateToggleBinding(PathSpace& space,
                         AppRootPathView appRoot,
                         TogglePaths const& paths,
                         ConcretePathView targetPath,
                         DirtyRectHint footprint,
                         std::optional<DirtyRectHint> dirty_override,
                         bool auto_render) -> SP::Expected<ToggleBinding> {
    if (auto style = read_toggle_style(space, paths); !style) {
        return std::unexpected(style.error());
    }
    DirtyRectHint hint = ensure_valid_hint(dirty_override.value_or(footprint));
    if (auto status = write_widget_footprint(space, paths.root, hint); !status) {
        return std::unexpected(status.error());
    }
    ToggleBinding binding{
        .widget = paths,
        .options = build_options(appRoot, paths.root, targetPath, hint, auto_render),
    };
    return binding;
}

auto CreateSliderBinding(PathSpace& space,
                         AppRootPathView appRoot,
                         SliderPaths const& paths,
                         ConcretePathView targetPath,
                         DirtyRectHint footprint,
                         std::optional<DirtyRectHint> dirty_override,
                         bool auto_render) -> SP::Expected<SliderBinding> {
    if (auto style = read_slider_style(space, paths); !style) {
        return std::unexpected(style.error());
    }
    DirtyRectHint hint = ensure_valid_hint(dirty_override.value_or(footprint));
    if (auto status = write_widget_footprint(space, paths.root, hint); !status) {
        return std::unexpected(status.error());
    }
    SliderBinding binding{
        .widget = paths,
        .options = build_options(appRoot, paths.root, targetPath, hint, auto_render),
    };
    return binding;
}

auto CreateListBinding(PathSpace& space,
                       AppRootPathView appRoot,
                       ListPaths const& paths,
                       ConcretePathView targetPath,
                       DirtyRectHint footprint,
                       std::optional<DirtyRectHint> dirty_override,
                       bool auto_render) -> SP::Expected<ListBinding> {
    if (auto style = read_list_style(space, paths); !style) {
        return std::unexpected(style.error());
    }
    if (auto items = read_list_items(space, paths); !items) {
        return std::unexpected(items.error());
    }

    DirtyRectHint hint = ensure_valid_hint(dirty_override.value_or(footprint));
    if (auto status = write_widget_footprint(space, paths.root, hint); !status) {
        return std::unexpected(status.error());
    }
    ListBinding binding{
        .widget = paths,
        .options = build_options(appRoot, paths.root, targetPath, hint, auto_render),
    };
    return binding;
}

auto CreateTreeBinding(PathSpace& space,
                       AppRootPathView appRoot,
                       TreePaths const& paths,
                       ConcretePathView targetPath,
                       DirtyRectHint footprint,
                       std::optional<DirtyRectHint> dirty_override,
                       bool auto_render) -> SP::Expected<TreeBinding> {
    if (auto style = read_tree_style(space, paths); !style) {
        return std::unexpected(style.error());
    }
    if (auto nodes = read_tree_nodes(space, paths); !nodes) {
        return std::unexpected(nodes.error());
    }

    DirtyRectHint hint = ensure_valid_hint(dirty_override.value_or(footprint));
    if (auto status = write_widget_footprint(space, paths.root, hint); !status) {
        return std::unexpected(status.error());
    }
    TreeBinding binding{
        .widget = paths,
        .options = build_options(appRoot, paths.root, targetPath, hint, auto_render),
    };
    return binding;
}

auto CreateStackBinding(PathSpace& space,
                        AppRootPathView appRoot,
                        StackPaths const& paths,
                        ConcretePathView targetPath,
                        DirtyRectHint footprint,
                        std::optional<DirtyRectHint> dirty_override,
                        bool auto_render) -> SP::Expected<StackBinding> {
    auto layout = Widgets::ReadStackLayout(space, paths);
    if (!layout) {
        return std::unexpected(layout.error());
    }

    DirtyRectHint hint = ensure_valid_hint(dirty_override.value_or(footprint));
    if (auto status = write_widget_footprint(space, paths.root, hint); !status) {
        return std::unexpected(status.error());
    }
    StackBinding binding{
        .layout = paths,
        .options = build_options(appRoot, paths.root, targetPath, hint, auto_render),
    };
    return binding;
}

auto CreateTextFieldBinding(PathSpace& space,
                            AppRootPathView appRoot,
                            TextFieldPaths const& paths,
                            ConcretePathView targetPath,
                            DirtyRectHint footprint,
                            std::optional<DirtyRectHint> dirty_override,
                            bool auto_render) -> SP::Expected<TextFieldBinding> {
    auto style = read_text_field_style(space, paths);
    if (!style) {
        return std::unexpected(style.error());
    }

    DirtyRectHint hint = ensure_valid_hint(dirty_override.value_or(text_input_dirty_hint(*style)));
    if (auto status = write_widget_footprint(space, paths.root, hint); !status) {
        return std::unexpected(status.error());
    }

    TextFieldBinding binding{
        .widget = paths,
        .options = build_options(appRoot, paths.root, targetPath, hint, auto_render),
    };
    return binding;
}

auto CreateTextAreaBinding(PathSpace& space,
                           AppRootPathView appRoot,
                           TextAreaPaths const& paths,
                           ConcretePathView targetPath,
                           DirtyRectHint footprint,
                           std::optional<DirtyRectHint> dirty_override,
                           bool auto_render) -> SP::Expected<TextAreaBinding> {
    auto style = read_text_area_style(space, paths);
    if (!style) {
        return std::unexpected(style.error());
    }

    DirtyRectHint hint = ensure_valid_hint(dirty_override.value_or(text_input_dirty_hint(*style)));
    if (auto status = write_widget_footprint(space, paths.root, hint); !status) {
        return std::unexpected(status.error());
    }

    TextAreaBinding binding{
        .widget = paths,
        .options = build_options(appRoot, paths.root, targetPath, hint, auto_render),
    };
    return binding;
}

auto DispatchButton(PathSpace& space,
                    ButtonBinding const& binding,
                    ButtonState const& new_state,
                    WidgetOpKind op_kind,
                    PointerInfo const& pointer) -> SP::Expected<bool> {
    switch (op_kind) {
    case WidgetOpKind::HoverEnter:
    case WidgetOpKind::HoverExit:
    case WidgetOpKind::Press:
    case WidgetOpKind::Release:
    case WidgetOpKind::Activate:
        break;
    default:
        return std::unexpected(make_error("Unsupported widget op kind for button binding",
                                          SP::Error::Code::InvalidType));
    }

    auto changed = Widgets::UpdateButtonState(space, binding.widget, new_state);
    if (!changed) {
        return std::unexpected(changed.error());
    }

    if (*changed) {
        if (auto status = submit_dirty_hint(space, binding.options); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = schedule_auto_render(space, binding.options, "widget/button"); !status) {
            return std::unexpected(status.error());
        }
    }

    float value = new_state.pressed ? 1.0f : 0.0f;
    if (auto status = enqueue_widget_op(space,
                                        binding.options,
                                        binding.widget.root.getPath(),
                                        op_kind,
                                        pointer,
                                        value);
        !status) {
        return std::unexpected(status.error());
    }
    if (op_kind == WidgetOpKind::Press || op_kind == WidgetOpKind::Activate) {
        if (auto handler = invoke_handler_if_present(space, binding.widget.root, "press"); !handler) {
            return std::unexpected(handler.error());
        }
    }
    bool focus_changed = false;
    if (op_kind == WidgetOpKind::Press || op_kind == WidgetOpKind::Activate) {
        auto focus = set_widget_focus(space, binding.options, binding.widget.root);
        if (!focus) {
            return std::unexpected(focus.error());
        }
        focus_changed = *focus;
    }
    return *changed || focus_changed;
}

auto DispatchToggle(PathSpace& space,
                    ToggleBinding const& binding,
                    ToggleState const& new_state,
                    WidgetOpKind op_kind,
                    PointerInfo const& pointer) -> SP::Expected<bool> {
    switch (op_kind) {
    case WidgetOpKind::HoverEnter:
    case WidgetOpKind::HoverExit:
    case WidgetOpKind::Press:
    case WidgetOpKind::Release:
    case WidgetOpKind::Toggle:
        break;
    default:
        return std::unexpected(make_error("Unsupported widget op kind for toggle binding",
                                          SP::Error::Code::InvalidType));
    }

    auto changed = Widgets::UpdateToggleState(space, binding.widget, new_state);
    if (!changed) {
        return std::unexpected(changed.error());
    }

    if (*changed) {
        if (auto status = submit_dirty_hint(space, binding.options); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = schedule_auto_render(space, binding.options, "widget/toggle"); !status) {
            return std::unexpected(status.error());
        }
    }

    float value = new_state.checked ? 1.0f : 0.0f;
    if (auto status = enqueue_widget_op(space,
                                        binding.options,
                                        binding.widget.root.getPath(),
                                        op_kind,
                                        pointer,
                                        value);
        !status) {
        return std::unexpected(status.error());
    }
    if (op_kind == WidgetOpKind::Toggle) {
        if (auto handler = invoke_handler_if_present(space, binding.widget.root, "toggle"); !handler) {
            return std::unexpected(handler.error());
        }
    }
    bool focus_changed = false;
    if (op_kind == WidgetOpKind::Press || op_kind == WidgetOpKind::Toggle) {
        auto focus = set_widget_focus(space, binding.options, binding.widget.root);
        if (!focus) {
            return std::unexpected(focus.error());
        }
        focus_changed = *focus;
    }
    return *changed || focus_changed;
}

auto DispatchSlider(PathSpace& space,
                    SliderBinding const& binding,
                    SliderState const& new_state,
                    WidgetOpKind op_kind,
                    PointerInfo const& pointer) -> SP::Expected<bool> {
    bool enqueue_op = false;
    switch (op_kind) {
    case WidgetOpKind::HoverEnter:
    case WidgetOpKind::HoverExit:
        enqueue_op = false;
        break;
    case WidgetOpKind::SliderBegin:
    case WidgetOpKind::SliderUpdate:
    case WidgetOpKind::SliderCommit:
        enqueue_op = true;
        break;
    default:
        return std::unexpected(make_error("Unsupported widget op kind for slider binding",
                                          SP::Error::Code::InvalidType));
    }

    auto changed = Widgets::UpdateSliderState(space, binding.widget, new_state);
    if (!changed) {
        return std::unexpected(changed.error());
    }

    auto current_state = space.read<SliderState, std::string>(binding.widget.state.getPath());
    if (!current_state) {
        return std::unexpected(current_state.error());
    }

    if (*changed) {
        if (auto status = submit_dirty_hint(space, binding.options); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = schedule_auto_render(space, binding.options, "widget/slider"); !status) {
            return std::unexpected(status.error());
        }
    }

    if (enqueue_op) {
        if (auto status = enqueue_widget_op(space,
                                            binding.options,
                                            binding.widget.root.getPath(),
                                            op_kind,
                                            pointer,
                                            current_state->value);
            !status) {
            return std::unexpected(status.error());
        }
        if (op_kind == WidgetOpKind::SliderCommit) {
            HandlerInvocationInfo handler_info{
                .value = current_state->value,
            };
            if (auto handler = invoke_handler_if_present(space,
                                                         binding.widget.root,
                                                         "change",
                                                         handler_info);
                !handler) {
                return std::unexpected(handler.error());
            }
        }
    }
    bool focus_changed = false;
    if (op_kind == WidgetOpKind::SliderBegin || op_kind == WidgetOpKind::SliderCommit) {
        auto focus = set_widget_focus(space, binding.options, binding.widget.root);
        if (!focus) {
            return std::unexpected(focus.error());
        }
        focus_changed = *focus;
    }
    return *changed || focus_changed;
}

auto DispatchList(PathSpace& space,
                  ListBinding const& binding,
                  ListState const& new_state,
                  WidgetOpKind op_kind,
                  PointerInfo const& pointer,
                  std::int32_t item_index,
                  float scroll_delta) -> SP::Expected<bool> {
    switch (op_kind) {
    case WidgetOpKind::ListHover:
    case WidgetOpKind::ListSelect:
    case WidgetOpKind::ListActivate:
    case WidgetOpKind::ListScroll:
        break;
    default:
        return std::unexpected(make_error("Unsupported widget op kind for list binding",
                                          SP::Error::Code::InvalidType));
    }

    auto current_state = space.read<ListState, std::string>(binding.widget.state.getPath());
    if (!current_state) {
        return std::unexpected(current_state.error());
    }

    Widgets::ListState desired = new_state;
    switch (op_kind) {
    case WidgetOpKind::ListHover:
        desired.hovered_index = item_index;
        break;
    case WidgetOpKind::ListSelect:
    case WidgetOpKind::ListActivate:
        if (item_index >= 0) {
            desired.selected_index = item_index;
        }
        break;
    case WidgetOpKind::ListScroll:
        desired.scroll_offset = current_state->scroll_offset + scroll_delta;
        break;
    default:
        break;
    }

    auto changed = Widgets::UpdateListState(space, binding.widget, desired);
    if (!changed) {
        return std::unexpected(changed.error());
    }

    auto updated_state = space.read<ListState, std::string>(binding.widget.state.getPath());
    if (!updated_state) {
        return std::unexpected(updated_state.error());
    }

    std::optional<std::string> selected_id;
    if (updated_state->selected_index >= 0) {
        auto items = read_list_items(space, binding.widget);
        if (!items) {
            return std::unexpected(items.error());
        }
        auto idx = static_cast<std::size_t>(updated_state->selected_index);
        if (idx < items->size()) {
            selected_id = (*items)[idx].id;
        }
    }

    if (*changed) {
        if (auto status = submit_dirty_hint(space, binding.options); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = schedule_auto_render(space,
                                               binding.options,
                                               "widget/list"); !status) {
            return std::unexpected(status.error());
        }
    }

    float op_value = 0.0f;
    switch (op_kind) {
    case WidgetOpKind::ListHover:
        op_value = static_cast<float>(updated_state->hovered_index);
        break;
    case WidgetOpKind::ListSelect:
    case WidgetOpKind::ListActivate:
        op_value = static_cast<float>(updated_state->selected_index);
        break;
    case WidgetOpKind::ListScroll:
        op_value = updated_state->scroll_offset;
        break;
    default:
        break;
    }

    if (auto status = enqueue_widget_op(space,
                                        binding.options,
                                        binding.widget.root.getPath(),
                                        op_kind,
                                        pointer,
                                        op_value);
        !status) {
        return std::unexpected(status.error());
    }
    if (op_kind == WidgetOpKind::ListSelect || op_kind == WidgetOpKind::ListActivate) {
        HandlerInvocationInfo handler_info{};
        if (selected_id) {
            handler_info.target_id = *selected_id;
        }
        if (auto handler = invoke_handler_if_present(space,
                                                     binding.widget.root,
                                                     "child_event",
                                                     handler_info);
            !handler) {
            return std::unexpected(handler.error());
        }
    }
    bool focus_changed = false;
    if (op_kind == WidgetOpKind::ListSelect || op_kind == WidgetOpKind::ListActivate) {
        auto focus = set_widget_focus(space, binding.options, binding.widget.root);
        if (!focus) {
            return std::unexpected(focus.error());
        }
        focus_changed = *focus;
    }
    return *changed || focus_changed;
}

auto DispatchTree(PathSpace& space,
                  TreeBinding const& binding,
                  TreeState const& new_state,
                  WidgetOpKind op_kind,
                  std::string_view node_id,
                  PointerInfo const& pointer,
                  float scroll_delta) -> SP::Expected<bool> {
    switch (op_kind) {
    case WidgetOpKind::TreeHover:
    case WidgetOpKind::TreeSelect:
    case WidgetOpKind::TreeToggle:
    case WidgetOpKind::TreeExpand:
    case WidgetOpKind::TreeCollapse:
    case WidgetOpKind::TreeRequestLoad:
    case WidgetOpKind::TreeScroll:
        break;
    default:
        return std::unexpected(make_error("Unsupported widget op kind for tree binding",
                                          SP::Error::Code::InvalidType));
    }

    auto current_state = space.read<TreeState, std::string>(binding.widget.state.getPath());
    if (!current_state) {
        return std::unexpected(current_state.error());
    }

    auto nodes = read_tree_nodes(space, binding.widget);
    if (!nodes) {
        return std::unexpected(nodes.error());
    }

    auto [index, children, roots] = build_tree_children(*nodes);
    (void)roots;

    auto find_node = [&](std::string const& id) -> std::optional<std::size_t> {
        auto it = index.find(id);
        if (it == index.end()) {
            return std::nullopt;
        }
        return it->second;
    };

    TreeState desired = *current_state;
    std::string node_key = node_id.empty() ? std::string{} : std::string{node_id};

    auto ensure_node = [&]() -> std::optional<std::size_t> {
        if (node_key.empty()) {
            return std::nullopt;
        }
        return find_node(node_key);
    };

    bool should_request_load = false;

    switch (op_kind) {
    case WidgetOpKind::TreeHover: {
        if (node_key.empty()) {
            desired.hovered_id.clear();
        } else if (auto node_index = ensure_node()) {
            if ((*nodes)[*node_index].enabled) {
                desired.hovered_id = node_key;
            }
        }
        break;
    }
    case WidgetOpKind::TreeSelect: {
        if (!node_key.empty()) {
            if (auto node_index = ensure_node()) {
                if ((*nodes)[*node_index].enabled) {
                    desired.selected_id = node_key;
                    desired.hovered_id = node_key;
                }
            }
        }
        break;
    }
    case WidgetOpKind::TreeToggle:
    case WidgetOpKind::TreeExpand:
    case WidgetOpKind::TreeCollapse:
    case WidgetOpKind::TreeRequestLoad: {
        if (node_key.empty()) {
            return std::unexpected(make_error("tree operation requires node id",
                                              SP::Error::Code::InvalidPath));
        }
        auto node_index = ensure_node();
        if (!node_index) {
            return std::unexpected(make_error("unknown tree node id",
                                              SP::Error::Code::InvalidPath));
        }
        bool has_published_children = (*node_index < children.size()) && !children[*node_index].empty();
        bool expandable = has_published_children || (*nodes)[*node_index].expandable;
        if (!expandable) {
            break;
        }

        auto toggle_expansion = [&](bool expand) {
            auto& expanded = desired.expanded_ids;
            auto it = std::find(expanded.begin(), expanded.end(), node_key);
            if (expand) {
                if (it == expanded.end()) {
                    expanded.push_back(node_key);
                    if (!has_published_children && (*nodes)[*node_index].expandable) {
                        should_request_load = true;
                        if (std::find(desired.loading_ids.begin(), desired.loading_ids.end(), node_key)
                            == desired.loading_ids.end()) {
                            desired.loading_ids.push_back(node_key);
                        }
                    }
                }
            } else {
                if (it != expanded.end()) {
                    expanded.erase(it);
                }
                desired.loading_ids.erase(std::remove(desired.loading_ids.begin(),
                                                     desired.loading_ids.end(),
                                                     node_key),
                                          desired.loading_ids.end());
            }
        };

        if (op_kind == WidgetOpKind::TreeToggle) {
            bool is_expanded = std::find(desired.expanded_ids.begin(), desired.expanded_ids.end(), node_key)
                               != desired.expanded_ids.end();
            toggle_expansion(!is_expanded);
        } else if (op_kind == WidgetOpKind::TreeExpand) {
            toggle_expansion(true);
        } else if (op_kind == WidgetOpKind::TreeCollapse) {
            toggle_expansion(false);
        } else if (op_kind == WidgetOpKind::TreeRequestLoad) {
            if (std::find(desired.loading_ids.begin(), desired.loading_ids.end(), node_key)
                == desired.loading_ids.end()) {
                desired.loading_ids.push_back(node_key);
            }
        }
        break;
    }
    case WidgetOpKind::TreeScroll: {
        desired.scroll_offset = current_state->scroll_offset + scroll_delta;
        break;
    }
    default:
        break;
    }

    auto changed = Widgets::UpdateTreeState(space, binding.widget, desired);
    if (!changed) {
        return std::unexpected(changed.error());
    }

    auto updated_state = space.read<TreeState, std::string>(binding.widget.state.getPath());
    if (!updated_state) {
        return std::unexpected(updated_state.error());
    }

    if (*changed) {
        if (auto status = submit_dirty_hint(space, binding.options); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = schedule_auto_render(space, binding.options, "widget/tree"); !status) {
            return std::unexpected(status.error());
        }
    }

    float op_value = 0.0f;
    if (op_kind == WidgetOpKind::TreeScroll) {
        op_value = updated_state->scroll_offset;
    }

    if (auto status = enqueue_widget_op(space,
                                        binding.options,
                                        binding.widget.root.getPath(),
                                        op_kind,
                                        pointer,
                                        op_value,
                                        node_key);
        !status) {
        return std::unexpected(status.error());
    }
    switch (op_kind) {
    case WidgetOpKind::TreeHover:
    case WidgetOpKind::TreeScroll:
        break;
    default:
        HandlerInvocationInfo handler_info{};
        if (!node_key.empty()) {
            handler_info.target_id = node_key;
        }
        if (auto handler = invoke_handler_if_present(space,
                                                     binding.widget.root,
                                                     "node_event",
                                                     handler_info);
            !handler) {
            return std::unexpected(handler.error());
        }
        break;
    }

    if (should_request_load) {
        if (auto status = enqueue_widget_op(space,
                                            binding.options,
                                            binding.widget.root.getPath(),
                                            WidgetOpKind::TreeRequestLoad,
                                            pointer,
                                            0.0f,
                                            node_key);
            !status) {
            return std::unexpected(status.error());
        }
        HandlerInvocationInfo handler_info{};
        handler_info.target_id = node_key;
        if (auto handler = invoke_handler_if_present(space,
                                                     binding.widget.root,
                                                     "node_event",
                                                     handler_info);
            !handler) {
            return std::unexpected(handler.error());
        }
    }

    bool focus_changed = false;
    if (op_kind == WidgetOpKind::TreeSelect || op_kind == WidgetOpKind::TreeToggle) {
        auto focus = set_widget_focus(space, binding.options, binding.widget.root);
        if (!focus) {
            return std::unexpected(focus.error());
        }
        focus_changed = *focus;
    }
    return *changed || focus_changed;
}

auto DispatchTextField(PathSpace& space,
                       TextFieldBinding const& binding,
                       TextFieldState const& new_state,
                       WidgetOpKind op_kind,
                       PointerInfo const& pointer,
                       float op_value) -> SP::Expected<bool> {
    switch (op_kind) {
    case WidgetOpKind::HoverEnter:
    case WidgetOpKind::HoverExit:
    case WidgetOpKind::TextHover:
    case WidgetOpKind::TextFocus:
    case WidgetOpKind::TextInput:
    case WidgetOpKind::TextDelete:
    case WidgetOpKind::TextMoveCursor:
    case WidgetOpKind::TextSetSelection:
    case WidgetOpKind::TextCompositionStart:
    case WidgetOpKind::TextCompositionUpdate:
    case WidgetOpKind::TextCompositionCommit:
    case WidgetOpKind::TextCompositionCancel:
    case WidgetOpKind::TextClipboardCopy:
    case WidgetOpKind::TextClipboardCut:
    case WidgetOpKind::TextClipboardPaste:
    case WidgetOpKind::TextSubmit:
        break;
    default:
        return std::unexpected(make_error("Unsupported widget op kind for text field binding",
                                          SP::Error::Code::InvalidType));
    }

    auto state_value = space.read<TextFieldState, std::string>(binding.widget.state.getPath());
    if (!state_value) {
        return std::unexpected(state_value.error());
    }

    Widgets::TextFieldState desired = *state_value;
    bool can_edit_text = desired.enabled && !desired.read_only;

    switch (op_kind) {
    case WidgetOpKind::HoverEnter:
        set_flag(desired.hovered, true);
        break;
    case WidgetOpKind::HoverExit:
        set_flag(desired.hovered, false);
        break;
    case WidgetOpKind::TextHover:
        set_flag(desired.hovered, pointer.inside);
        break;
    case WidgetOpKind::TextFocus:
        set_flag(desired.focused, true);
        if (pointer.inside) {
            set_flag(desired.hovered, true);
        }
        break;
    case WidgetOpKind::TextInput:
        if (can_edit_text && std::isfinite(op_value)) {
            auto codepoint = static_cast<char32_t>(std::llround(op_value));
            auto utf8 = encode_utf8(codepoint);
            insert_text(desired, utf8, false);
        }
        break;
    case WidgetOpKind::TextDelete:
        if (can_edit_text) {
            bool forward = op_value >= 0.0f;
            delete_single(desired, forward);
        }
        break;
    case WidgetOpKind::TextMoveCursor:
        move_cursor(desired, static_cast<int>(std::llround(op_value)));
        break;
    case WidgetOpKind::TextSetSelection:
        apply_selection_payload(desired, new_state);
        break;
    case WidgetOpKind::TextCompositionStart:
        if (can_edit_text) {
            begin_composition(desired);
        }
        break;
    case WidgetOpKind::TextCompositionUpdate:
        if (can_edit_text) {
            update_composition(desired,
                               new_state.composition_text,
                               new_state.composition_start,
                               new_state.composition_end,
                               false);
        }
        break;
    case WidgetOpKind::TextCompositionCommit:
        if (can_edit_text) {
            commit_composition(desired, false);
        }
        break;
    case WidgetOpKind::TextCompositionCancel:
        cancel_composition(desired);
        break;
    case WidgetOpKind::TextClipboardCopy: {
        auto status = copy_selection(space, binding.widget.root, desired);
        if (!status) {
            return std::unexpected(status.error());
        }
        break;
    }
    case WidgetOpKind::TextClipboardCut: {
        if (can_edit_text) {
            auto status = copy_selection(space, binding.widget.root, desired);
            if (!status) {
                return std::unexpected(status.error());
            }
            erase_selection(desired);
        }
        break;
    }
    case WidgetOpKind::TextClipboardPaste: {
        if (can_edit_text) {
            std::string pasted = new_state.composition_text;
            if (pasted.empty()) {
                auto stored = read_clipboard_text(space, binding.widget.root);
                if (!stored) {
                    return std::unexpected(stored.error());
                }
                pasted = *stored;
            }
            insert_text(desired, pasted, false);
        }
        break;
    }
    case WidgetOpKind::TextSubmit:
        set_flag(desired.submit_pending, true);
        break;
    default:
        break;
    }

    auto changed = Widgets::UpdateTextFieldState(space, binding.widget, desired);
    if (!changed) {
        return std::unexpected(changed.error());
    }

    if (*changed) {
        if (auto status = submit_dirty_hint(space, binding.options); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = schedule_auto_render(space, binding.options, "widget/text_field"); !status) {
            return std::unexpected(status.error());
        }
    }

    bool requires_focus = !(op_kind == WidgetOpKind::HoverEnter
                            || op_kind == WidgetOpKind::HoverExit
                            || op_kind == WidgetOpKind::TextHover);
    bool focus_changed = false;
    if (requires_focus) {
        auto focus = set_widget_focus(space, binding.options, binding.widget.root);
        if (!focus) {
            return std::unexpected(focus.error());
        }
        focus_changed = *focus;
    }

    float event_value = 0.0f;
    switch (op_kind) {
    case WidgetOpKind::TextInput:
        event_value = op_value;
        break;
    case WidgetOpKind::TextDelete:
        event_value = (op_value >= 0.0f) ? 1.0f : -1.0f;
        break;
    case WidgetOpKind::TextMoveCursor:
        event_value = op_value;
        break;
    case WidgetOpKind::TextSubmit:
        event_value = 1.0f;
        break;
    default:
        break;
    }

    if (requires_focus) {
        if (auto status = enqueue_widget_op(space,
                                            binding.options,
                                            binding.widget.root.getPath(),
                                            op_kind,
                                            pointer,
                                            event_value);
            !status) {
            return std::unexpected(status.error());
        }
        std::string_view event_name = (op_kind == WidgetOpKind::TextSubmit) ? "submit" : "change";
        if (!event_name.empty()) {
            if (auto handler = invoke_handler_if_present(space, binding.widget.root, event_name); !handler) {
                return std::unexpected(handler.error());
            }
        }
    }

    return *changed || focus_changed;
}

auto DispatchTextArea(PathSpace& space,
                      TextAreaBinding const& binding,
                      TextAreaState const& new_state,
                      WidgetOpKind op_kind,
                      PointerInfo const& pointer,
                      float scroll_delta_y,
                      float op_value) -> SP::Expected<bool> {
    switch (op_kind) {
    case WidgetOpKind::HoverEnter:
    case WidgetOpKind::HoverExit:
    case WidgetOpKind::TextHover:
    case WidgetOpKind::TextFocus:
    case WidgetOpKind::TextInput:
    case WidgetOpKind::TextDelete:
    case WidgetOpKind::TextMoveCursor:
    case WidgetOpKind::TextSetSelection:
    case WidgetOpKind::TextCompositionStart:
    case WidgetOpKind::TextCompositionUpdate:
    case WidgetOpKind::TextCompositionCommit:
    case WidgetOpKind::TextCompositionCancel:
    case WidgetOpKind::TextClipboardCopy:
    case WidgetOpKind::TextClipboardCut:
    case WidgetOpKind::TextClipboardPaste:
    case WidgetOpKind::TextSubmit:
    case WidgetOpKind::TextScroll:
        break;
    default:
        return std::unexpected(make_error("Unsupported widget op kind for text area binding",
                                          SP::Error::Code::InvalidType));
    }

    auto state_value = space.read<TextAreaState, std::string>(binding.widget.state.getPath());
    if (!state_value) {
        return std::unexpected(state_value.error());
    }

    Widgets::TextAreaState desired = *state_value;
    bool can_edit_text = desired.enabled && !desired.read_only;

    switch (op_kind) {
    case WidgetOpKind::HoverEnter:
        set_flag(desired.hovered, true);
        break;
    case WidgetOpKind::HoverExit:
        set_flag(desired.hovered, false);
        break;
    case WidgetOpKind::TextHover:
        set_flag(desired.hovered, pointer.inside);
        break;
    case WidgetOpKind::TextFocus:
        set_flag(desired.focused, true);
        if (pointer.inside) {
            set_flag(desired.hovered, true);
        }
        break;
    case WidgetOpKind::TextInput:
        if (can_edit_text && std::isfinite(op_value)) {
            auto codepoint = static_cast<char32_t>(std::llround(op_value));
            auto utf8 = encode_utf8(codepoint);
            insert_text(desired, utf8, true);
        }
        break;
    case WidgetOpKind::TextDelete:
        if (can_edit_text) {
            bool forward = op_value >= 0.0f;
            delete_single(desired, forward);
        }
        break;
    case WidgetOpKind::TextMoveCursor:
        move_cursor(desired, static_cast<int>(std::llround(op_value)));
        break;
    case WidgetOpKind::TextSetSelection:
        apply_selection_payload(desired, new_state);
        break;
    case WidgetOpKind::TextCompositionStart:
        if (can_edit_text) {
            begin_composition(desired);
        }
        break;
    case WidgetOpKind::TextCompositionUpdate:
        if (can_edit_text) {
            update_composition(desired,
                               new_state.composition_text,
                               new_state.composition_start,
                               new_state.composition_end,
                               true);
        }
        break;
    case WidgetOpKind::TextCompositionCommit:
        if (can_edit_text) {
            commit_composition(desired, true);
        }
        break;
    case WidgetOpKind::TextCompositionCancel:
        cancel_composition(desired);
        break;
    case WidgetOpKind::TextClipboardCopy: {
        auto status = copy_selection(space, binding.widget.root, desired);
        if (!status) {
            return std::unexpected(status.error());
        }
        break;
    }
    case WidgetOpKind::TextClipboardCut: {
        if (can_edit_text) {
            auto status = copy_selection(space, binding.widget.root, desired);
            if (!status) {
                return std::unexpected(status.error());
            }
            erase_selection(desired);
        }
        break;
    }
    case WidgetOpKind::TextClipboardPaste: {
        if (can_edit_text) {
            std::string pasted = new_state.composition_text;
            if (pasted.empty()) {
                auto stored = read_clipboard_text(space, binding.widget.root);
                if (!stored) {
                    return std::unexpected(stored.error());
                }
                pasted = *stored;
            }
            insert_text(desired, pasted, true);
        }
        break;
    }
    case WidgetOpKind::TextSubmit:
        set_flag(desired.submit_pending, true);
        break;
    case WidgetOpKind::TextScroll:
        if (std::isfinite(scroll_delta_y) && scroll_delta_y != 0.0f) {
            desired.scroll_y = std::max(0.0f, desired.scroll_y + scroll_delta_y);
        }
        break;
    default:
        break;
    }

    auto changed = Widgets::UpdateTextAreaState(space, binding.widget, desired);
    if (!changed) {
        return std::unexpected(changed.error());
    }

    if (*changed) {
        if (auto status = submit_dirty_hint(space, binding.options); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = schedule_auto_render(space, binding.options, "widget/text_area"); !status) {
            return std::unexpected(status.error());
        }
    }

    bool requires_focus = !(op_kind == WidgetOpKind::HoverEnter
                            || op_kind == WidgetOpKind::HoverExit
                            || op_kind == WidgetOpKind::TextHover);
    bool focus_changed = false;
    if (requires_focus) {
        auto focus = set_widget_focus(space, binding.options, binding.widget.root);
        if (!focus) {
            return std::unexpected(focus.error());
        }
        focus_changed = *focus;
    }

    float event_value = 0.0f;
    switch (op_kind) {
    case WidgetOpKind::TextInput:
        event_value = op_value;
        break;
    case WidgetOpKind::TextDelete:
        event_value = (op_value >= 0.0f) ? 1.0f : -1.0f;
        break;
    case WidgetOpKind::TextMoveCursor:
        event_value = op_value;
        break;
    case WidgetOpKind::TextSubmit:
        event_value = 1.0f;
        break;
    case WidgetOpKind::TextScroll:
        event_value = desired.scroll_y;
        break;
    default:
        break;
    }

    if (requires_focus) {
        if (auto status = enqueue_widget_op(space,
                                            binding.options,
                                            binding.widget.root.getPath(),
                                            op_kind,
                                            pointer,
                                            event_value);
            !status) {
            return std::unexpected(status.error());
        }
        std::string_view event_name = (op_kind == WidgetOpKind::TextSubmit) ? "submit" : "change";
        if (!event_name.empty()) {
            if (auto handler = invoke_handler_if_present(space, binding.widget.root, event_name); !handler) {
                return std::unexpected(handler.error());
            }
        }
    }

    return *changed || focus_changed;
}

auto UpdateStack(PathSpace& space,
                 StackBinding const& binding,
                 StackLayoutParams const& params) -> SP::Expected<bool> {
    auto changed = Widgets::UpdateStackLayout(space, binding.layout, params);
    if (!changed) {
        return std::unexpected(changed.error());
    }
    if (!*changed) {
        return false;
    }

    auto layout = Widgets::ReadStackLayout(space, binding.layout);
    if (!layout) {
        return std::unexpected(layout.error());
    }

    DirtyRectHint updated_hint = binding.options.dirty_rect;
    auto layout_hint = make_default_dirty_rect(layout->width, layout->height);
    if (updated_hint.max_x <= updated_hint.min_x || updated_hint.max_y <= updated_hint.min_y) {
        updated_hint = layout_hint;
    } else {
        updated_hint.min_x = std::min(updated_hint.min_x, layout_hint.min_x);
        updated_hint.min_y = std::min(updated_hint.min_y, layout_hint.min_y);
        updated_hint.max_x = std::max(updated_hint.max_x, layout_hint.max_x);
        updated_hint.max_y = std::max(updated_hint.max_y, layout_hint.max_y);
    }
    updated_hint = ensure_valid_hint(updated_hint);
    if (auto status = write_widget_footprint(space, binding.layout.root, updated_hint); !status) {
        return std::unexpected(status.error());
    }

    BindingOptions refreshed = binding.options;
    refreshed.dirty_rect = updated_hint;
    if (auto status = submit_dirty_hint(space, refreshed); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = schedule_auto_render(space, refreshed, "widget/stack"); !status) {
        return std::unexpected(status.error());
    }
    return true;
}

} // namespace SP::UI::Runtime::Widgets::Bindings
