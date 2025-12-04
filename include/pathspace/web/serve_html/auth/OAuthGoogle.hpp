#pragma once

#include <chrono>
#include <expected>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SP::ServeHtml::OAuthGoogle {

struct UrlView {
    std::string scheme;
    std::string host;
    std::string path;
    int         port{0};
    bool        tls{false};
};

auto parse_url(std::string_view url) -> std::optional<UrlView>;
auto build_absolute_url(UrlView const& url) -> std::string;
auto build_query_string(std::vector<std::pair<std::string, std::string>> const& params) -> std::string;
std::string compute_code_challenge(std::string_view verifier);

struct AuthStateEntry {
    std::string                           redirect;
    std::string                           code_verifier;
    std::chrono::steady_clock::time_point created_at{};
};

struct IssuedState {
    std::string   state;
    AuthStateEntry entry;
};

class AuthStateStore {
public:
    AuthStateStore();

    auto issue(std::string redirect) -> IssuedState;
    auto take(std::string const& state) -> std::optional<AuthStateEntry>;

private:
    void prune_locked();

    std::unordered_map<std::string, AuthStateEntry> entries_;
    std::mutex                                      mutex_;
    std::mt19937                                    rng_;
};

struct JwksKey {
    std::string               key_id;
    std::string               algorithm;
    std::string               use;
    std::string               modulus_b64;
    std::string               exponent_b64;
    std::vector<unsigned char> modulus;
    std::vector<unsigned char> exponent;
};

class JwksCache {
public:
    void set_endpoint(UrlView endpoint);
    auto lookup(std::string const& key_id) -> std::optional<JwksKey>;

private:
    void refresh_locked();

    std::optional<UrlView>              endpoint_;
    std::unordered_map<std::string, JwksKey> keys_;
    std::chrono::steady_clock::time_point    next_refresh_{};
    std::mutex                               mutex_;
};

struct IdTokenPayload {
    std::string sub;
    std::string email;
    bool        email_verified{false};
};

enum class ErrorCode {
    InvalidUrl,
    HttpClientUnavailable,
    HttpRequestFailed,
    InvalidResponse,
    MissingKey,
    CryptoInitializationFailed,
    SignatureVerificationFailed,
    ClaimValidationFailed,
};

struct Error {
    ErrorCode   code;
    std::string message;
};

auto describe_error(Error const& error) -> std::string;

auto exchange_authorization_code(UrlView const& token_url,
                                 std::vector<std::pair<std::string, std::string>> const& params)
    -> std::expected<std::string, Error>;

auto decode_id_token(std::string const& token,
                     JwksCache&           jwks_cache,
                     std::string const&   expected_audience)
    -> std::expected<IdTokenPayload, Error>;

} // namespace SP::ServeHtml::OAuthGoogle
