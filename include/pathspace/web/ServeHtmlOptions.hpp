#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace SP::ServeHtml {

struct ServeHtmlOptions {
    std::string host{"127.0.0.1"};
    int         port{8080};
    std::string apps_root{"/system/applications"};
    std::string users_root{"/system/auth/users"};
    std::string renderer{"html"};
    std::string session_cookie_name{"ps_session"};
    std::int64_t session_idle_timeout_seconds{1800};
    std::int64_t session_absolute_timeout_seconds{28800};
    std::string session_store_backend{"memory"};
    std::string session_store_path{"/system/web/sessions"};
    std::int64_t ip_rate_limit_per_minute{600};
    std::int64_t ip_rate_limit_burst{120};
    std::int64_t session_rate_limit_per_minute{300};
    std::int64_t session_rate_limit_burst{60};
    std::int64_t demo_refresh_interval_ms{0};
    bool        auth_optional{false};
    bool        seed_demo{false};
    bool        show_help{false};
    std::string google_client_id;
    std::string google_client_secret;
    std::string google_redirect_uri;
    std::string google_auth_endpoint{"https://accounts.google.com/o/oauth2/v2/auth"};
    std::string google_token_endpoint{"https://oauth2.googleapis.com/token"};
    std::string google_jwks_endpoint{"https://www.googleapis.com/oauth2/v3/certs"};
    std::string google_users_root{"/system/auth/oauth/google"};
    std::string google_scope{"openid email profile"};
};

auto ParseServeHtmlArguments(int argc, char** argv) -> std::optional<ServeHtmlOptions>;

void PrintServeHtmlUsage();

bool ApplyServeHtmlEnvOverrides(ServeHtmlOptions& options);

auto ValidateServeHtmlOptions(ServeHtmlOptions const& options) -> std::optional<std::string>;

bool IsValidServeHtmlPort(int port);
bool IsValidServeHtmlRenderer(std::string_view renderer);

} // namespace SP::ServeHtml

