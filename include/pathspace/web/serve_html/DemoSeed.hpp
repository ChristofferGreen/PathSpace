#pragma once

#include <atomic>
#include <chrono>
#include <string_view>

namespace SP {
class PathSpace;
} // namespace SP

namespace SP::ServeHtml {

class ServeHtmlSpace;
struct ServeHtmlOptions;

namespace Demo {

inline constexpr std::string_view kDemoUser{"demo"};
inline constexpr std::string_view kDemoPassword{"demo"};
inline constexpr std::string_view kDemoApp{"demo_web"};
inline constexpr std::string_view kDemoView{"gallery"};
inline constexpr std::string_view kDemoAssetPath{"images/demo-badge.txt"};
inline constexpr std::string_view kDemoGoogleSub{"google-user-123"};

void SeedDemoApplication(SP::PathSpace& space, ServeHtmlOptions const& options);

void RunDemoRefresh(ServeHtmlSpace&            space,
                    ServeHtmlOptions          options,
                    std::chrono::milliseconds interval,
                    std::atomic<bool>&        stop_flag,
                    std::atomic<bool>&        global_stop_flag);

} // namespace Demo

} // namespace SP::ServeHtml
