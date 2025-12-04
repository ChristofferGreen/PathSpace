#include <pathspace/web/serve_html/routing/AuthController.hpp>

#include <pathspace/web/ServeHtmlIdentifier.hpp>
#include <pathspace/web/ServeHtmlOptions.hpp>
#include <pathspace/web/ServeHtmlServer.hpp>
#include <pathspace/web/serve_html/Metrics.hpp>
#include <pathspace/web/serve_html/PathSpaceUtils.hpp>
#include <pathspace/web/serve_html/Routes.hpp>
#include <pathspace/web/serve_html/auth/Credentials.hpp>
#include <pathspace/web/serve_html/auth/SessionStore.hpp>
#include <pathspace/web/serve_html/auth/OAuthGoogle.hpp>

#include "core/Error.hpp"

#include "httplib.h"

#include <bcrypt/bcrypt.h>
#include <nlohmann/json.hpp>

#include <iostream>

#include <optional>
#include <vector>

namespace SP::ServeHtml {

namespace {

using json = nlohmann::json;

auto parse_url(std::string_view url) -> std::optional<OAuthGoogle::UrlView> {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) {
        return std::nullopt;
    }
    auto scheme = url.substr(0, scheme_end);
    bool tls    = false;
    if (scheme == "https") {
        tls = true;
    } else if (scheme == "http") {
        tls = false;
    } else {
        return std::nullopt;
    }

    auto remainder = url.substr(scheme_end + 3);
    auto slash     = remainder.find('/');
    std::string_view authority;
    std::string      path;
    if (slash == std::string_view::npos) {
        authority = remainder;
        path      = "/";
    } else {
        authority = remainder.substr(0, slash);
        path      = std::string{remainder.substr(slash)};
        if (path.empty()) {
            path = "/";
        }
    }
    if (authority.empty()) {
        return std::nullopt;
    }

    std::string host;
    int         port = tls ? 443 : 80;
    auto        colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
        host = std::string{authority.substr(0, colon)};
        auto port_view = authority.substr(colon + 1);
        if (port_view.empty()) {
            return std::nullopt;
        }
        int parsed_port = 0;
        auto result = std::from_chars(port_view.data(), port_view.data() + port_view.size(), parsed_port);
        if (result.ec != std::errc{} || parsed_port <= 0 || parsed_port > 65535) {
            return std::nullopt;
        }
        port = parsed_port;
    } else {
        host = std::string{authority};
    }

    if (host.empty()) {
        return std::nullopt;
    }

    return UrlView{std::string{scheme}, std::move(host), std::move(path), port, tls};
}



auto resolve_google_username(ServeHtmlSpace& space,
                             ServeHtmlOptions const& options,
                             std::string const& sub) -> SP::Expected<std::string> {
    auto mapping_path = make_google_mapping_path(options, sub);
    if (mapping_path.empty()) {
        return std::unexpected(SP::Error{SP::Error::Code::NoSuchPath, "google users root unset"});
    }
    auto mapping = read_optional_value<std::string>(space, mapping_path);
    if (!mapping) {
        return std::unexpected(mapping.error());
    }
    if (!mapping->has_value() || mapping->value().empty()) {
        return std::unexpected(SP::Error{SP::Error::Code::NoObjectFound, "google user not registered"});
    }
    return mapping->value();
}


} // namespace

struct AuthController::GoogleConfig {
    bool                                  enabled{false};
    std::optional<OAuthGoogle::UrlView>   auth_url;
    std::optional<OAuthGoogle::UrlView>   token_url;
    OAuthGoogle::AuthStateStore           state_store;
    OAuthGoogle::JwksCache                jwks_cache;
    std::string                           scope;
};

AuthController::~AuthController() = default;

AuthController::AuthController(HttpRequestContext& ctx)
    : ctx_{ctx} {}

auto AuthController::Create(HttpRequestContext& ctx) -> std::unique_ptr<AuthController> {
    auto controller = std::unique_ptr<AuthController>(new AuthController(ctx));
    if (!controller->initialize()) {
        return nullptr;
    }
    return controller;
}

auto AuthController::initialize() -> bool {
    auto const& options = ctx_.options;
    bool google_enabled = !options.google_client_id.empty()
                          && !options.google_client_secret.empty()
                          && !options.google_redirect_uri.empty();
    if (!google_enabled) {
        return true;
    }

    auto google = std::make_unique<GoogleConfig>();
    google->enabled = true;
    google->scope = options.google_scope.empty() ? std::string{"openid email profile"}
                                                : options.google_scope;
    auto auth = parse_url(options.google_auth_endpoint);
    auto token = parse_url(options.google_token_endpoint);
    auto jwks = parse_url(options.google_jwks_endpoint);
    if (!auth || !token || !jwks) {
        std::cerr << "[serve_html] invalid Google OAuth endpoints; disable or fix configuration\n";
        return false;
    }
    google->auth_url = auth;
    google->token_url = token;
    google->jwks_cache.set_endpoint(*jwks);
    google_ = std::move(google);
    return true;
}

void AuthController::register_routes(httplib::Server& server) {
    register_local_routes(server);
    if (google_) {
        register_google_routes(server);
    }
}

void AuthController::register_local_routes(httplib::Server& server) {
    server.Post("/login", [this](httplib::Request const& req, httplib::Response& res) {
        [[maybe_unused]] RequestMetricsScope request_scope{ctx_.metrics, RouteMetric::Login, res};
        auto session_cookie = read_cookie_value(req, ctx_.session_store.cookie_name());
        if (!apply_rate_limits(ctx_, "login", req, res, session_cookie, nullptr)) {
            return;
        }

        auto payload = json::parse(req.body, nullptr, false);
        if (payload.is_discarded()) {
            respond_bad_request(res, "body must be JSON");
            return;
        }
        if (!payload.contains("username") || !payload.contains("password")
            || !payload["username"].is_string() || !payload["password"].is_string()) {
            respond_bad_request(res, "username/password required");
            return;
        }

        auto username = payload["username"].get<std::string>();
        auto password = payload["password"].get<std::string>();
        if (!is_identifier(username) || password.empty()) {
            respond_bad_request(res, "invalid username or password");
            return;
        }

        auto password_path = make_user_password_path(ctx_.options, username);
        auto stored_hash = read_optional_value<std::string>(ctx_.space, password_path);
        if (!stored_hash) {
            respond_server_error(res, "failed to read credentials");
            return;
        }
        if (!stored_hash->has_value()) {
            respond_unauthorized(res);
            ctx_.metrics.record_auth_failure();
            return;
        }

        int check = bcrypt_checkpw(password.c_str(), stored_hash->value().c_str());
        if (check == -1) {
            respond_server_error(res, "bcrypt verification failed");
            return;
        }
        if (check != 0) {
            respond_unauthorized(res);
            ctx_.metrics.record_auth_failure();
            return;
        }

        auto session_id = ctx_.session_store.create_session(username);
        if (!session_id) {
            respond_server_error(res, "failed to create session");
            return;
        }

        apply_session_cookie(ctx_, res, *session_id);
        write_json_response(res, json{{"status", "ok"}, {"username", username}}, 200, true);
    });

    server.Post("/logout", [this](httplib::Request const& req, httplib::Response& res) {
        [[maybe_unused]] RequestMetricsScope request_scope{ctx_.metrics, RouteMetric::Logout, res};
        auto session_cookie = read_cookie_value(req, ctx_.session_store.cookie_name());
        if (!apply_rate_limits(ctx_, "logout", req, res, session_cookie, nullptr)) {
            return;
        }
        if (session_cookie) {
            ctx_.session_store.revoke(*session_cookie);
        }
        expire_session_cookie(ctx_, res);
        write_json_response(res, json{{"status", "ok"}}, 200, true);
    });

    server.Get("/session", [this](httplib::Request const& req, httplib::Response& res) {
        [[maybe_unused]] RequestMetricsScope request_scope{ctx_.metrics, RouteMetric::Session, res};
        auto session_cookie = read_cookie_value(req, ctx_.session_store.cookie_name());
        if (!apply_rate_limits(ctx_, "session", req, res, session_cookie, nullptr)) {
            return;
        }

        bool authenticated = false;
        if (session_cookie) {
            authenticated = ctx_.session_store.validate(*session_cookie).has_value();
            if (!authenticated) {
                expire_session_cookie(ctx_, res);
            }
        }
        write_json_response(res, json{{"authenticated", authenticated}}, 200, true);
    });
}

void AuthController::register_google_routes(httplib::Server& server) {
    if (!google_ || !google_->enabled) {
        return;
    }

    server.Get("/login/google", [this](httplib::Request const& req, httplib::Response& res) {
        [[maybe_unused]] RequestMetricsScope request_scope{ctx_.metrics, RouteMetric::LoginGoogle, res};
        auto session_cookie = read_cookie_value(req, ctx_.session_store.cookie_name());
        if (!apply_rate_limits(ctx_, "login_google", req, res, session_cookie, nullptr)) {
            return;
        }

        auto redirect = req.has_param("redirect") ? req.get_param_value("redirect") : std::string{};
        auto issued    = google_->state_store.issue(redirect);
        auto challenge = OAuthGoogle::compute_code_challenge(issued.entry.code_verifier);

        std::vector<std::pair<std::string, std::string>> params{
            {"client_id", ctx_.options.google_client_id},
            {"redirect_uri", ctx_.options.google_redirect_uri},
            {"response_type", "code"},
            {"scope", google_->scope},
            {"state", issued.state},
            {"code_challenge", challenge},
            {"code_challenge_method", "S256"},
            {"access_type", "online"},
        };

        auto location = google_->auth_url ? OAuthGoogle::build_absolute_url(*google_->auth_url)
                                          : std::string{};
        location.push_back(location.find('?') == std::string::npos ? '?' : '&');
        location.append(OAuthGoogle::build_query_string(params));

        res.status = 302;
        res.set_header("Location", location);
        res.set_header("Cache-Control", "no-store");
    });

    server.Get("/login/google/callback",
               [this](httplib::Request const& req, httplib::Response& res) {
                   [[maybe_unused]] RequestMetricsScope request_scope{
                       ctx_.metrics, RouteMetric::LoginGoogleCallback, res};
                   auto session_cookie = read_cookie_value(req, ctx_.session_store.cookie_name());
                   if (!apply_rate_limits(ctx_, "login_google_callback", req, res, session_cookie, nullptr)) {
                       return;
                   }
                   if (req.has_param("error")) {
                       auto message = req.get_param_value("error");
                       respond_bad_request(res, "google_auth_error: " + message);
                       return;
                   }
                   if (!req.has_param("state") || !req.has_param("code")) {
                       respond_bad_request(res, "missing state or code");
                       return;
                   }

                   auto state = req.get_param_value("state");
                   auto code  = req.get_param_value("code");
                   auto entry = google_->state_store.take(state);
                   if (!entry) {
                       respond_bad_request(res, "invalid oauth state");
                       return;
                   }
                   if (code.empty()) {
                       respond_bad_request(res, "authorization code missing");
                       return;
                   }
                   if (!google_->token_url) {
                       respond_server_error(res, "Google OAuth not configured");
                       return;
                   }

                   std::vector<std::pair<std::string, std::string>> token_params{{
                       {"client_id", ctx_.options.google_client_id},
                       {"client_secret", ctx_.options.google_client_secret},
                       {"redirect_uri", ctx_.options.google_redirect_uri},
                       {"grant_type", "authorization_code"},
                       {"code", code},
                       {"code_verifier", entry->code_verifier},
                   }};
                   auto id_token = OAuthGoogle::exchange_authorization_code(
                       *google_->token_url, token_params);
                   if (!id_token) {
                       std::cerr << "[serve_html] google token exchange failed: "
                                 << OAuthGoogle::describe_error(id_token.error()) << "\n";
                       respond_server_error(res, "Google token exchange failed");
                       return;
                   }

                   auto decoded = OAuthGoogle::decode_id_token(
                       *id_token, google_->jwks_cache, ctx_.options.google_client_id);
                   if (!decoded) {
                       std::cerr << "[serve_html] google id token invalid: "
                                 << OAuthGoogle::describe_error(decoded.error()) << "\n";
                       respond_unauthorized(res);
                       ctx_.metrics.record_auth_failure();
                       return;
                   }
                   auto username = resolve_google_username(ctx_.space, ctx_.options, decoded->sub);
                   if (!username) {
                       respond_unauthorized(res);
                       ctx_.metrics.record_auth_failure();
                       return;
                   }

                   auto session_id = ctx_.session_store.create_session(*username);
                   if (!session_id) {
                       respond_server_error(res, "failed to create session");
                       return;
                   }

                   apply_session_cookie(ctx_, res, *session_id);
                   res.status = 302;
                   auto redirect = entry->redirect.empty() ? std::string{"/apps"} : entry->redirect;
                   res.set_header("Location", redirect);
                   res.set_header("Cache-Control", "no-store");
               });
}

} // namespace SP::ServeHtml
