#pragma once

#include <pathspace/ui/WidgetSharedTypes.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace SP::UI::Declarative {

struct WidgetMailboxEvent {
    std::string topic;
    SP::UI::Runtime::Widgets::Bindings::WidgetOpKind kind =
        SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::HoverEnter;
    std::string widget_path;
    std::string target_id;
    SP::UI::Runtime::Widgets::Bindings::PointerInfo pointer;
    float value = 0.0f;
    std::uint64_t sequence = 0;
    std::uint64_t timestamp_ns = 0;
};

namespace Mailbox {

[[nodiscard]] inline auto TopicFor(
    SP::UI::Runtime::Widgets::Bindings::WidgetOpKind kind) -> std::string_view {
    using SP::UI::Runtime::Widgets::Bindings::WidgetOpKind;
    switch (kind) {
    case WidgetOpKind::HoverEnter:
        return "hover_enter";
    case WidgetOpKind::HoverExit:
        return "hover_exit";
    case WidgetOpKind::Press:
        return "press";
    case WidgetOpKind::Release:
        return "release";
    case WidgetOpKind::Activate:
        return "activate";
    case WidgetOpKind::Toggle:
        return "toggle";
    case WidgetOpKind::SliderBegin:
        return "slider_begin";
    case WidgetOpKind::SliderUpdate:
        return "slider_update";
    case WidgetOpKind::SliderCommit:
        return "slider_commit";
    case WidgetOpKind::ListHover:
        return "list_hover";
    case WidgetOpKind::ListSelect:
        return "list_select";
    case WidgetOpKind::ListActivate:
        return "list_activate";
    case WidgetOpKind::ListScroll:
        return "list_scroll";
    case WidgetOpKind::TreeHover:
        return "tree_hover";
    case WidgetOpKind::TreeSelect:
        return "tree_select";
    case WidgetOpKind::TreeToggle:
        return "tree_toggle";
    case WidgetOpKind::TreeExpand:
        return "tree_expand";
    case WidgetOpKind::TreeCollapse:
        return "tree_collapse";
    case WidgetOpKind::TreeRequestLoad:
        return "tree_request_load";
    case WidgetOpKind::TreeScroll:
        return "tree_scroll";
    case WidgetOpKind::TextHover:
        return "text_hover";
    case WidgetOpKind::TextFocus:
        return "text_focus";
    case WidgetOpKind::TextInput:
        return "text_input";
    case WidgetOpKind::TextDelete:
        return "text_delete";
    case WidgetOpKind::TextMoveCursor:
        return "text_move_cursor";
    case WidgetOpKind::TextSetSelection:
        return "text_set_selection";
    case WidgetOpKind::TextCompositionStart:
        return "text_composition_start";
    case WidgetOpKind::TextCompositionUpdate:
        return "text_composition_update";
    case WidgetOpKind::TextCompositionCommit:
        return "text_composition_commit";
    case WidgetOpKind::TextCompositionCancel:
        return "text_composition_cancel";
    case WidgetOpKind::TextClipboardCopy:
        return "text_clipboard_copy";
    case WidgetOpKind::TextClipboardCut:
        return "text_clipboard_cut";
    case WidgetOpKind::TextClipboardPaste:
        return "text_clipboard_paste";
    case WidgetOpKind::TextScroll:
        return "text_scroll";
    case WidgetOpKind::TextSubmit:
        return "text_submit";
    case WidgetOpKind::StackSelect:
        return "stack_select";
    case WidgetOpKind::PaintStrokeBegin:
        return "paint_stroke_begin";
    case WidgetOpKind::PaintStrokeUpdate:
        return "paint_stroke_update";
    case WidgetOpKind::PaintStrokeCommit:
        return "paint_stroke_commit";
    default:
        return "";
    }
}

} // namespace Mailbox

} // namespace SP::UI::Declarative

