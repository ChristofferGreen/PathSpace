#ifndef CPPHTTPLIB_NO_EXCEPTIONS
#define CPPHTTPLIB_NO_EXCEPTIONS
#endif
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif

#include "httplib.h"

#include <pathspace/web/serve_html/auth/OAuthGoogle.hpp>

#include <nlohmann/json.hpp>

#include <openssl/bn.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>

#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace SP::ServeHtml::OAuthGoogle {

namespace {

using json = nlohmann::json;

constexpr std::string_view kGoogleIssuerPrimary{"https://accounts.google.com"};
constexpr std::string_view kGoogleIssuerLegacy{"accounts.google.com"};
constexpr std::chrono::seconds kGoogleStateTtl{std::chrono::seconds{600}};
constexpr std::chrono::minutes kGoogleJwksTtl{std::chrono::minutes{15}};
constexpr std::chrono::seconds kHttpRequestTimeout{std::chrono::seconds{5}};
constexpr std::chrono::seconds kTokenClockSkew{std::chrono::seconds{60}};

struct SimpleHttpResponse {
    int              status{0};
    std::string      body;
    httplib::Headers headers;
};

std::string base64_url_encode(std::string_view input) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string           output;
    output.reserve(((input.size() + 2) / 3) * 4);
    std::uint32_t chunk = 0;
    int           chunk_bits = 0;
    for (unsigned char ch : input) {
        chunk = (chunk << 8) | ch;
        chunk_bits += 8;
        while (chunk_bits >= 6) {
            chunk_bits -= 6;
            auto index = static_cast<unsigned>((chunk >> chunk_bits) & 0x3F);
            output.push_back(kAlphabet[index]);
        }
    }
    if (chunk_bits > 0) {
        chunk <<= (6 - chunk_bits);
        auto index = static_cast<unsigned>(chunk & 0x3F);
        output.push_back(kAlphabet[index]);
    }
    return output;
}

auto decode_base64url(std::string_view input) -> std::optional<std::vector<unsigned char>> {
    auto decode_char = [](char ch) -> std::optional<unsigned char> {
        if (ch >= 'A' && ch <= 'Z') {
            return static_cast<unsigned char>(ch - 'A');
        }
        if (ch >= 'a' && ch <= 'z') {
            return static_cast<unsigned char>(26 + ch - 'a');
        }
        if (ch >= '0' && ch <= '9') {
            return static_cast<unsigned char>(52 + ch - '0');
        }
        if (ch == '-') {
            return static_cast<unsigned char>(62);
        }
        if (ch == '_') {
            return static_cast<unsigned char>(63);
        }
        return std::nullopt;
    };

    std::vector<unsigned char> output;
    output.reserve((input.size() * 3) / 4);
    std::uint32_t value = 0;
    int           bits  = 0;
    for (char ch : input) {
        auto decoded = decode_char(ch);
        if (!decoded) {
            return std::nullopt;
        }
        value = (value << 6) | *decoded;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<unsigned char>((value >> bits) & 0xFF));
        }
    }
    return output;
}

auto decode_base64url_string(std::string_view input) -> std::optional<std::string> {
    auto decoded = decode_base64url(input);
    if (!decoded) {
        return std::nullopt;
    }
    return std::string(decoded->begin(), decoded->end());
}

std::unique_ptr<httplib::ClientImpl> make_http_client(UrlView const& url) {
    std::unique_ptr<httplib::ClientImpl> client;
    if (url.tls) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        auto ssl_client = std::make_unique<httplib::SSLClient>(url.host, url.port);
        ssl_client->enable_server_certificate_verification(true);
        client = std::unique_ptr<httplib::ClientImpl>(std::move(ssl_client));
#else
        (void)url;
        return nullptr;
#endif
    } else {
        client = std::make_unique<httplib::ClientImpl>(url.host, url.port);
    }
    int timeout_seconds = static_cast<int>(kHttpRequestTimeout.count());
    client->set_connection_timeout(timeout_seconds, 0);
    client->set_read_timeout(timeout_seconds, 0);
    client->set_write_timeout(timeout_seconds, 0);
    client->set_follow_location(false);
    client->set_keep_alive(false);
    return client;
}

auto http_post_form(UrlView const& url, std::string const& body)
    -> std::expected<SimpleHttpResponse, Error> {
    auto client = make_http_client(url);
    if (!client) {
        return std::unexpected(Error{ErrorCode::HttpClientUnavailable, "http client unavailable"});
    }
    httplib::Headers headers{{"Accept", "application/json"}};
    auto response = client->Post(url.path.c_str(), headers, body, "application/x-www-form-urlencoded");
    if (!response) {
        return std::unexpected(Error{ErrorCode::HttpRequestFailed, "POST request failed"});
    }
    return SimpleHttpResponse{response->status, response->body, response->headers};
}

auto http_get(UrlView const& url) -> std::expected<SimpleHttpResponse, Error> {
    auto client = make_http_client(url);
    if (!client) {
        return std::unexpected(Error{ErrorCode::HttpClientUnavailable, "http client unavailable"});
    }
    auto response = client->Get(url.path.c_str());
    if (!response) {
        return std::unexpected(Error{ErrorCode::HttpRequestFailed, "GET request failed"});
    }
    return SimpleHttpResponse{response->status, response->body, response->headers};
}

std::string percent_encode(std::string_view value) {
    static constexpr char kHexDigits[] = "0123456789ABCDEF";
    std::string           encoded;
    encoded.reserve(value.size() * 2);
    for (unsigned char ch : value) {
        if ((std::isalnum(ch) != 0) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(kHexDigits[(ch >> 4) & 0x0F]);
            encoded.push_back(kHexDigits[ch & 0x0F]);
        }
    }
    return encoded;
}

auto parse_claim_int(json const& claim) -> std::optional<std::int64_t> {
    if (claim.is_number_integer()) {
        return claim.get<std::int64_t>();
    }
    if (claim.is_string()) {
        auto text = claim.get<std::string>();
        if (text.empty()) {
            return std::nullopt;
        }
        std::int64_t value = 0;
        auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec == std::errc{}) {
            return value;
        }
    }
    return std::nullopt;
}

bool aud_matches(json const& claim, std::string const& expected) {
    if (claim.is_string()) {
        return claim.get<std::string>() == expected;
    }
    if (claim.is_array()) {
        for (auto const& entry : claim) {
            if (entry.is_string() && entry.get<std::string>() == expected) {
                return true;
            }
        }
    }
    return false;
}

struct RsaDeleter {
    void operator()(RSA* rsa) const {
        if (rsa != nullptr) {
            RSA_free(rsa);
        }
    }
};

using RsaPtr = std::unique_ptr<RSA, RsaDeleter>;

auto make_rsa(JwksKey const& key) -> RsaPtr {
    BIGNUM* modulus  = BN_bin2bn(key.modulus.data(), static_cast<int>(key.modulus.size()), nullptr);
    BIGNUM* exponent = BN_bin2bn(key.exponent.data(), static_cast<int>(key.exponent.size()), nullptr);
    if (!modulus || !exponent) {
        if (modulus) {
            BN_free(modulus);
        }
        if (exponent) {
            BN_free(exponent);
        }
        return nullptr;
    }
    RSA* rsa = RSA_new();
    if (!rsa) {
        BN_free(modulus);
        BN_free(exponent);
        return nullptr;
    }
    if (RSA_set0_key(rsa, modulus, exponent, nullptr) != 1) {
        RSA_free(rsa);
        BN_free(modulus);
        BN_free(exponent);
        return nullptr;
    }
    return RsaPtr{rsa};
}

std::string generate_token(std::mt19937& rng) {
    std::uniform_int_distribution<> dist(0, 255);
    std::array<unsigned char, 16>    bytes{};
    for (auto& byte : bytes) {
        byte = static_cast<unsigned char>(dist(rng));
    }
    return base64_url_encode(
        std::string_view{reinterpret_cast<char const*>(bytes.data()), bytes.size()});
}

std::string generate_code_verifier_internal() {
    static constexpr char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~";
    std::random_device rd;
    std::mt19937       gen{rd()};
    std::uniform_int_distribution<> dist(0, static_cast<int>(std::size(alphabet)) - 1);
    std::string verifier;
    verifier.reserve(64);
    for (int i = 0; i < 64; ++i) {
        verifier.push_back(alphabet[dist(gen)]);
    }
    return verifier;
}

} // namespace

AuthStateStore::AuthStateStore()
    : rng_{std::random_device{}()} {}

auto AuthStateStore::issue(std::string redirect) -> IssuedState {
    IssuedState issued;
    issued.state          = generate_token(rng_);
    issued.entry.redirect = std::move(redirect);
    issued.entry.code_verifier = generate_code_verifier_internal();
    issued.entry.created_at    = std::chrono::steady_clock::now();

    std::lock_guard const lock{mutex_};
    entries_[issued.state] = issued.entry;
    prune_locked();
    return issued;
}

auto AuthStateStore::take(std::string const& state) -> std::optional<AuthStateEntry> {
    std::lock_guard const lock{mutex_};
    auto                  it = entries_.find(state);
    if (it == entries_.end()) {
        return std::nullopt;
    }
    auto entry = it->second;
    entries_.erase(it);
    prune_locked();
    return entry;
}

void AuthStateStore::prune_locked() {
    auto now = std::chrono::steady_clock::now();
    for (auto it = entries_.begin(); it != entries_.end();) {
        if ((now - it->second.created_at) > kGoogleStateTtl || entries_.size() > 1024) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

void JwksCache::set_endpoint(UrlView endpoint) {
    std::lock_guard const lock{mutex_};
    endpoint_ = std::move(endpoint);
}

auto JwksCache::lookup(std::string const& key_id) -> std::optional<JwksKey> {
    std::lock_guard const lock{mutex_};
    auto now = std::chrono::steady_clock::now();
    if (now > next_refresh_) {
        refresh_locked();
        next_refresh_ = now + kGoogleJwksTtl;
    }
    if (auto it = keys_.find(key_id); it != keys_.end()) {
        return it->second;
    }
    refresh_locked();
    next_refresh_ = now + kGoogleJwksTtl;
    if (auto it = keys_.find(key_id); it != keys_.end()) {
        return it->second;
    }
    return std::nullopt;
}

void JwksCache::refresh_locked() {
    if (!endpoint_) {
        return;
    }
    auto response = http_get(*endpoint_);
    if (!response || response->status != 200) {
        return;
    }
    auto payload = json::parse(response->body, nullptr, false);
    if (payload.is_discarded() || !payload.is_object() || !payload.contains("keys")) {
        return;
    }
    auto const& keys = payload["keys"];
    if (!keys.is_array()) {
        return;
    }

    std::unordered_map<std::string, JwksKey> parsed;
    for (auto const& entry : keys) {
        if (!entry.contains("kid") || !entry.contains("n") || !entry.contains("e")) {
            continue;
        }
        JwksKey key;
        key.key_id      = entry["kid"].get<std::string>();
        key.algorithm   = entry.value("alg", "");
        key.use         = entry.value("use", "");
        key.modulus_b64 = entry["n"].get<std::string>();
        key.exponent_b64 = entry["e"].get<std::string>();
        auto modulus = decode_base64url(key.modulus_b64);
        auto exponent = decode_base64url(key.exponent_b64);
        if (!modulus || !exponent) {
            continue;
        }
        key.modulus = std::move(*modulus);
        key.exponent = std::move(*exponent);
        parsed[key.key_id] = std::move(key);
    }
    if (!parsed.empty()) {
        keys_ = std::move(parsed);
    }
}

auto parse_url(std::string_view url) -> std::optional<UrlView> {
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

auto build_absolute_url(UrlView const& url) -> std::string {
    std::string absolute = url.scheme;
    absolute.append("://");
    absolute.append(url.host);
    bool default_port = (url.tls && url.port == 443) || (!url.tls && url.port == 80);
    if (!default_port) {
        absolute.push_back(':');
        absolute.append(std::to_string(url.port));
    }
    absolute.append(url.path);
    return absolute;
}

auto build_query_string(std::vector<std::pair<std::string, std::string>> const& params) -> std::string {
    std::string query;
    bool        first = true;
    for (auto const& param : params) {
        if (!first) {
            query.push_back('&');
        }
        first = false;
        query.append(percent_encode(param.first));
        query.push_back('=');
        query.append(percent_encode(param.second));
    }
    return query;
}

std::string compute_code_challenge(std::string_view verifier) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<unsigned const char*>(verifier.data()), verifier.size(), hash);
    return base64_url_encode(std::string_view{reinterpret_cast<char const*>(hash), SHA256_DIGEST_LENGTH});
}

auto describe_error(Error const& error) -> std::string {
    switch (error.code) {
    case ErrorCode::InvalidUrl:
        return "invalid_url: " + error.message;
    case ErrorCode::HttpClientUnavailable:
        return "http_client_unavailable: " + error.message;
    case ErrorCode::HttpRequestFailed:
        return "http_request_failed: " + error.message;
    case ErrorCode::InvalidResponse:
        return "invalid_response: " + error.message;
    case ErrorCode::MissingKey:
        return "missing_key: " + error.message;
    case ErrorCode::CryptoInitializationFailed:
        return "crypto_init_failed: " + error.message;
    case ErrorCode::SignatureVerificationFailed:
        return "signature_verification_failed: " + error.message;
    case ErrorCode::ClaimValidationFailed:
        return "claim_validation_failed: " + error.message;
    }
    return error.message;
}

auto exchange_authorization_code(UrlView const& token_url,
                                 std::vector<std::pair<std::string, std::string>> const& params)
    -> std::expected<std::string, Error> {
    auto body      = build_query_string(params);
    auto response  = http_post_form(token_url, body);
    if (!response) {
        return std::unexpected(response.error());
    }
    if (response->status != 200) {
        return std::unexpected(Error{ErrorCode::HttpRequestFailed,
                                     "token endpoint returned status "
                                         + std::to_string(response->status)});
    }
    auto token_payload = json::parse(response->body, nullptr, false);
    if (token_payload.is_discarded() || !token_payload.contains("id_token")
        || !token_payload["id_token"].is_string()) {
        return std::unexpected(Error{ErrorCode::InvalidResponse, "id_token missing"});
    }
    return token_payload["id_token"].get<std::string>();
}

auto decode_id_token(std::string const& token,
                     JwksCache&           jwks_cache,
                     std::string const&   expected_audience)
    -> std::expected<IdTokenPayload, Error> {
    auto first_dot = token.find('.');
    auto second_dot = token.find('.', first_dot == std::string::npos ? first_dot : first_dot + 1);
    if (first_dot == std::string::npos || second_dot == std::string::npos) {
        return std::unexpected(Error{ErrorCode::InvalidResponse, "token missing segments"});
    }
    auto header_b64  = token.substr(0, first_dot);
    auto payload_b64 = token.substr(first_dot + 1, second_dot - first_dot - 1);
    auto signature   = token.substr(second_dot + 1);

    auto header_json  = decode_base64url_string(header_b64);
    auto payload_json = decode_base64url_string(payload_b64);
    if (!header_json || !payload_json) {
        return std::unexpected(Error{ErrorCode::InvalidResponse, "unable to decode jwt"});
    }
    auto header = json::parse(*header_json, nullptr, false);
    auto payload = json::parse(*payload_json, nullptr, false);
    if (header.is_discarded() || payload.is_discarded() || !header.contains("kid")) {
        return std::unexpected(Error{ErrorCode::InvalidResponse, "jwt header invalid"});
    }

    auto key_id = header["kid"].get<std::string>();
    auto jwks   = jwks_cache.lookup(key_id);
    if (!jwks) {
        return std::unexpected(Error{ErrorCode::MissingKey, "jwks key not found"});
    }
    auto rsa = make_rsa(*jwks);
    if (!rsa) {
        return std::unexpected(Error{ErrorCode::CryptoInitializationFailed, "rsa init failed"});
    }

    auto signed_data = token.substr(0, second_dot);
    auto signature_bytes = decode_base64url(signature);
    if (!signature_bytes || signature_bytes->empty()) {
        return std::unexpected(Error{ErrorCode::InvalidResponse, "jwt signature missing"});
    }

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<unsigned const char*>(signed_data.data()), signed_data.size(), hash);
    if (RSA_verify(NID_sha256,
                   hash,
                   SHA256_DIGEST_LENGTH,
                   signature_bytes->data(),
                   static_cast<unsigned int>(signature_bytes->size()),
                   rsa.get()) != 1) {
        return std::unexpected(Error{ErrorCode::SignatureVerificationFailed, "rsa verify failed"});
    }

    if (!payload.contains("aud") || !aud_matches(payload["aud"], expected_audience)) {
        return std::unexpected(Error{ErrorCode::ClaimValidationFailed, "audience mismatch"});
    }
    if (!payload.contains("iss") || !payload["iss"].is_string()) {
        return std::unexpected(Error{ErrorCode::ClaimValidationFailed, "issuer missing"});
    }
    auto issuer = payload["iss"].get<std::string>();
    if (issuer != kGoogleIssuerPrimary && issuer != kGoogleIssuerLegacy) {
        return std::unexpected(Error{ErrorCode::ClaimValidationFailed, "issuer invalid"});
    }
    auto now_seconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch());
    if (!payload.contains("exp")) {
        return std::unexpected(Error{ErrorCode::ClaimValidationFailed, "exp missing"});
    }
    auto expires_at = parse_claim_int(payload["exp"]);
    if (!expires_at) {
        return std::unexpected(Error{ErrorCode::ClaimValidationFailed, "exp invalid"});
    }
    if (now_seconds.count() - kTokenClockSkew.count() > *expires_at) {
        return std::unexpected(Error{ErrorCode::ClaimValidationFailed, "token expired"});
    }
    if (payload.contains("nbf")) {
        if (auto not_before = parse_claim_int(payload["nbf"])) {
            if (now_seconds.count() + kTokenClockSkew.count() < *not_before) {
                return std::unexpected(Error{ErrorCode::ClaimValidationFailed, "token not active"});
            }
        }
    }

    if (!payload.contains("sub") || !payload["sub"].is_string()) {
        return std::unexpected(Error{ErrorCode::ClaimValidationFailed, "sub missing"});
    }
    auto subject = payload["sub"].get<std::string>();
    if (subject.empty()) {
        return std::unexpected(Error{ErrorCode::ClaimValidationFailed, "sub empty"});
    }

    IdTokenPayload result;
    result.sub = std::move(subject);
    if (payload.contains("email") && payload["email"].is_string()) {
        result.email = payload["email"].get<std::string>();
    }
    if (payload.contains("email_verified")) {
        if (payload["email_verified"].is_boolean()) {
            result.email_verified = payload["email_verified"].get<bool>();
        } else if (payload["email_verified"].is_string()) {
            auto flag = payload["email_verified"].get<std::string>();
            result.email_verified = (flag == "true" || flag == "1");
        }
    }
    return result;
}

} // namespace SP::ServeHtml::OAuthGoogle
