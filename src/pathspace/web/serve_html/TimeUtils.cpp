#include <pathspace/web/serve_html/TimeUtils.hpp>

#include <ctime>
#include <iomanip>
#include <sstream>

namespace SP::ServeHtml {

namespace {

bool gmtime_utc(std::time_t value, std::tm& out) {
#if defined(_WIN32)
    return gmtime_s(&out, &value) == 0;
#else
    return gmtime_r(&value, &out) != nullptr;
#endif
}

} // namespace

auto format_timestamp(std::chrono::system_clock::time_point tp) -> std::string {
    auto seconds_part = std::chrono::time_point_cast<std::chrono::seconds>(tp);
    auto millis       = std::chrono::duration_cast<std::chrono::milliseconds>(tp - seconds_part);
    std::time_t raw   = std::chrono::system_clock::to_time_t(seconds_part);
    std::tm tm{};
    if (!gmtime_utc(raw, tm)) {
        return "1970-01-01T00:00:00.000Z";
    }

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setw(3) << std::setfill('0') << millis.count();
    oss << 'Z';
    return oss.str();
}

auto format_timestamp_from_ns(std::uint64_t timestamp_ns) -> std::string {
    if (timestamp_ns == 0) {
        return {};
    }
    auto duration = std::chrono::nanoseconds(timestamp_ns);
    auto tp       = std::chrono::time_point<std::chrono::system_clock>(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(duration));
    return format_timestamp(tp);
}

} // namespace SP::ServeHtml

