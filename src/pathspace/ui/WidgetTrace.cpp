#include <pathspace/ui/WidgetTrace.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <utility>

namespace SP::UI {

namespace {

auto parse_int(std::string_view value, int fallback = 0) -> int {
    try {
        std::size_t consumed = 0;
        int parsed = std::stoi(std::string(value), &consumed);
        if (consumed == value.size()) {
            return parsed;
        }
    } catch (...) {
    }
    return fallback;
}

auto parse_uint(std::string_view value, unsigned int fallback = 0U) -> unsigned int {
    try {
        std::size_t consumed = 0;
        unsigned long parsed = std::stoul(std::string(value), &consumed);
        if (consumed == value.size()) {
            return static_cast<unsigned int>(parsed);
        }
    } catch (...) {
    }
    return fallback;
}

auto parse_double(std::string_view value, double fallback = 0.0) -> double {
    try {
        std::size_t consumed = 0;
        double parsed = std::stod(std::string(value), &consumed);
        if (consumed == value.size()) {
            return parsed;
        }
    } catch (...) {
    }
    return fallback;
}

} // namespace

WidgetTrace::WidgetTrace(WidgetTraceOptions options)
    : options_(std::move(options)) {}

void WidgetTrace::init_from_env() {
    recorded_events_.clear();
    start_time_valid_ = false;
    record_enabled_ = false;

    if (replay_enabled_) {
        return;
    }

    if (auto const* replay = std::getenv(options_.replay_env.c_str())) {
        if (replay && replay[0] != '\0') {
            enable_replay(replay);
            return;
        }
    }

    if (auto const* record = std::getenv(options_.record_env.c_str())) {
        if (record && record[0] != '\0') {
            enable_recording(record);
        }
    }
}

void WidgetTrace::enable_recording(std::string path) {
    record_enabled_ = true;
    replay_enabled_ = false;
    record_path_ = std::move(path);
    replay_path_.clear();
    start_time_valid_ = false;
    recorded_events_.clear();
    replay_events_.clear();
}

void WidgetTrace::enable_replay(std::string path) {
    replay_enabled_ = true;
    record_enabled_ = false;
    replay_path_ = std::move(path);
    record_path_.clear();
    recorded_events_.clear();
    replay_events_.clear();

    namespace fs = std::filesystem;
    try {
        if (!fs::exists(replay_path_)) {
            std::cerr << options_.log_prefix << ": replay trace '" << replay_path_
                      << "' does not exist\n";
            replay_events_.clear();
            return;
        }
        std::ifstream input(replay_path_);
        if (!input) {
            std::cerr << options_.log_prefix << ": failed to open trace '" << replay_path_
                      << "' for replay\n";
            return;
        }
        std::string line;
        while (std::getline(input, line)) {
            if (auto parsed = parse_line(line)) {
                replay_events_.push_back(*parsed);
            }
        }
        if (replay_events_.empty()) {
            std::cerr << options_.log_prefix << ": replay trace '" << replay_path_
                      << "' contained no events\n";
        }
    } catch (std::exception const& ex) {
        std::cerr << options_.log_prefix << ": failed to load trace '" << replay_path_
                  << "': " << ex.what() << "\n";
    }
}

void WidgetTrace::record_mouse(LocalMouseEvent const& event) {
    if (!record_enabled_) {
        return;
    }
    WidgetTraceEvent trace_event{};
    switch (event.type) {
    case LocalMouseEventType::AbsoluteMove:
        trace_event.kind = WidgetTraceEventKind::MouseAbsolute;
        break;
    case LocalMouseEventType::Move:
        trace_event.kind = WidgetTraceEventKind::MouseRelative;
        break;
    case LocalMouseEventType::ButtonDown:
        trace_event.kind = WidgetTraceEventKind::MouseDown;
        break;
    case LocalMouseEventType::ButtonUp:
        trace_event.kind = WidgetTraceEventKind::MouseUp;
        break;
    case LocalMouseEventType::Wheel:
        trace_event.kind = WidgetTraceEventKind::MouseWheel;
        break;
    }
    trace_event.x = event.x;
    trace_event.y = event.y;
    trace_event.dx = event.dx;
    trace_event.dy = event.dy;
    trace_event.wheel = event.wheel;
    trace_event.button = static_cast<int>(event.button);
    append_record(trace_event);
}

void WidgetTrace::record_key(LocalKeyEvent const& event) {
    if (!record_enabled_) {
        return;
    }
    WidgetTraceEvent trace_event{};
    trace_event.kind = (event.type == LocalKeyEventType::KeyDown)
        ? WidgetTraceEventKind::KeyDown
        : WidgetTraceEventKind::KeyUp;
    trace_event.keycode = event.keycode;
    trace_event.modifiers = event.modifiers;
    trace_event.repeat = event.repeat;
    trace_event.character = event.character;
    append_record(trace_event);
}

void WidgetTrace::flush() {
    if (!record_enabled_) {
        return;
    }
    namespace fs = std::filesystem;
    try {
        fs::path path(record_path_);
        if (!path.parent_path().empty()) {
            fs::create_directories(path.parent_path());
        }
        std::ofstream output(record_path_);
        if (!output) {
            std::cerr << options_.log_prefix << ": failed to open trace output '"
                      << record_path_ << "'\n";
            return;
        }
        for (auto const& event : recorded_events_) {
            output << format_event(event) << '\n';
        }
        output.flush();
        std::cout << options_.log_prefix << ": captured " << recorded_events_.size()
                  << " events to '" << record_path_ << "'\n";
    } catch (std::exception const& ex) {
        std::cerr << options_.log_prefix << ": failed writing trace '" << record_path_
                  << "': " << ex.what() << "\n";
    }
}

void WidgetTrace::ensure_start() {
    if (!start_time_valid_) {
        start_time_ = std::chrono::steady_clock::now();
        start_time_valid_ = true;
    }
}

void WidgetTrace::append_record(WidgetTraceEvent event) {
    ensure_start();
    auto now = std::chrono::steady_clock::now();
    event.time_ms = std::chrono::duration<double, std::milli>(now - start_time_).count();
    recorded_events_.push_back(event);
}

std::string_view WidgetTrace::kind_to_string(WidgetTraceEventKind kind) {
    switch (kind) {
    case WidgetTraceEventKind::MouseAbsolute:
        return "mouse_absolute";
    case WidgetTraceEventKind::MouseRelative:
        return "mouse_relative";
    case WidgetTraceEventKind::MouseDown:
        return "mouse_down";
    case WidgetTraceEventKind::MouseUp:
        return "mouse_up";
    case WidgetTraceEventKind::MouseWheel:
        return "mouse_wheel";
    case WidgetTraceEventKind::KeyDown:
        return "key_down";
    case WidgetTraceEventKind::KeyUp:
        return "key_up";
    }
    return "unknown";
}

std::optional<WidgetTraceEventKind> WidgetTrace::string_to_kind(std::string_view value) {
    if (value == "mouse_absolute") {
        return WidgetTraceEventKind::MouseAbsolute;
    }
    if (value == "mouse_relative") {
        return WidgetTraceEventKind::MouseRelative;
    }
    if (value == "mouse_down") {
        return WidgetTraceEventKind::MouseDown;
    }
    if (value == "mouse_up") {
        return WidgetTraceEventKind::MouseUp;
    }
    if (value == "mouse_wheel") {
        return WidgetTraceEventKind::MouseWheel;
    }
    if (value == "key_down") {
        return WidgetTraceEventKind::KeyDown;
    }
    if (value == "key_up") {
        return WidgetTraceEventKind::KeyUp;
    }
    return std::nullopt;
}

std::string WidgetTrace::format_event(WidgetTraceEvent const& event) const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3) << event.time_ms
        << ' ' << "event=" << kind_to_string(event.kind)
        << " x=" << event.x
        << " y=" << event.y
        << " dx=" << event.dx
        << " dy=" << event.dy
        << " wheel=" << event.wheel
        << " button=" << event.button
        << " keycode=" << event.keycode
        << " modifiers=" << event.modifiers
        << " repeat=" << (event.repeat ? 1 : 0)
        << " char=" << static_cast<std::uint32_t>(event.character);
    return oss.str();
}

std::optional<WidgetTraceEvent> WidgetTrace::parse_line(std::string const& line) {
    if (line.empty()) {
        return std::nullopt;
    }
    std::istringstream iss(line);
    WidgetTraceEvent event{};
    if (!(iss >> event.time_ms)) {
        return std::nullopt;
    }
    std::string token;
    while (iss >> token) {
        auto pos = token.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        auto key = token.substr(0, pos);
        auto value = token.substr(pos + 1);
        if (key == "event") {
            if (auto kind = string_to_kind(value)) {
                event.kind = *kind;
            }
        } else if (key == "x") {
            event.x = parse_int(value, event.x);
        } else if (key == "y") {
            event.y = parse_int(value, event.y);
        } else if (key == "dx") {
            event.dx = parse_int(value, event.dx);
        } else if (key == "dy") {
            event.dy = parse_int(value, event.dy);
        } else if (key == "wheel") {
            event.wheel = parse_int(value, event.wheel);
        } else if (key == "button") {
            event.button = parse_int(value, event.button);
        } else if (key == "keycode") {
            event.keycode = parse_uint(value, event.keycode);
        } else if (key == "modifiers") {
            event.modifiers = parse_uint(value, event.modifiers);
        } else if (key == "repeat") {
            event.repeat = parse_int(value, event.repeat ? 1 : 0) != 0;
        } else if (key == "char") {
            auto parsed = parse_uint(value, 0U);
            event.character = static_cast<char32_t>(parsed);
        }
    }
    return event;
}

} // namespace SP::UI

