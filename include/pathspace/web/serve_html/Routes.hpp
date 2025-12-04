#pragma once

#include <pathspace/web/ServeHtmlOptions.hpp>

#include <string>
#include <string_view>

namespace SP::ServeHtml {

inline std::string make_app_route(std::string_view app, std::string_view view) {
    std::string route = "/apps/";
    route.append(app);
    route.push_back('/');
    route.append(view);
    return route;
}

inline std::string make_app_root_path(ServeHtmlOptions const& options, std::string_view app) {
    std::string root = options.apps_root;
    root.append("/");
    root.append(app);
    return root;
}

inline std::string make_target_base(ServeHtmlOptions const& options,
                                    std::string_view     app,
                                    std::string_view     view) {
    std::string base = options.apps_root;
    base.append("/");
    base.append(app);
    base.append("/renderers/");
    base.append(options.renderer);
    base.append("/targets/html/");
    base.append(view);
    return base;
}

inline std::string make_html_base(ServeHtmlOptions const& options,
                                  std::string_view     app,
                                  std::string_view     view) {
    auto base = make_target_base(options, app, view);
    base.append("/output/v1/html");
    return base;
}

inline std::string make_common_base(ServeHtmlOptions const& options,
                                    std::string_view     app,
                                    std::string_view     view) {
    auto base = make_target_base(options, app, view);
    base.append("/output/v1/common");
    return base;
}

inline std::string make_diagnostics_path(ServeHtmlOptions const& options,
                                         std::string_view     app,
                                         std::string_view     view) {
    auto base = make_target_base(options, app, view);
    base.append("/diagnostics/errors/live");
    return base;
}

inline std::string make_watch_glob(ServeHtmlOptions const& options,
                                   std::string_view     app,
                                   std::string_view     view) {
    auto base = make_target_base(options, app, view);
    base.append("/**");
    return base;
}

inline std::string make_ops_queue_path(ServeHtmlOptions const& options,
                                       std::string_view     app,
                                       std::string_view     op) {
    auto root = make_app_root_path(options, app);
    root.append("/ops/");
    root.append(op);
    root.append("/inbox/queue");
    return root;
}

inline std::string make_events_url(std::string_view app, std::string_view view) {
    auto route = make_app_route(app, view);
    route.append("/events");
    return route;
}

inline std::string make_payload_url(std::string_view app, std::string_view view) {
    auto route = make_app_route(app, view);
    route.append("?format=json");
    return route;
}

inline std::string make_google_mapping_path(ServeHtmlOptions const& options, std::string const& sub) {
    if (options.google_users_root.empty()) {
        return {};
    }
    std::string path = options.google_users_root;
    if (path.back() != '/') {
        path.push_back('/');
    }
    path.append(sub);
    return path;
}

inline std::string make_user_base(ServeHtmlOptions const& options, std::string_view username) {
    std::string base = options.users_root;
    base.append("/");
    base.append(username);
    return base;
}

inline std::string make_user_password_path(ServeHtmlOptions const& options,
                                           std::string_view       username) {
    auto base = make_user_base(options, username);
    base.append("/password_bcrypt");
    return base;
}

} // namespace SP::ServeHtml

