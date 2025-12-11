#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/path/ConcretePath.hpp>
#include <pathspace/ui/WidgetSharedTypes.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>

namespace Widgets = SP::UI::Runtime::Widgets;
namespace Bindings = SP::UI::Runtime::Widgets::Bindings;
namespace Runtime = SP::UI::Runtime;

namespace {

auto make_pointer(bool inside = true) -> Bindings::PointerInfo {
    auto pointer = Bindings::PointerInfo::Make(0.0f, 0.0f);
    pointer.inside = inside;
    pointer.primary = true;
    return pointer;
}

auto make_target_path(std::string const& name) -> SP::ConcretePathString {
    return SP::ConcretePathString{"/system/tests/targets/" + name};
}

auto read_field_state(SP::PathSpace& space,
                      Widgets::TextFieldPaths const& paths) -> Widgets::TextFieldState {
    auto state = space.read<Widgets::TextFieldState, std::string>(paths.state.getPath());
    REQUIRE(state);
    return *state;
}

auto read_area_state(SP::PathSpace& space,
                     Widgets::TextAreaPaths const& paths) -> Widgets::TextAreaState {
    auto state = space.read<Widgets::TextAreaState, std::string>(paths.state.getPath());
    REQUIRE(state);
    return *state;
}

auto make_text_field_binding(SP::PathSpace& space,
                             SP::App::AppRootPathView app_root,
                             std::string const& name)
    -> std::pair<Widgets::TextFieldPaths, Bindings::TextFieldBinding> {
    Widgets::TextFieldParams params{};
    params.name = name;
    auto paths = Widgets::CreateTextField(space, app_root, params);
    REQUIRE(paths);

    Runtime::DirtyRectHint footprint{0.0f, 0.0f, params.style.width, params.style.height};
    auto target = make_target_path(name);
    auto binding = Bindings::CreateTextFieldBinding(space,
                                                   app_root,
                                                   *paths,
                                                   SP::ConcretePathStringView{target.getPath()},
                                                   footprint,
                                                   Runtime::DirtyRectHint{},
                                                   false);
    REQUIRE(binding);
    return {*paths, *binding};
}

auto make_text_area_binding(SP::PathSpace& space,
                            SP::App::AppRootPathView app_root,
                            std::string const& name)
    -> std::pair<Widgets::TextAreaPaths, Bindings::TextAreaBinding> {
    Widgets::TextAreaParams params{};
    params.name = name;
    auto paths = Widgets::CreateTextArea(space, app_root, params);
    REQUIRE(paths);

    Runtime::DirtyRectHint footprint{0.0f, 0.0f, params.style.width, params.style.height};
    auto target = make_target_path(name + "_area");
    auto binding = Bindings::CreateTextAreaBinding(space,
                                                  app_root,
                                                  *paths,
                                                  SP::ConcretePathStringView{target.getPath()},
                                                  footprint,
                                                  Runtime::DirtyRectHint{},
                                                  false);
    REQUIRE(binding);
    return {*paths, *binding};
}

} // namespace

TEST_CASE("TextField handles typing, deletion, and cursor moves") {
    SP::PathSpace space;
    SP::App::AppRootPath app_root{"/system/applications/text_field_test"};
    auto app_view = SP::App::AppRootPathView{app_root.getPath()};
    auto [paths, binding] = make_text_field_binding(space, app_view, "field_core");

    auto pointer = make_pointer();

    auto dispatch = [&](Bindings::WidgetOpKind kind, float value = 0.0f) {
        auto state = read_field_state(space, paths);
        auto status = Bindings::DispatchTextField(space, binding, state, kind, pointer, value);
        REQUIRE(status);
    };

    dispatch(Bindings::WidgetOpKind::TextInput, static_cast<float>('H'));
    dispatch(Bindings::WidgetOpKind::TextInput, static_cast<float>('i'));

    auto state = read_field_state(space, paths);
    CHECK(state.text == "Hi");
    CHECK(state.cursor == 2);

    dispatch(Bindings::WidgetOpKind::TextDelete, -1.0f);
    state = read_field_state(space, paths);
    CHECK(state.text == "H");
    CHECK(state.cursor == 1);

    dispatch(Bindings::WidgetOpKind::TextMoveCursor, -1.0f);
    state = read_field_state(space, paths);
    CHECK(state.cursor == 0);
}

TEST_CASE("TextField clipboard and paste flows") {
    SP::PathSpace space;
    SP::App::AppRootPath app_root{"/system/applications/text_field_clipboard"};
    auto app_view = SP::App::AppRootPathView{app_root.getPath()};
    auto [paths, binding] = make_text_field_binding(space, app_view, "field_clip");

    auto pointer = make_pointer();

    auto dispatch = [&](Bindings::WidgetOpKind kind,
                        Widgets::TextFieldState const& payload,
                        float value = 0.0f) {
        auto status = Bindings::DispatchTextField(space, binding, payload, kind, pointer, value);
        REQUIRE(status);
    };

    auto seed_state = read_field_state(space, paths);
    dispatch(Bindings::WidgetOpKind::TextInput, seed_state, static_cast<float>('H'));
    dispatch(Bindings::WidgetOpKind::TextInput, read_field_state(space, paths), static_cast<float>('e'));
    dispatch(Bindings::WidgetOpKind::TextInput, read_field_state(space, paths), static_cast<float>('l'));
    dispatch(Bindings::WidgetOpKind::TextInput, read_field_state(space, paths), static_cast<float>('l'));
    dispatch(Bindings::WidgetOpKind::TextInput, read_field_state(space, paths), static_cast<float>('o'));

    auto payload = read_field_state(space, paths);
    payload.selection_start = 1;
    payload.selection_end = 4;
    dispatch(Bindings::WidgetOpKind::TextSetSelection, payload);

    dispatch(Bindings::WidgetOpKind::TextClipboardCopy, read_field_state(space, paths));

    auto clipboard = space.read<std::string, std::string>(
        Widgets::WidgetSpacePath(paths.root.getPath(), "/ops/clipboard/last_text"));
    REQUIRE(clipboard);
    CHECK(*clipboard == "ell");

    dispatch(Bindings::WidgetOpKind::TextClipboardCut, read_field_state(space, paths));
    auto state = read_field_state(space, paths);
    CHECK(state.text == "Ho");

    dispatch(Bindings::WidgetOpKind::TextClipboardPaste, read_field_state(space, paths));
    state = read_field_state(space, paths);
    CHECK(state.text == "Hello");

    auto paste_payload = read_field_state(space, paths);
    paste_payload.composition_text = "!";
    dispatch(Bindings::WidgetOpKind::TextClipboardPaste, paste_payload);
    state = read_field_state(space, paths);
    CHECK(state.text == "Hello!");
}

TEST_CASE("TextField composition commit") {
    SP::PathSpace space;
    SP::App::AppRootPath app_root{"/system/applications/text_field_composition"};
    auto app_view = SP::App::AppRootPathView{app_root.getPath()};
    auto [paths, binding] = make_text_field_binding(space, app_view, "field_compose");

    auto pointer = make_pointer();
    auto send = [&](Bindings::WidgetOpKind kind,
                    Widgets::TextFieldState const& payload,
                    float value = 0.0f) {
        auto status = Bindings::DispatchTextField(space, binding, payload, kind, pointer, value);
        REQUIRE(status);
    };

    send(Bindings::WidgetOpKind::TextInput,
         read_field_state(space, paths),
         static_cast<float>('A'));

    send(Bindings::WidgetOpKind::TextCompositionStart, read_field_state(space, paths));

    auto update_state = read_field_state(space, paths);
    update_state.composition_text = "é";
    update_state.composition_start = update_state.cursor;
    update_state.composition_end = update_state.cursor;
    send(Bindings::WidgetOpKind::TextCompositionUpdate, update_state);

    send(Bindings::WidgetOpKind::TextCompositionCommit, read_field_state(space, paths));
    auto final_state = read_field_state(space, paths);
    CHECK(final_state.text == "Aé");
    CHECK_FALSE(final_state.composition_active);
}

TEST_CASE("TextArea supports multiline input and scroll") {
    SP::PathSpace space;
    SP::App::AppRootPath app_root{"/system/applications/text_area_test"};
    auto app_view = SP::App::AppRootPathView{app_root.getPath()};
    auto [paths, binding] = make_text_area_binding(space, app_view, "area");

    auto pointer = make_pointer();

    auto dispatch = [&](Bindings::WidgetOpKind kind,
                        Widgets::TextAreaState const& payload,
                        float scroll_delta = 0.0f,
                        float value = 0.0f) {
        auto status = Bindings::DispatchTextArea(space, binding, payload, kind, pointer, scroll_delta, value);
        REQUIRE(status);
    };

    dispatch(Bindings::WidgetOpKind::TextInput,
             read_area_state(space, paths),
             0.0f,
             static_cast<float>('N'));
    dispatch(Bindings::WidgetOpKind::TextInput,
             read_area_state(space, paths),
             0.0f,
             static_cast<float>('\n'));
    dispatch(Bindings::WidgetOpKind::TextInput,
             read_area_state(space, paths),
             0.0f,
             static_cast<float>('L'));

    auto state = read_area_state(space, paths);
    CHECK(state.text == "N\nL");

    dispatch(Bindings::WidgetOpKind::TextScroll, state, 5.0f);
    state = read_area_state(space, paths);
    CHECK(state.scroll_y == doctest::Approx(5.0f));

    dispatch(Bindings::WidgetOpKind::TextDelete, state, 0.0f, -1.0f);
    state = read_area_state(space, paths);
    CHECK(state.text.back() == '\n');
}
