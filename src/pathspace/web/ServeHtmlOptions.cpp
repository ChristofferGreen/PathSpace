#include <pathspace/web/ServeHtmlOptions.hpp>

#include <pathspace/web/ServeHtmlIdentifier.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace SP::ServeHtml {

namespace {

std::string normalize_root(std::string root, std::string_view fallback) {
    if (root.empty()) {
        return std::string{fallback};
    }
    while (root.size() > 1 && root.back() == '/') {
        root.pop_back();
    }
    if (root.empty()) {
        return std::string{fallback};
    }
    return root;
}

bool is_absolute_path(std::string_view value) {
    return !value.empty() && value.front() == '/';
}

bool is_http_url(std::string_view value) {
    return value.starts_with("http://") || value.starts_with("https://");
}

template <typename T>
bool parse_integer(std::string_view text, T& out) {
    T value{};
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{}) {
        return false;
    }
    out = value;
    return true;
}

template <typename T>
bool parse_integer_in_range(std::string_view text, T min, T max, T& out) {
    T value{};
    if (!parse_integer(text, value)) {
        return false;
    }
    if (value < min || value > max) {
        return false;
    }
    out = value;
    return true;
}

std::optional<bool> parse_bool(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::string normalized;
    normalized.reserve(text.size());
    std::transform(text.begin(), text.end(), std::back_inserter(normalized), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
        return true;
    }
    if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
        return false;
    }
    return std::nullopt;
}

template <typename Setter>
bool apply_env(char const* key, Setter&& setter) {
    if (const char* raw = std::getenv(key)) {
        return setter(std::string_view{raw});
    }
    return true;
}

} // namespace

bool IsValidServeHtmlPort(int port) {
    return port > 0 && port <= 65535;
}

bool IsValidServeHtmlRenderer(std::string_view renderer) {
    return is_identifier(renderer);
}

auto ValidateServeHtmlOptions(ServeHtmlOptions const& options) -> std::optional<std::string> {
    if (options.host.empty()) {
        return std::string{"--host must not be empty"};
    }
    if (!IsValidServeHtmlPort(options.port)) {
        return std::string{"--port must be within 1-65535"};
    }
    if (!IsValidServeHtmlRenderer(options.renderer)) {
        return std::string{"--renderer must be an identifier (letters, numbers, '.', '-', '_')"};
    }
    if (!is_identifier(options.session_cookie_name)) {
        return std::string{"--session-cookie must be an identifier"};
    }
    if (!is_absolute_path(options.apps_root)) {
        return std::string{"--apps-root must be an absolute path"};
    }
    if (!is_absolute_path(options.users_root)) {
        return std::string{"--users-root must be an absolute path"};
    }
    if (options.session_idle_timeout_seconds < 0) {
        return std::string{"--session-timeout must be >= 0"};
    }
    if (options.session_absolute_timeout_seconds < 0) {
        return std::string{"--session-max-age must be >= 0"};
    }
    if (options.session_store_backend != "memory"
        && options.session_store_backend != "pathspace") {
        return std::string{"Unsupported session store backend: " + options.session_store_backend};
    }
    if (options.session_store_backend == "pathspace"
        && !is_absolute_path(options.session_store_path)) {
        return std::string{"PathSpace session store requires an absolute --session-store-root"};
    }
    if (options.ip_rate_limit_per_minute < 0) {
        return std::string{"--rate-limit-ip-per-minute must be >= 0"};
    }
    if (options.ip_rate_limit_burst < 0) {
        return std::string{"--rate-limit-ip-burst must be >= 0"};
    }
    if (options.session_rate_limit_per_minute < 0) {
        return std::string{"--rate-limit-session-per-minute must be >= 0"};
    }
    if (options.session_rate_limit_burst < 0) {
        return std::string{"--rate-limit-session-burst must be >= 0"};
    }
    if (options.demo_refresh_interval_ms < 0) {
        return std::string{"--demo-refresh-interval-ms must be >= 0"};
    }
    if (options.demo_refresh_interval_ms > 0 && !options.seed_demo) {
        return std::string{"--demo-refresh-interval-ms requires --seed-demo"};
    }
    if (!is_absolute_path(options.google_users_root)) {
        return std::string{"--google-users-root must be an absolute path"};
    }
    if (!options.google_redirect_uri.empty() && !is_http_url(options.google_redirect_uri)) {
        return std::string{"--google-redirect-uri must be an absolute http(s) URL"};
    }
    if (!options.google_auth_endpoint.empty() && !is_http_url(options.google_auth_endpoint)) {
        return std::string{"--google-auth-endpoint must be http(s) URL"};
    }
    if (!options.google_token_endpoint.empty() && !is_http_url(options.google_token_endpoint)) {
        return std::string{"--google-token-endpoint must be http(s) URL"};
    }
    if (!options.google_jwks_endpoint.empty() && !is_http_url(options.google_jwks_endpoint)) {
        return std::string{"--google-jwks-endpoint must be http(s) URL"};
    }
    bool google_fields_set = !options.google_client_id.empty()
                             || !options.google_client_secret.empty()
                             || !options.google_redirect_uri.empty();
    if (google_fields_set) {
        if (options.google_client_id.empty() || options.google_client_secret.empty()
            || options.google_redirect_uri.empty()) {
            return std::string{
                "Google OAuth requires --google-client-id, --google-client-secret, and --google-redirect-uri"};
        }
    }
    return std::nullopt;
}

bool ApplyServeHtmlEnvOverrides(ServeHtmlOptions& options) {
    auto require_abs_path = [](std::string_view value, char const* key) -> bool {
        if (!is_absolute_path(value)) {
            std::cerr << key << " must be an absolute path\n";
            return false;
        }
        return true;
    };

    if (!apply_env("PATHSPACE_SERVE_HTML_HOST", [&](std::string_view value) {
            if (value.empty()) {
                std::cerr << "PATHSPACE_SERVE_HTML_HOST must not be empty\n";
                return false;
            }
            options.host = std::string{value};
            return true;
        })) {
        return false;
    }

    if (!apply_env("PATHSPACE_SERVE_HTML_PORT", [&](std::string_view value) {
            int parsed = options.port;
            if (!parse_integer_in_range<int>(value, 1, 65535, parsed)) {
                std::cerr << "PATHSPACE_SERVE_HTML_PORT must be within 1-65535\n";
                return false;
            }
            options.port = parsed;
            return true;
        })) {
        return false;
    }

    if (!apply_env("PATHSPACE_SERVE_HTML_APPS_ROOT", [&](std::string_view value) {
            if (!require_abs_path(value, "PATHSPACE_SERVE_HTML_APPS_ROOT")) {
                return false;
            }
            options.apps_root = std::string{value};
            return true;
        })) {
        return false;
    }

    if (!apply_env("PATHSPACE_SERVE_HTML_USERS_ROOT", [&](std::string_view value) {
            if (!require_abs_path(value, "PATHSPACE_SERVE_HTML_USERS_ROOT")) {
                return false;
            }
            options.users_root = std::string{value};
            return true;
        })) {
        return false;
    }

    if (!apply_env("PATHSPACE_SERVE_HTML_RENDERER", [&](std::string_view value) {
            if (!IsValidServeHtmlRenderer(value)) {
                std::cerr << "PATHSPACE_SERVE_HTML_RENDERER must be an identifier (letters, numbers, '.', '-', '_')\n";
                return false;
            }
            options.renderer = std::string{value};
            return true;
        })) {
        return false;
    }

    if (!apply_env("PATHSPACE_SERVE_HTML_SESSION_COOKIE", [&](std::string_view value) {
            if (!is_identifier(value)) {
                std::cerr << "PATHSPACE_SERVE_HTML_SESSION_COOKIE must be an identifier\n";
                return false;
            }
            options.session_cookie_name = std::string{value};
            return true;
        })) {
        return false;
    }

    auto apply_non_negative_i64 = [&](char const* key, std::int64_t& target, char const* message) {
        return apply_env(key, [&](std::string_view value) {
            std::int64_t parsed = target;
            if (!parse_integer_in_range<std::int64_t>(value, 0, std::numeric_limits<std::int64_t>::max(), parsed)) {
                std::cerr << key << ' ' << message << "\n";
                return false;
            }
            target = parsed;
            return true;
        });
    };

    if (!apply_non_negative_i64("PATHSPACE_SERVE_HTML_SESSION_TIMEOUT",
                                 options.session_idle_timeout_seconds,
                                 "must be >= 0")) {
        return false;
    }

    if (!apply_non_negative_i64("PATHSPACE_SERVE_HTML_SESSION_MAX_AGE",
                                 options.session_absolute_timeout_seconds,
                                 "must be >= 0")) {
        return false;
    }

    if (!apply_env("PATHSPACE_SERVE_HTML_SESSION_STORE", [&](std::string_view value) {
            if (value != "memory" && value != "pathspace") {
                std::cerr << "PATHSPACE_SERVE_HTML_SESSION_STORE must be 'memory' or 'pathspace'\n";
                return false;
            }
            options.session_store_backend = std::string{value};
            return true;
        })) {
        return false;
    }

    if (!apply_env("PATHSPACE_SERVE_HTML_SESSION_STORE_ROOT", [&](std::string_view value) {
            if (!require_abs_path(value, "PATHSPACE_SERVE_HTML_SESSION_STORE_ROOT")) {
                return false;
            }
            options.session_store_path = std::string{value};
            return true;
        })) {
        return false;
    }

    if (!apply_non_negative_i64("PATHSPACE_SERVE_HTML_RATE_LIMIT_IP_PER_MINUTE",
                                 options.ip_rate_limit_per_minute,
                                 "must be >= 0")) {
        return false;
    }

    if (!apply_non_negative_i64("PATHSPACE_SERVE_HTML_RATE_LIMIT_IP_BURST",
                                 options.ip_rate_limit_burst,
                                 "must be >= 0")) {
        return false;
    }

    if (!apply_non_negative_i64("PATHSPACE_SERVE_HTML_RATE_LIMIT_SESSION_PER_MINUTE",
                                 options.session_rate_limit_per_minute,
                                 "must be >= 0")) {
        return false;
    }

    if (!apply_non_negative_i64("PATHSPACE_SERVE_HTML_RATE_LIMIT_SESSION_BURST",
                                 options.session_rate_limit_burst,
                                 "must be >= 0")) {
        return false;
    }

    if (!apply_non_negative_i64("PATHSPACE_SERVE_HTML_DEMO_REFRESH_INTERVAL_MS",
                                 options.demo_refresh_interval_ms,
                                 "must be >= 0")) {
        return false;
    }

    auto apply_bool_env = [&](char const* key, bool& target) {
        return apply_env(key, [&](std::string_view value) {
            auto parsed = parse_bool(value);
            if (!parsed.has_value()) {
                std::cerr << key << " must be a boolean (true/false, 1/0, yes/no)\n";
                return false;
            }
            target = *parsed;
            return true;
        });
    };

    if (!apply_bool_env("PATHSPACE_SERVE_HTML_ALLOW_UNAUTHENTICATED", options.auth_optional)) {
        return false;
    }
    if (!apply_bool_env("PATHSPACE_SERVE_HTML_SEED_DEMO", options.seed_demo)) {
        return false;
    }

    auto apply_http_env = [&](char const* key, std::string& target, char const* message) {
        return apply_env(key, [&](std::string_view value) {
            if (!is_http_url(value)) {
                std::cerr << key << ' ' << message << "\n";
                return false;
            }
            target = std::string{value};
            return true;
        });
    };

    if (!apply_env("PATHSPACE_SERVE_HTML_GOOGLE_CLIENT_ID", [&](std::string_view value) {
            if (value.empty()) {
                std::cerr << "PATHSPACE_SERVE_HTML_GOOGLE_CLIENT_ID must not be empty\n";
                return false;
            }
            options.google_client_id = std::string{value};
            return true;
        })) {
        return false;
    }

    if (!apply_env("PATHSPACE_SERVE_HTML_GOOGLE_CLIENT_SECRET", [&](std::string_view value) {
            if (value.empty()) {
                std::cerr << "PATHSPACE_SERVE_HTML_GOOGLE_CLIENT_SECRET must not be empty\n";
                return false;
            }
            options.google_client_secret = std::string{value};
            return true;
        })) {
        return false;
    }

    if (!apply_env("PATHSPACE_SERVE_HTML_GOOGLE_REDIRECT_URI", [&](std::string_view value) {
            if (!is_http_url(value)) {
                std::cerr << "PATHSPACE_SERVE_HTML_GOOGLE_REDIRECT_URI must be an absolute http(s) URL\n";
                return false;
            }
            options.google_redirect_uri = std::string{value};
            return true;
        })) {
        return false;
    }

    if (!apply_http_env("PATHSPACE_SERVE_HTML_GOOGLE_AUTH_ENDPOINT",
                        options.google_auth_endpoint,
                        "must be http(s) URL")) {
        return false;
    }

    if (!apply_http_env("PATHSPACE_SERVE_HTML_GOOGLE_TOKEN_ENDPOINT",
                        options.google_token_endpoint,
                        "must be http(s) URL")) {
        return false;
    }

    if (!apply_http_env("PATHSPACE_SERVE_HTML_GOOGLE_JWKS_ENDPOINT",
                        options.google_jwks_endpoint,
                        "must be http(s) URL")) {
        return false;
    }

    if (!apply_env("PATHSPACE_SERVE_HTML_GOOGLE_USERS_ROOT", [&](std::string_view value) {
            if (!require_abs_path(value, "PATHSPACE_SERVE_HTML_GOOGLE_USERS_ROOT")) {
                return false;
            }
            options.google_users_root = std::string{value};
            return true;
        })) {
        return false;
    }

    if (!apply_env("PATHSPACE_SERVE_HTML_GOOGLE_SCOPE", [&](std::string_view value) {
        if (value.empty()) {
            std::cerr << "PATHSPACE_SERVE_HTML_GOOGLE_SCOPE must not be empty\n";
            return false;
        }
        options.google_scope = std::string{value};
        return true;
    })) {
        return false;
    }

    return true;
}

void PrintServeHtmlUsage() {
    std::cout << "Usage: pathspace_serve_html [options]\n"
              << "  --host <host>           Bind address (default 127.0.0.1)\n"
              << "  --port <port>           Bind port (default 8080)\n"
              << "  --apps-root <path>      Apps root prefix (default /system/applications)\n"
              << "  --users-root <path>     Users root prefix (default /system/auth/users)\n"
              << "  --renderer <name>       Renderer identifier (default html)\n"
              << "  --session-cookie <name> Session cookie name (default ps_session)\n"
              << "  --session-timeout <sec> Session idle timeout in seconds (default 1800)\n"
              << "  --session-max-age <sec> Session absolute lifetime in seconds (default 28800)\n"
              << "  --session-store <backend> Session store backend (memory|pathspace)\n"
              << "  --session-store-root <path> Session storage path (pathspace backend)\n"
              << "  --rate-limit-ip-per-minute <n> Requests per minute per client IP (default 600)\n"
              << "  --rate-limit-ip-burst <n> Burst capacity per client IP (default 120)\n"
              << "  --rate-limit-session-per-minute <n> Requests per minute per session (default 300)\n"
              << "  --rate-limit-session-burst <n> Burst capacity per session (default 60)\n"
              << "  --allow-unauthenticated Allow /apps/* without login (development)\n"
              << "  --seed-demo             Seed an in-memory demo app (demo_web/gallery)\n"
              << "  --demo-refresh-interval-ms <ms> Demo frame/diagnostic cadence (requires --seed-demo)\n"
              << "  --google-client-id <id> Google OAuth client identifier (enables Google Sign-In)\n"
              << "  --google-client-secret <secret> Google OAuth client secret\n"
              << "  --google-redirect-uri <url> Redirect URI (must point to /login/google/callback)\n"
              << "  --google-auth-endpoint <url> Authorization endpoint override\n"
              << "  --google-token-endpoint <url> Token endpoint override\n"
              << "  --google-jwks-endpoint <url> JWKS endpoint override\n"
              << "  --google-users-root <path> Path storing /<sub> -> username mappings\n"
              << "  --google-scope <scopes> Override OAuth scopes (default: openid email profile)\n"
              << "  --help                  Show this help\n";
}

std::optional<ServeHtmlOptions> ParseServeHtmlArguments(int argc, char** argv) {
    ServeHtmlOptions options{};
    if (!ApplyServeHtmlEnvOverrides(options)) {
        return std::nullopt;
    }

    auto require_value = [&](int& index, std::string_view flag) -> std::optional<std::string_view> {
        if (index + 1 >= argc) {
            std::cerr << flag << " requires a value\n";
            return std::nullopt;
        }
        return std::string_view{argv[++index]};
    };

    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--host") {
            if (auto value = require_value(i, "--host")) {
                if (value->empty()) {
                    std::cerr << "--host must not be empty\n";
                    return std::nullopt;
                }
                options.host = std::string{*value};
            } else {
                return std::nullopt;
            }
        } else if (arg == "--port") {
            if (auto value = require_value(i, "--port")) {
                int parsed = options.port;
                if (!parse_integer_in_range<int>(*value, 1, 65535, parsed)) {
                    std::cerr << "--port must be within 1-65535\n";
                    return std::nullopt;
                }
                options.port = parsed;
            } else {
                return std::nullopt;
            }
        } else if (arg == "--apps-root") {
            if (auto value = require_value(i, "--apps-root")) {
                if (!is_absolute_path(*value)) {
                    std::cerr << "--apps-root must be an absolute path\n";
                    return std::nullopt;
                }
                options.apps_root = std::string{*value};
            } else {
                return std::nullopt;
            }
        } else if (arg == "--users-root") {
            if (auto value = require_value(i, "--users-root")) {
                if (!is_absolute_path(*value)) {
                    std::cerr << "--users-root must be an absolute path\n";
                    return std::nullopt;
                }
                options.users_root = std::string{*value};
            } else {
                return std::nullopt;
            }
        } else if (arg == "--renderer") {
            if (auto value = require_value(i, "--renderer")) {
                if (!IsValidServeHtmlRenderer(*value)) {
                    std::cerr << "--renderer must be an identifier (letters, numbers, '.', '-', '_')\n";
                    return std::nullopt;
                }
                options.renderer = std::string{*value};
            } else {
                return std::nullopt;
            }
        } else if (arg == "--session-cookie") {
            if (auto value = require_value(i, "--session-cookie")) {
                if (!is_identifier(*value)) {
                    std::cerr << "--session-cookie must be an identifier\n";
                    return std::nullopt;
                }
                options.session_cookie_name = std::string{*value};
            } else {
                return std::nullopt;
            }
        } else if (arg == "--session-timeout") {
            if (auto value = require_value(i, "--session-timeout")) {
                std::int64_t parsed = options.session_idle_timeout_seconds;
                if (!parse_integer_in_range<std::int64_t>(*value,
                                                          0,
                                                          std::numeric_limits<std::int64_t>::max(),
                                                          parsed)) {
                    std::cerr << "--session-timeout must be >= 0\n";
                    return std::nullopt;
                }
                options.session_idle_timeout_seconds = parsed;
            } else {
                return std::nullopt;
            }
        } else if (arg == "--session-max-age") {
            if (auto value = require_value(i, "--session-max-age")) {
                std::int64_t parsed = options.session_absolute_timeout_seconds;
                if (!parse_integer_in_range<std::int64_t>(*value,
                                                          0,
                                                          std::numeric_limits<std::int64_t>::max(),
                                                          parsed)) {
                    std::cerr << "--session-max-age must be >= 0\n";
                    return std::nullopt;
                }
                options.session_absolute_timeout_seconds = parsed;
            } else {
                return std::nullopt;
            }
        } else if (arg == "--session-store") {
            if (auto value = require_value(i, "--session-store")) {
                if (*value != "memory" && *value != "pathspace") {
                    std::cerr << "--session-store must be 'memory' or 'pathspace'\n";
                    return std::nullopt;
                }
                options.session_store_backend = std::string{*value};
            } else {
                return std::nullopt;
            }
        } else if (arg == "--session-store-root") {
            if (auto value = require_value(i, "--session-store-root")) {
                if (!is_absolute_path(*value)) {
                    std::cerr << "--session-store-root must be an absolute path\n";
                    return std::nullopt;
                }
                options.session_store_path = std::string{*value};
            } else {
                return std::nullopt;
            }
        } else if (arg == "--rate-limit-ip-per-minute") {
            if (auto value = require_value(i, "--rate-limit-ip-per-minute")) {
                std::int64_t parsed = options.ip_rate_limit_per_minute;
                if (!parse_integer_in_range<std::int64_t>(*value,
                                                          0,
                                                          std::numeric_limits<std::int64_t>::max(),
                                                          parsed)) {
                    std::cerr << "--rate-limit-ip-per-minute must be >= 0\n";
                    return std::nullopt;
                }
                options.ip_rate_limit_per_minute = parsed;
            } else {
                return std::nullopt;
            }
        } else if (arg == "--rate-limit-ip-burst") {
            if (auto value = require_value(i, "--rate-limit-ip-burst")) {
                std::int64_t parsed = options.ip_rate_limit_burst;
                if (!parse_integer_in_range<std::int64_t>(*value,
                                                          0,
                                                          std::numeric_limits<std::int64_t>::max(),
                                                          parsed)) {
                    std::cerr << "--rate-limit-ip-burst must be >= 0\n";
                    return std::nullopt;
                }
                options.ip_rate_limit_burst = parsed;
            } else {
                return std::nullopt;
            }
        } else if (arg == "--rate-limit-session-per-minute") {
            if (auto value = require_value(i, "--rate-limit-session-per-minute")) {
                std::int64_t parsed = options.session_rate_limit_per_minute;
                if (!parse_integer_in_range<std::int64_t>(*value,
                                                          0,
                                                          std::numeric_limits<std::int64_t>::max(),
                                                          parsed)) {
                    std::cerr << "--rate-limit-session-per-minute must be >= 0\n";
                    return std::nullopt;
                }
                options.session_rate_limit_per_minute = parsed;
            } else {
                return std::nullopt;
            }
        } else if (arg == "--rate-limit-session-burst") {
            if (auto value = require_value(i, "--rate-limit-session-burst")) {
                std::int64_t parsed = options.session_rate_limit_burst;
                if (!parse_integer_in_range<std::int64_t>(*value,
                                                          0,
                                                          std::numeric_limits<std::int64_t>::max(),
                                                          parsed)) {
                    std::cerr << "--rate-limit-session-burst must be >= 0\n";
                    return std::nullopt;
                }
                options.session_rate_limit_burst = parsed;
            } else {
                return std::nullopt;
            }
        } else if (arg == "--demo-refresh-interval-ms") {
            if (auto value = require_value(i, "--demo-refresh-interval-ms")) {
                std::int64_t parsed = options.demo_refresh_interval_ms;
                if (!parse_integer_in_range<std::int64_t>(*value,
                                                          0,
                                                          std::numeric_limits<std::int64_t>::max(),
                                                          parsed)) {
                    std::cerr << "--demo-refresh-interval-ms must be >= 0\n";
                    return std::nullopt;
                }
                options.demo_refresh_interval_ms = parsed;
            } else {
                return std::nullopt;
            }
        } else if (arg == "--google-client-id") {
            if (auto value = require_value(i, "--google-client-id")) {
                if (value->empty()) {
                    std::cerr << "--google-client-id must not be empty\n";
                    return std::nullopt;
                }
                options.google_client_id = std::string{*value};
            } else {
                return std::nullopt;
            }
        } else if (arg == "--google-client-secret") {
            if (auto value = require_value(i, "--google-client-secret")) {
                if (value->empty()) {
                    std::cerr << "--google-client-secret must not be empty\n";
                    return std::nullopt;
                }
                options.google_client_secret = std::string{*value};
            } else {
                return std::nullopt;
            }
        } else if (arg == "--google-redirect-uri") {
            if (auto value = require_value(i, "--google-redirect-uri")) {
                if (!is_http_url(*value)) {
                    std::cerr << "--google-redirect-uri must be an absolute http(s) URL\n";
                    return std::nullopt;
                }
                options.google_redirect_uri = std::string{*value};
            } else {
                return std::nullopt;
            }
        } else if (arg == "--google-auth-endpoint") {
            if (auto value = require_value(i, "--google-auth-endpoint")) {
                if (!is_http_url(*value)) {
                    std::cerr << "--google-auth-endpoint must be http(s) URL\n";
                    return std::nullopt;
                }
                options.google_auth_endpoint = std::string{*value};
            } else {
                return std::nullopt;
            }
        } else if (arg == "--google-token-endpoint") {
            if (auto value = require_value(i, "--google-token-endpoint")) {
                if (!is_http_url(*value)) {
                    std::cerr << "--google-token-endpoint must be http(s) URL\n";
                    return std::nullopt;
                }
                options.google_token_endpoint = std::string{*value};
            } else {
                return std::nullopt;
            }
        } else if (arg == "--google-jwks-endpoint") {
            if (auto value = require_value(i, "--google-jwks-endpoint")) {
                if (!is_http_url(*value)) {
                    std::cerr << "--google-jwks-endpoint must be http(s) URL\n";
                    return std::nullopt;
                }
                options.google_jwks_endpoint = std::string{*value};
            } else {
                return std::nullopt;
            }
        } else if (arg == "--google-users-root") {
            if (auto value = require_value(i, "--google-users-root")) {
                if (!is_absolute_path(*value)) {
                    std::cerr << "--google-users-root must be an absolute path\n";
                    return std::nullopt;
                }
                options.google_users_root = std::string{*value};
            } else {
                return std::nullopt;
            }
        } else if (arg == "--google-scope") {
            if (auto value = require_value(i, "--google-scope")) {
                if (value->empty()) {
                    std::cerr << "--google-scope must not be empty\n";
                    return std::nullopt;
                }
                options.google_scope = std::string{*value};
            } else {
                return std::nullopt;
            }
        } else if (arg == "--allow-unauthenticated") {
            options.auth_optional = true;
        } else if (arg == "--seed-demo") {
            options.seed_demo = true;
        } else if (arg == "--help" || arg == "-h") {
            options.show_help = true;
            break;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return std::nullopt;
        }
    }

    options.apps_root = normalize_root(options.apps_root, "/system/applications");
    options.users_root = normalize_root(options.users_root, "/system/auth/users");
    options.session_store_path = normalize_root(options.session_store_path, "/system/web/sessions");
    options.google_users_root = normalize_root(options.google_users_root, "/system/auth/oauth/google");

    if (auto error = ValidateServeHtmlOptions(options)) {
        std::cerr << *error << "\n";
        return std::nullopt;
    }

    return options;
}

} // namespace SP::ServeHtml
