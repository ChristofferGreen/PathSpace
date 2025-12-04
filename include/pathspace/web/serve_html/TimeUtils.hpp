#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace SP::ServeHtml {

auto format_timestamp(std::chrono::system_clock::time_point tp) -> std::string;

auto format_timestamp_from_ns(std::uint64_t timestamp_ns) -> std::string;

} // namespace SP::ServeHtml

