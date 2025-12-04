#include <pathspace/web/serve_html/DemoSeed.hpp>

#include <pathspace/web/ServeHtmlOptions.hpp>
#include <pathspace/web/ServeHtmlServer.hpp>
#include <pathspace/web/serve_html/PathSpaceUtils.hpp>
#include <pathspace/web/serve_html/Routes.hpp>
#include <pathspace/web/serve_html/auth/Credentials.hpp>

#include "core/Error.hpp"

#include "pathspace/ui/runtime/UIRuntime.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace SP::ServeHtml::Demo {

namespace {

namespace UiRuntime = SP::UI::Runtime;

void insert_or_replace_string(SP::PathSpace& space,
                              std::string const& path,
                              std::string const& value) {
    auto status = replace_single_value<std::string>(space, path, value);
    if (!status) {
        std::cerr << "[serve_html] Failed to seed " << path << ": "
                  << SP::describeError(status.error()) << "\n";
    }
}

} // namespace

void SeedDemoApplication(SP::PathSpace& space, ServeHtmlOptions const& options) {
    auto html_base   = make_html_base(options, kDemoApp, kDemoView);
    auto common_base = make_common_base(options, kDemoApp, kDemoView);
    auto diag_path   = make_diagnostics_path(options, kDemoApp, kDemoView);

    insert_or_replace_string(space,
                             html_base + "/dom",
                             "<div class=\"demo-gallery\"><p>Welcome to PathSpace demo.</p>\n"
                             "<button type=\"button\">Reload demo</button>\n"
                             "<pre id=\"demo-log\">Ready.</pre></div>");

    static constexpr auto kDemoCss =
        ".demo-gallery{font-family:system-ui,sans-serif;max-width:640px;margin:48px auto;"
        "padding:24px;border-radius:20px;background:#f8fafc;box-shadow:0 14px 30px rgba(5,25,45,0.12);}"
        ".demo-gallery h1{font-size:32px;margin-bottom:12px;color:#132b4a;}"
        ".demo-gallery p{font-size:16px;color:#3a4b5c;margin-bottom:16px;}"
        ".demo-gallery button{background:#215ba0;color:#fff;border:none;padding:12px 24px;"
        "border-radius:999px;font-size:16px;cursor:pointer;}"
        ".demo-gallery button:disabled{opacity:0.6;cursor:wait;}"
        ".demo-gallery pre{margin-top:24px;padding:12px;background:#0a1a2b;color:#9cc2ff;"
        "border-radius:12px;font-size:13px;}";

    insert_or_replace_string(space, html_base + "/css", kDemoCss);

    static constexpr auto kDemoJs =
        "document.addEventListener('DOMContentLoaded', () => {\n"
        "  const log = document.getElementById('demo-log');\n"
        "  if (log) {\n"
        "    log.textContent = `Ready. Last refreshed at ${new Date().toLocaleTimeString()}`;\n"
        "  }\n"
        "});\n";
    insert_or_replace_string(space, html_base + "/js", kDemoJs);

    static constexpr auto kDemoCommands =
        "[{\"op\":\"fillRect\",\"args\":[32,32,256,128],\"color\":\"#1f4c94\"}]";
    insert_or_replace_string(space, html_base + "/commands", kDemoCommands);

    auto demo_asset_relative = std::string{kDemoAssetPath};
    auto asset_data_path     = html_base + "/assets/data/" + demo_asset_relative;
    auto asset_meta_path     = html_base + "/assets/meta/" + demo_asset_relative;
    std::vector<std::uint8_t> demo_asset_bytes{'P', 'a', 't', 'h', 'S', 'p', 'a', 'c', 'e',
                                               ' ', 'd', 'e', 'm', 'o', ' ', 'a', 's', 's', 'e', 't'};
    if (auto status = replace_single_value(space, asset_data_path, demo_asset_bytes); !status) {
        std::cerr << "[serve_html] Failed to seed demo asset bytes: "
                  << SP::describeError(status.error()) << "\n";
    }
    if (auto status = replace_single_value<std::string>(space, asset_meta_path, "text/plain");
        !status) {
        std::cerr << "[serve_html] Failed to seed demo asset mime: "
                  << SP::describeError(status.error()) << "\n";
    }

    if (auto status = replace_single_value(space,
                                           html_base + "/assets/manifest",
                                           std::vector<std::string>{std::string{kDemoAssetPath}});
        !status) {
        std::cerr << "[serve_html] Failed to seed demo asset manifest: "
                  << SP::describeError(status.error()) << "\n";
    }

    auto revision_result = space.insert(html_base + "/revision", static_cast<std::uint64_t>(1));
    if (!revision_result.errors.empty()) {
        std::cerr << "[serve_html] Failed to seed revision: "
                  << SP::describeError(revision_result.errors.front()) << "\n";
    }

    auto frame_index_result =
        space.insert(common_base + "/frameIndex", static_cast<std::uint64_t>(1));
    if (!frame_index_result.errors.empty()) {
        std::cerr << "[serve_html] Failed to seed frameIndex: "
                  << SP::describeError(frame_index_result.errors.front()) << "\n";
    }

    UiRuntime::Diagnostics::PathSpaceError initial_error{};
    auto diag_result = space.insert(diag_path, initial_error);
    if (!diag_result.errors.empty()) {
        std::cerr << "[serve_html] Failed to seed diagnostics: "
                  << SP::describeError(diag_result.errors.front()) << "\n";
    }

    auto google_mapping_path = make_google_mapping_path(options, std::string{kDemoGoogleSub});
    if (!google_mapping_path.empty()) {
        auto mapping_result = space.insert(google_mapping_path, std::string{kDemoUser});
        if (!mapping_result.errors.empty()) {
            std::cerr << "[serve_html] Failed to seed demo Google mapping: "
                      << SP::describeError(mapping_result.errors.front()) << "\n";
        }
    }

    SeedDemoCredentials(space, options);

    std::cout << "[serve_html] Seeded demo app at /apps/" << kDemoApp << "/" << kDemoView
              << " (renderer '" << options.renderer << "')\n";
}

void RunDemoRefresh(ServeHtmlSpace&            space,
                    ServeHtmlOptions          options,
                    std::chrono::milliseconds interval,
                    std::atomic<bool>&        stop_flag,
                    std::atomic<bool>&        global_stop_flag) {
    if (interval <= std::chrono::milliseconds(0)) {
        return;
    }
    auto html_base   = make_html_base(options, kDemoApp, kDemoView);
    auto common_base = make_common_base(options, kDemoApp, kDemoView);
    auto diag_path   = make_diagnostics_path(options, kDemoApp, kDemoView);

    std::uint64_t revision    = 1;
    std::uint64_t frame_index = 1;
    bool          emit_error  = false;

    while (!stop_flag.load(std::memory_order_acquire)
           && !global_stop_flag.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(interval);
        ++revision;
        ++frame_index;

        auto frame_result =
            replace_single_value<std::uint64_t>(space, common_base + "/frameIndex", frame_index);
        if (!frame_result) {
            std::cerr << "[serve_html] demo refresh failed to update frameIndex: "
                      << SP::describeError(frame_result.error()) << "\n";
        }

        auto revision_result =
            replace_single_value<std::uint64_t>(space, html_base + "/revision", revision);
        if (!revision_result) {
            std::cerr << "[serve_html] demo refresh failed to update revision: "
                      << SP::describeError(revision_result.error()) << "\n";
        }

        UiRuntime::Diagnostics::PathSpaceError diag{};
        if (emit_error) {
            diag.code         = 1201;
            diag.severity     = UiRuntime::Diagnostics::PathSpaceError::Severity::Warning;
            diag.message      = "Demo renderer warning";
            diag.detail       = "Injected by --demo-refresh-interval-ms";
            diag.path         = html_base;
            diag.revision     = revision;
            auto now          = std::chrono::system_clock::now();
            auto ns           = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
            diag.timestamp_ns = static_cast<std::uint64_t>(ns);
        }
        auto diag_result = replace_single_value<UiRuntime::Diagnostics::PathSpaceError>(space,
                                                                                        diag_path,
                                                                                        diag);
        if (!diag_result) {
            std::cerr << "[serve_html] demo refresh failed to update diagnostics: "
                      << SP::describeError(diag_result.error()) << "\n";
        }

        emit_error = !emit_error;
    }
}

} // namespace SP::ServeHtml::Demo
