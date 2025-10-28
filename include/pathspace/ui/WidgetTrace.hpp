#pragma once

#include <pathspace/ui/LocalWindowBridge.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace SP::UI {

enum class WidgetTraceEventKind {
    MouseAbsolute,
    MouseRelative,
    MouseDown,
    MouseUp,
    MouseWheel,
    KeyDown,
    KeyUp,
};

struct WidgetTraceEvent {
    WidgetTraceEventKind kind = WidgetTraceEventKind::MouseAbsolute;
    double time_ms = 0.0;
    int x = -1;
    int y = -1;
    int dx = 0;
    int dy = 0;
    int wheel = 0;
    int button = 0;
    unsigned int keycode = 0;
    unsigned int modifiers = 0;
    char32_t character = U'\0';
    bool repeat = false;
};

struct WidgetTraceOptions {
    std::string record_env = "PATHSPACE_WIDGET_TRACE_RECORD";
    std::string replay_env = "PATHSPACE_WIDGET_TRACE_REPLAY";
    std::string log_prefix = "widget_trace";
};

class WidgetTrace {
public:
    explicit WidgetTrace(WidgetTraceOptions options = {});

    void init_from_env();
    void enable_recording(std::string path);
    void enable_replay(std::string path);

    void record_mouse(LocalMouseEvent const& event);
    void record_key(LocalKeyEvent const& event);
    void flush();

    [[nodiscard]] bool recording() const { return record_enabled_; }
    [[nodiscard]] bool replaying() const { return replay_enabled_; }
    [[nodiscard]] std::string const& record_path() const { return record_path_; }
    [[nodiscard]] std::string const& replay_path() const { return replay_path_; }
    [[nodiscard]] std::vector<WidgetTraceEvent> const& events() const { return replay_events_; }

private:
    WidgetTraceOptions options_{};
    bool record_enabled_ = false;
    bool replay_enabled_ = false;
    bool start_time_valid_ = false;
    std::string record_path_;
    std::string replay_path_;
    std::vector<WidgetTraceEvent> recorded_events_;
    std::vector<WidgetTraceEvent> replay_events_;
    std::chrono::steady_clock::time_point start_time_{};

    void ensure_start();
    void append_record(WidgetTraceEvent event);
    [[nodiscard]] static std::string_view kind_to_string(WidgetTraceEventKind kind);
    [[nodiscard]] static std::optional<WidgetTraceEventKind> string_to_kind(std::string_view value);
    [[nodiscard]] std::string format_event(WidgetTraceEvent const& event) const;
    [[nodiscard]] static std::optional<WidgetTraceEvent> parse_line(std::string const& line);
};

} // namespace SP::UI

