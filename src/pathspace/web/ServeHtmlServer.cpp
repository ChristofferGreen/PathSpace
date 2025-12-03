#define CPPHTTPLIB_NO_EXCEPTIONS
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"

#include <pathspace/web/ServeHtmlServer.hpp>

#include "core/Error.hpp"
#include "pathspace/ui/runtime/UIRuntime.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <bcrypt/bcrypt.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>

namespace SP::ServeHtml {

static std::atomic<bool> g_should_stop{false};

namespace {

using json = nlohmann::json;
namespace UiRuntime = SP::UI::Runtime;

std::string format_timestamp(std::chrono::system_clock::time_point tp);

struct SessionConfig {
    std::string                       cookie_name;
    std::chrono::seconds              idle_timeout{std::chrono::seconds{0}};
    std::chrono::seconds              absolute_timeout{std::chrono::seconds{0}};
};

class SessionStore {
public:
    explicit SessionStore(SessionConfig config)
        : config_{std::move(config)} {}

    virtual ~SessionStore() = default;

    auto create_session(std::string username) -> std::optional<std::string> {
        if (username.empty()) {
            return std::nullopt;
        }
        SessionRecord record{};
        record.id = generate_token();
        record.username = std::move(username);
        record.created_at = Clock::now();
        record.last_seen = record.created_at;
        if (!write_session(record)) {
            std::cerr << "[serve_html] failed to persist session " << record.id << "\n";
            return std::nullopt;
        }
        return record.id;
    }

    auto validate(std::string const& id) -> std::optional<std::string> {
        if (id.empty()) {
            return std::nullopt;
        }

        auto const now = Clock::now();
        auto       record = read_session(id);
        if (!record) {
            return std::nullopt;
        }
        if (is_expired(*record, now)) {
            delete_session(id);
            return std::nullopt;
        }
        record->last_seen = now;
        if (!write_session(*record)) {
            std::cerr << "[serve_html] failed to update session " << id << "\n";
            return std::nullopt;
        }
        return record->username;
    }

    void revoke(std::string const& id) {
        if (id.empty()) {
            return;
        }
        delete_session(id);
    }

    auto cookie_max_age() const -> std::chrono::seconds {
        if (config_.absolute_timeout.count() > 0) {
            return config_.absolute_timeout;
        }
        return config_.idle_timeout.count() > 0 ? config_.idle_timeout
                                                : std::chrono::seconds{0};
    }

    auto cookie_name() const -> std::string const& {
        return config_.cookie_name;
    }

protected:
    using Clock = std::chrono::system_clock;

    struct SessionRecord {
        std::string       id;
        std::string       username;
        Clock::time_point created_at{};
        Clock::time_point last_seen{};
    };

    static auto generate_token() -> std::string {
        std::array<unsigned char, 32> buffer{};
        std::random_device            device;
        for (auto& byte : buffer) {
            byte = static_cast<unsigned char>(device());
        }

        std::ostringstream stream;
        stream << std::hex << std::setfill('0');
        for (auto byte : buffer) {
            stream << std::setw(2)
                   << static_cast<int>(byte);
        }
        return stream.str();
    }

    auto is_expired(SessionRecord const& record, Clock::time_point now) const -> bool {
        if (config_.absolute_timeout.count() > 0
            && (now - record.created_at) > config_.absolute_timeout) {
            return true;
        }
        if (config_.idle_timeout.count() > 0
            && (now - record.last_seen) > config_.idle_timeout) {
            return true;
        }
        return false;
    }

    virtual auto read_session(std::string const& id) -> std::optional<SessionRecord> = 0;
    virtual auto write_session(SessionRecord const& record) -> bool                 = 0;
    virtual void delete_session(std::string const& id)                              = 0;

    SessionConfig config_;
};

class InMemorySessionStore final : public SessionStore {
public:
    explicit InMemorySessionStore(SessionConfig config)
        : SessionStore(std::move(config)) {}

private:
    auto read_session(std::string const& id) -> std::optional<SessionRecord> override {
        std::lock_guard const lock{mutex_};
        auto                  it = sessions_.find(id);
        if (it == sessions_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    auto write_session(SessionRecord const& record) -> bool override {
        std::lock_guard const lock{mutex_};
        sessions_[record.id] = record;
        return true;
    }

    void delete_session(std::string const& id) override {
        std::lock_guard const lock{mutex_};
        sessions_.erase(id);
    }

    std::unordered_map<std::string, SessionRecord> sessions_;
    mutable std::mutex                             mutex_;
};

struct AssetLocator {
    std::string                                  view;
    std::optional<std::uint64_t>                 revision;
    std::chrono::steady_clock::time_point        updated_at{};
};

std::mutex                                   g_asset_index_mutex;
std::unordered_map<std::string, AssetLocator> g_asset_index;

constexpr std::size_t kMaxApiPayloadBytes = 1024 * 1024; // 1 MiB

constexpr std::string_view kGoogleIssuerPrimary{"https://accounts.google.com"};
constexpr std::string_view kGoogleIssuerLegacy{"accounts.google.com"};
constexpr std::chrono::seconds kGoogleStateTtl{std::chrono::seconds{600}};
constexpr std::chrono::minutes kGoogleJwksTtl{std::chrono::minutes{15}};
constexpr std::chrono::seconds kHttpRequestTimeout{std::chrono::seconds{5}};
constexpr std::chrono::seconds kTokenClockSkew{std::chrono::seconds{60}};
constexpr std::string_view kDemoGoogleSub{"google-user-123"};

struct UrlView {
    std::string scheme;
    std::string host;
    std::string path;
    int         port{0};
    bool        tls{false};
};

struct SimpleHttpResponse {
    int                status{0};
    std::string        body;
    httplib::Headers   headers;
};

std::optional<UrlView> parse_url(std::string_view url) {
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

std::string build_absolute_url(UrlView const& url) {
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

std::string build_query_string(std::vector<std::pair<std::string, std::string>> const& params) {
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

std::string sanitize_redirect_path(std::string_view value) {
    if (value.empty() || value.front() != '/' || (value.size() > 1 && value.substr(0, 2) == "//")) {
        return std::string{"/"};
    }
    std::string sanitized;
    sanitized.reserve(value.size());
    for (char ch : value) {
        if (ch == '\r' || ch == '\n') {
            break;
        }
        sanitized.push_back(ch);
    }
    if (sanitized.empty()) {
        return std::string{"/"};
    }
    return sanitized;
}

std::string base64url_encode(unsigned char const* data, std::size_t length) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string encoded;
    encoded.reserve(((length + 2) / 3) * 4);
    for (std::size_t i = 0; i < length; i += 3) {
        std::size_t remaining = length - i;
        std::uint32_t chunk   = static_cast<std::uint32_t>(data[i]) << 16;
        if (remaining > 1) {
            chunk |= static_cast<std::uint32_t>(data[i + 1]) << 8;
        }
        if (remaining > 2) {
            chunk |= static_cast<std::uint32_t>(data[i + 2]);
        }

        encoded.push_back(kAlphabet[(chunk >> 18) & 0x3F]);
        encoded.push_back(kAlphabet[(chunk >> 12) & 0x3F]);
        if (remaining > 1) {
            encoded.push_back(kAlphabet[(chunk >> 6) & 0x3F]);
        }
        if (remaining > 2) {
            encoded.push_back(kAlphabet[chunk & 0x3F]);
        }
    }
    return encoded;
}

std::string base64url_encode(std::string_view value) {
    auto const* bytes = reinterpret_cast<unsigned char const*>(value.data());
    return base64url_encode(bytes, value.size());
}

std::optional<std::vector<unsigned char>> base64url_decode(std::string_view value) {
    if (value.empty()) {
        return std::vector<unsigned char>{};
    }

    std::string normalized;
    normalized.reserve(((value.size() + 3) / 4) * 4);
    for (char ch : value) {
        if (ch == '-') {
            normalized.push_back('+');
        } else if (ch == '_') {
            normalized.push_back('/');
        } else {
            normalized.push_back(ch);
        }
    }
    while (normalized.size() % 4 != 0) {
        normalized.push_back('=');
    }

    static constexpr std::array<int, 256> kDecodeTable = [] {
        std::array<int, 256> table{};
        table.fill(-1);
        for (int i = 0; i < 26; ++i) {
            table['A' + i] = i;
            table['a' + i] = 26 + i;
        }
        for (int i = 0; i < 10; ++i) {
            table['0' + i] = 52 + i;
        }
        table['+'] = 62;
        table['/'] = 63;
        table['='] = -2;
        return table;
    }();

    std::vector<unsigned char> decoded;
    decoded.reserve((normalized.size() / 4) * 3);
    for (std::size_t i = 0; i < normalized.size(); i += 4) {
        int values[4];
        for (int j = 0; j < 4; ++j) {
            unsigned char ch = static_cast<unsigned char>(normalized[i + j]);
            values[j]         = kDecodeTable[ch];
            if (values[j] == -1) {
                return std::nullopt;
            }
        }
        if (values[0] < 0 || values[1] < 0) {
            return std::nullopt;
        }

        std::uint32_t chunk = (values[0] << 18) | (values[1] << 12);
        decoded.push_back(static_cast<unsigned char>((chunk >> 16) & 0xFF));

        if (values[2] >= 0) {
            chunk |= (values[2] << 6);
            decoded.push_back(static_cast<unsigned char>((chunk >> 8) & 0xFF));
            if (values[3] >= 0) {
                chunk |= values[3];
                decoded.push_back(static_cast<unsigned char>(chunk & 0xFF));
            }
        }
    }

    return decoded;
}

std::string generate_random_token(std::size_t bytes) {
    std::vector<unsigned char> buffer(bytes);
    std::random_device          device;
    for (auto& byte : buffer) {
        byte = static_cast<unsigned char>(device());
    }
    return base64url_encode(buffer.data(), buffer.size());
}

std::string generate_pkce_verifier() {
    // 48 random bytes exceed the 43-character PKCE minimum.
    return generate_random_token(48);
}

std::string compute_code_challenge(std::string_view verifier) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<unsigned char const*>(verifier.data()), verifier.size(), digest);
    return base64url_encode(digest, SHA256_DIGEST_LENGTH);
}

class GoogleAuthStateStore {
public:
    struct Entry {
        std::string                             code_verifier;
        std::string                             redirect_path;
        std::chrono::steady_clock::time_point   created_at{};
    };

    struct IssuedState {
        std::string state;
        Entry       entry;
    };

    auto issue(std::string_view redirect_hint) -> IssuedState {
        IssuedState issued{};
        issued.state             = generate_random_token(32);
        issued.entry.code_verifier = generate_pkce_verifier();
        issued.entry.redirect_path = sanitize_redirect_path(redirect_hint);
        issued.entry.created_at    = Clock::now();
        {
            std::lock_guard const lock{mutex_};
            states_[issued.state] = issued.entry;
            prune_locked(issued.entry.created_at);
        }
        return issued;
    }

    auto take(std::string const& state) -> std::optional<Entry> {
        if (state.empty()) {
            return std::nullopt;
        }
        std::lock_guard const lock{mutex_};
        auto                  now = Clock::now();
        prune_locked(now);
        auto it = states_.find(state);
        if (it == states_.end()) {
            return std::nullopt;
        }
        if ((now - it->second.created_at) > kGoogleStateTtl) {
            states_.erase(it);
            return std::nullopt;
        }
        Entry entry = it->second;
        states_.erase(it);
        return entry;
    }

    std::string last_state_token() const {
        return entry_token_;
    }

private:
    using Clock = std::chrono::steady_clock;

    void prune_locked(Clock::time_point now) {
        for (auto it = states_.begin(); it != states_.end();) {
            if ((now - it->second.created_at) > kGoogleStateTtl) {
                it = states_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::unordered_map<std::string, Entry> states_;
    mutable std::mutex                     mutex_;
};

struct JwksKey {
    std::string                   kid;
    std::vector<unsigned char>    modulus;
    std::vector<unsigned char>    exponent;
};

class GoogleJwksCache {
public:
    void set_endpoint(UrlView endpoint) {
        jwks_endpoint_ = std::move(endpoint);
    }

    auto get_key(std::string const& kid) -> std::optional<JwksKey> {
        auto maybe_key = find_cached_key(kid);
        if (maybe_key) {
            return maybe_key;
        }
        if (!jwks_endpoint_) {
            return std::nullopt;
        }
        auto response = http_get(*jwks_endpoint_);
        if (!response || response->status != 200) {
            return std::nullopt;
        }
        auto parsed_keys = parse_jwks(response->body);
        if (parsed_keys.empty()) {
            return std::nullopt;
        }
        {
            std::lock_guard const lock{mutex_};
            keys_      = std::move(parsed_keys);
            fetched_at_ = Clock::now();
        }
        return find_cached_key(kid);
    }

private:
    using Clock = std::chrono::steady_clock;

    auto parse_jwks(std::string const& body) -> std::vector<JwksKey> {
        std::vector<JwksKey> keys;
        auto                 doc = json::parse(body, nullptr, false);
        if (doc.is_discarded() || !doc.contains("keys")) {
            return keys;
        }
        auto const& jwks = doc["keys"];
        if (!jwks.is_array()) {
            return keys;
        }
        for (auto const& entry : jwks) {
            if (!entry.contains("kid") || !entry.contains("n") || !entry.contains("e")) {
                continue;
            }
            auto kid = entry["kid"].get<std::string>();
            auto n   = entry["n"].get<std::string>();
            auto e   = entry["e"].get<std::string>();
            auto modulus  = base64url_decode(n);
            auto exponent = base64url_decode(e);
            if (!modulus || !exponent || modulus->empty() || exponent->empty()) {
                continue;
            }
            keys.push_back(JwksKey{std::move(kid), std::move(*modulus), std::move(*exponent)});
        }
        return keys;
    }

    auto find_cached_key(std::string const& kid) -> std::optional<JwksKey> {
        std::lock_guard const lock{mutex_};
        auto                  now = Clock::now();
        if (keys_.empty() || (now - fetched_at_) > kGoogleJwksTtl) {
            return std::nullopt;
        }
        for (auto const& key : keys_) {
            if (key.kid == kid) {
                return key;
            }
        }
        return std::nullopt;
    }

    std::optional<UrlView>       jwks_endpoint_;
    std::vector<JwksKey>         keys_;
    Clock::time_point            fetched_at_{};
    std::mutex                   mutex_;
};

struct GoogleIdTokenPayload {
    std::string sub;
    std::string email;
    bool        email_verified{false};
};

std::unique_ptr<httplib::Client> make_http_client(UrlView const& url) {
    std::unique_ptr<httplib::Client> client;
    if (url.tls) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        auto ssl_client = std::make_unique<httplib::SSLClient>(url.host, url.port);
        ssl_client->enable_server_certificate_verification(true);
        client = std::move(ssl_client);
#else
        (void)url;
        return nullptr;
#endif
    } else {
        client = std::make_unique<httplib::Client>(url.host, url.port);
    }
    int timeout_seconds = static_cast<int>(kHttpRequestTimeout.count());
    client->set_connection_timeout(timeout_seconds, 0);
    client->set_read_timeout(timeout_seconds, 0);
    client->set_write_timeout(timeout_seconds, 0);
    client->set_follow_location(false);
    client->set_keep_alive(false);
    return client;
}

std::optional<SimpleHttpResponse> http_post_form(UrlView const& url,
                                                 std::string const& body,
                                                 httplib::Headers   headers = {}) {
    auto client = make_http_client(url);
    if (!client) {
        return std::nullopt;
    }
    auto response = client->Post(url.path.c_str(), headers, body, "application/x-www-form-urlencoded");
    if (!response) {
        return std::nullopt;
    }
    return SimpleHttpResponse{response->status, response->body, response->headers};
}

std::optional<SimpleHttpResponse> http_get(UrlView const& url,
                                           httplib::Headers headers = {}) {
    auto client = make_http_client(url);
    if (!client) {
        return std::nullopt;
    }
    auto response = client->Get(url.path.c_str(), headers);
    if (!response) {
        return std::nullopt;
    }
    return SimpleHttpResponse{response->status, response->body, response->headers};
}

std::optional<std::int64_t> parse_claim_int(json const& claim) {
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

RsaPtr make_rsa(JwksKey const& key) {
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

std::optional<GoogleIdTokenPayload> decode_google_id_token(std::string const& token,
                                                           GoogleJwksCache&     jwks_cache,
                                                           std::string const&   expected_audience) {
    auto first_dot = token.find('.');
    auto second_dot = token.find('.', first_dot == std::string::npos ? first_dot : first_dot + 1);
    if (first_dot == std::string::npos || second_dot == std::string::npos) {
        return std::nullopt;
    }
    auto header_segment  = token.substr(0, first_dot);
    auto payload_segment = token.substr(first_dot + 1, second_dot - first_dot - 1);
    auto signature_segment = token.substr(second_dot + 1);

    auto header_bytes  = base64url_decode(header_segment);
    auto payload_bytes = base64url_decode(payload_segment);
    auto signature     = base64url_decode(signature_segment);
    if (!header_bytes || !payload_bytes || !signature) {
        return std::nullopt;
    }

    auto header_json = json::parse(std::string{header_bytes->begin(), header_bytes->end()}, nullptr, false);
    if (header_json.is_discarded() || !header_json.contains("kid")) {
        return std::nullopt;
    }
    auto kid = header_json["kid"].get<std::string>();
    if (!header_json.contains("alg") || header_json["alg"].get<std::string>() != "RS256") {
        return std::nullopt;
    }

    auto key = jwks_cache.get_key(kid);
    if (!key) {
        return std::nullopt;
    }
    auto rsa = make_rsa(*key);
    if (!rsa) {
        return std::nullopt;
    }

    auto signing_input = header_segment + '.' + payload_segment;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<unsigned char const*>(signing_input.data()), signing_input.size(), digest);
    int verify = RSA_verify(NID_sha256,
                            digest,
                            SHA256_DIGEST_LENGTH,
                            signature->data(),
                            static_cast<unsigned int>(signature->size()),
                            rsa.get());
    if (verify != 1) {
        return std::nullopt;
    }

    auto payload_json = json::parse(std::string{payload_bytes->begin(), payload_bytes->end()}, nullptr, false);
    if (payload_json.is_discarded()) {
        return std::nullopt;
    }

    if (!payload_json.contains("sub") || !payload_json["sub"].is_string()) {
        return std::nullopt;
    }
    if (!payload_json.contains("aud") || !aud_matches(payload_json["aud"], expected_audience)) {
        return std::nullopt;
    }
    if (!payload_json.contains("iss")) {
        return std::nullopt;
    }
    auto issuer = payload_json["iss"].get<std::string>();
    if (issuer != kGoogleIssuerPrimary && issuer != kGoogleIssuerLegacy) {
        return std::nullopt;
    }

    auto exp = payload_json.contains("exp") ? parse_claim_int(payload_json["exp"]) : std::nullopt;
    if (!exp) {
        return std::nullopt;
    }
    auto now_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    if ((*exp + kTokenClockSkew.count()) < now_seconds) {
        return std::nullopt;
    }

    GoogleIdTokenPayload payload{
        .sub = payload_json["sub"].get<std::string>(),
    };
    if (payload_json.contains("email") && payload_json["email"].is_string()) {
        payload.email = payload_json["email"].get<std::string>();
    }
    if (payload_json.contains("email_verified")) {
        if (payload_json["email_verified"].is_boolean()) {
            payload.email_verified = payload_json["email_verified"].get<bool>();
        } else if (payload_json["email_verified"].is_string()) {
            auto flag = payload_json["email_verified"].get<std::string>();
            payload.email_verified = (flag == "true" || flag == "1");
        }
    }
    return payload;
}

std::string make_google_mapping_path(ServeHtmlOptions const& options, std::string const& sub) {
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

auto resolve_google_username(SP::PathSpace&           space,
                             ServeHtmlOptions const& options,
                             std::string const&      sub) -> SP::Expected<std::string> {
    auto mapping_path = make_google_mapping_path(options, sub);
    if (mapping_path.empty()) {
        return std::unexpected(SP::Error{SP::Error::Code::NoSuchPath, "google users root unset"});
    }
    auto mapping = read_optional_value<std::string>(space, mapping_path);
    if (!mapping) {
        return std::unexpected(mapping.error());
    }
    if (!mapping->has_value() || mapping->value().empty()) {
        return std::unexpected(SP::Error{SP::Error::Code::NoObjectFound,
                                         "google user not registered"});
    }
    return mapping->value();
}

enum class RouteMetric : std::size_t {
    Root = 0,
    Healthz,
    Login,
    LoginGoogle,
    LoginGoogleCallback,
    Logout,
    Session,
    Apps,
    Assets,
    ApiOps,
    Events,
    Metrics,
    Count,
};

constexpr std::array<std::pair<RouteMetric, char const*>, static_cast<std::size_t>(RouteMetric::Count)>
    kRouteMetricNames{{
        {RouteMetric::Root, "root"},
        {RouteMetric::Healthz, "healthz"},
        {RouteMetric::Login, "login"},
        {RouteMetric::LoginGoogle, "login_google"},
        {RouteMetric::LoginGoogleCallback, "login_google_callback"},
        {RouteMetric::Logout, "logout"},
        {RouteMetric::Session, "session"},
        {RouteMetric::Apps, "apps"},
        {RouteMetric::Assets, "assets"},
        {RouteMetric::ApiOps, "api_ops"},
        {RouteMetric::Events, "events"},
        {RouteMetric::Metrics, "metrics"},
    }};

constexpr std::size_t kRouteCount = static_cast<std::size_t>(RouteMetric::Count);

struct HistogramSnapshot {
    static constexpr std::size_t kBucketCount = 10;
    std::array<std::uint64_t, kBucketCount>   buckets{};
    std::uint64_t                             count{0};
    std::uint64_t                             sum_micros{0};
};

class Histogram {
public:
    void observe(std::chrono::microseconds value) {
        auto const micros = static_cast<std::uint64_t>(value.count());
        sum_micros_.fetch_add(micros, std::memory_order_relaxed);
        count_.fetch_add(1, std::memory_order_relaxed);
        double const millis = static_cast<double>(micros) / 1000.0;
        for (std::size_t i = 0; i < kLatencyBucketsMs.size(); ++i) {
            if (millis <= kLatencyBucketsMs[i]) {
                buckets_[i].fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        buckets_.back().fetch_add(1, std::memory_order_relaxed);
    }

    auto snapshot() const -> HistogramSnapshot {
        HistogramSnapshot snapshot{};
        for (std::size_t i = 0; i < kLatencyBucketsMs.size(); ++i) {
            snapshot.buckets[i] = buckets_[i].load(std::memory_order_relaxed);
        }
        snapshot.count      = count_.load(std::memory_order_relaxed);
        snapshot.sum_micros = sum_micros_.load(std::memory_order_relaxed);
        return snapshot;
    }

    static auto bucket_boundaries() -> std::array<double, HistogramSnapshot::kBucketCount> const& {
        return kLatencyBucketsMs;
    }

private:
    static constexpr std::array<double, HistogramSnapshot::kBucketCount> kLatencyBucketsMs{
        1.0,   5.0,    20.0,   50.0,   100.0,
        250.0, 500.0,  1000.0, 2500.0, std::numeric_limits<double>::infinity()};

    std::array<std::atomic<std::uint64_t>, HistogramSnapshot::kBucketCount> buckets_{};
    std::atomic<std::uint64_t>                                               count_{0};
    std::atomic<std::uint64_t>                                               sum_micros_{0};
};

struct RateLimitKey {
    std::string scope;
    std::string route;

    auto operator<(RateLimitKey const& other) const -> bool {
        if (scope != other.scope) {
            return scope < other.scope;
        }
        return route < other.route;
    }
};

class MetricsCollector {
public:
    void record_request(RouteMetric route,
                        int         status,
                        std::chrono::microseconds latency) {
        auto const index = static_cast<std::size_t>(route);
        if (index >= routes_.size()) {
            return;
        }
        auto& counters = routes_[index];
        counters.latency.observe(latency);
        counters.total.fetch_add(1, std::memory_order_relaxed);
        int effective_status = status == 0 ? 200 : status;
        if (effective_status >= 400) {
            counters.errors.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void record_auth_failure() {
        auth_failures_.fetch_add(1, std::memory_order_relaxed);
    }

    void record_rate_limit(std::string_view scope, std::string_view route) {
        std::lock_guard const lock{rate_limit_mutex_};
        RateLimitKey          key{std::string{scope}, std::string{route}};
        rate_limit_counts_[std::move(key)] += 1;
    }

    void record_asset_cache_hit() {
        asset_cache_hits_.fetch_add(1, std::memory_order_relaxed);
    }

    void record_asset_cache_miss() {
        asset_cache_misses_.fetch_add(1, std::memory_order_relaxed);
    }

    void record_sse_connection_open() {
        sse_connections_current_.fetch_add(1, std::memory_order_relaxed);
        sse_connections_total_.fetch_add(1, std::memory_order_relaxed);
    }

    void record_sse_connection_close() {
        sse_connections_current_.fetch_sub(1, std::memory_order_relaxed);
    }

    void record_sse_event(std::string_view event_type) {
        std::lock_guard const lock{sse_event_mutex_};
        sse_event_counts_[std::string{event_type}] += 1;
    }

    void record_render_trigger_latency(std::chrono::microseconds latency) {
        render_trigger_latency_.observe(latency);
    }

    auto render_prometheus() const -> std::string {
        metrics_scrapes_.fetch_add(1, std::memory_order_relaxed);
        std::ostringstream out;

        out << "# HELP pathspace_serve_html_request_duration_seconds Request latency histogram\n";
        out << "# TYPE pathspace_serve_html_request_duration_seconds histogram\n";
        auto const& buckets = Histogram::bucket_boundaries();
        for (std::size_t i = 0; i < routes_.size(); ++i) {
            auto const  snapshot = routes_[i].latency.snapshot();
            auto const* name     = kRouteMetricNames[i].second;
            std::uint64_t cumulative = 0;
            for (std::size_t b = 0; b < buckets.size(); ++b) {
                cumulative += snapshot.buckets[b];
                auto boundary = buckets[b];
                out << "pathspace_serve_html_request_duration_seconds_bucket{route=\"" << name
                    << "\",le=\"" << (std::isinf(boundary) ? std::string{"+Inf"}
                                                                   : std::to_string(boundary / 1000.0))
                    << "\"} " << cumulative << "\n";
            }
            double sum_seconds = snapshot.sum_micros / 1'000'000.0;
            out << "pathspace_serve_html_request_duration_seconds_sum{route=\"" << name
                << "\"} " << sum_seconds << "\n";
            out << "pathspace_serve_html_request_duration_seconds_count{route=\"" << name
                << "\"} " << snapshot.count << "\n";
        }

        out << "# HELP pathspace_serve_html_requests_total Total HTTP requests\n";
        out << "# TYPE pathspace_serve_html_requests_total counter\n";
        out << "# HELP pathspace_serve_html_request_errors_total HTTP requests returning >=400\n";
        out << "# TYPE pathspace_serve_html_request_errors_total counter\n";
        for (std::size_t i = 0; i < routes_.size(); ++i) {
            auto const* name = kRouteMetricNames[i].second;
            auto const  total = routes_[i].total.load(std::memory_order_relaxed);
            auto const  errors = routes_[i].errors.load(std::memory_order_relaxed);
            out << "pathspace_serve_html_requests_total{route=\"" << name << "\"} " << total
                << "\n";
            out << "pathspace_serve_html_request_errors_total{route=\"" << name
                << "\"} " << errors << "\n";
        }

        out << "# HELP pathspace_serve_html_sse_connections Current SSE connections\n";
        out << "# TYPE pathspace_serve_html_sse_connections gauge\n";
        out << "pathspace_serve_html_sse_connections "
            << sse_connections_current_.load(std::memory_order_relaxed) << "\n";
        out << "# HELP pathspace_serve_html_sse_connections_total Total SSE connections opened\n";
        out << "# TYPE pathspace_serve_html_sse_connections_total counter\n";
        out << "pathspace_serve_html_sse_connections_total "
            << sse_connections_total_.load(std::memory_order_relaxed) << "\n";

        {
            std::lock_guard const lock{sse_event_mutex_};
            if (!sse_event_counts_.empty()) {
                out << "# HELP pathspace_serve_html_sse_events_total SSE events emitted by type\n";
                out << "# TYPE pathspace_serve_html_sse_events_total counter\n";
                for (auto const& entry : sse_event_counts_) {
                    out << "pathspace_serve_html_sse_events_total{type=\"" << entry.first << "\"} "
                        << entry.second << "\n";
                }
            }
        }

        out << "# HELP pathspace_serve_html_asset_cache_hits_total Asset cache hits (304)\n";
        out << "# TYPE pathspace_serve_html_asset_cache_hits_total counter\n";
        out << "pathspace_serve_html_asset_cache_hits_total "
            << asset_cache_hits_.load(std::memory_order_relaxed) << "\n";
        out << "# HELP pathspace_serve_html_asset_cache_misses_total Asset cache misses\n";
        out << "# TYPE pathspace_serve_html_asset_cache_misses_total counter\n";
        out << "pathspace_serve_html_asset_cache_misses_total "
            << asset_cache_misses_.load(std::memory_order_relaxed) << "\n";

        out << "# HELP pathspace_serve_html_auth_failures_total Authentication failures\n";
        out << "# TYPE pathspace_serve_html_auth_failures_total counter\n";
        out << "pathspace_serve_html_auth_failures_total "
            << auth_failures_.load(std::memory_order_relaxed) << "\n";

        out << "# HELP pathspace_serve_html_render_trigger_latency_seconds Ops enqueue latency\n";
        out << "# TYPE pathspace_serve_html_render_trigger_latency_seconds histogram\n";
        auto render_snapshot = render_trigger_latency_.snapshot();
        std::uint64_t cumulative = 0;
        for (std::size_t b = 0; b < buckets.size(); ++b) {
            cumulative += render_snapshot.buckets[b];
            auto boundary = buckets[b];
            out << "pathspace_serve_html_render_trigger_latency_seconds_bucket{le=\""
                << (std::isinf(boundary) ? std::string{"+Inf"}
                                          : std::to_string(boundary / 1000.0))
                << "\"} " << cumulative << "\n";
        }
        out << "pathspace_serve_html_render_trigger_latency_seconds_sum "
            << (render_snapshot.sum_micros / 1'000'000.0) << "\n";
        out << "pathspace_serve_html_render_trigger_latency_seconds_count "
            << render_snapshot.count << "\n";

        {
            std::lock_guard const lock{rate_limit_mutex_};
            if (!rate_limit_counts_.empty()) {
                out << "# HELP pathspace_serve_html_rate_limit_rejections_total Rate-limited requests\n";
                out << "# TYPE pathspace_serve_html_rate_limit_rejections_total counter\n";
                for (auto const& entry : rate_limit_counts_) {
                    out << "pathspace_serve_html_rate_limit_rejections_total{scope=\""
                        << entry.first.scope << "\",route=\"" << entry.first.route << "\"} "
                        << entry.second << "\n";
                }
            }
        }

        out << "# HELP pathspace_serve_html_metrics_scrapes_total Metrics scrapes\n";
        out << "# TYPE pathspace_serve_html_metrics_scrapes_total counter\n";
        out << "pathspace_serve_html_metrics_scrapes_total "
            << metrics_scrapes_.load(std::memory_order_relaxed) << "\n";

        return out.str();
    }

    auto snapshot_json() const -> json {
        json snapshot;
        snapshot["captured_at"] = format_timestamp(std::chrono::system_clock::now());

        json request_stats;
        for (std::size_t i = 0; i < routes_.size(); ++i) {
            auto const* name      = kRouteMetricNames[i].second;
            auto const  total     = routes_[i].total.load(std::memory_order_relaxed);
            auto const  errors    = routes_[i].errors.load(std::memory_order_relaxed);
            auto const  hist      = routes_[i].latency.snapshot();
            double      avg_ms    = hist.count == 0
                                        ? 0.0
                                        : static_cast<double>(hist.sum_micros) / 1000.0
                                              / static_cast<double>(hist.count);
            request_stats[name] = json{{"total", total},
                                       {"errors", errors},
                                       {"avg_ms", avg_ms}};
        }
        snapshot["requests"] = std::move(request_stats);

        snapshot["sse"] = json{{"connections_current",
                                 sse_connections_current_.load(std::memory_order_relaxed)},
                                {"connections_total",
                                 sse_connections_total_.load(std::memory_order_relaxed)}};

        snapshot["assets"] = json{{"cache_hits", asset_cache_hits_.load(std::memory_order_relaxed)},
                                   {"cache_misses", asset_cache_misses_.load(std::memory_order_relaxed)}};

        snapshot["auth_failures"] = auth_failures_.load(std::memory_order_relaxed);

        auto render_hist = render_trigger_latency_.snapshot();
        double render_avg_ms = render_hist.count == 0
                                   ? 0.0
                                   : static_cast<double>(render_hist.sum_micros) / 1000.0
                                         / static_cast<double>(render_hist.count);
        snapshot["render_triggers"] = json{{"count", render_hist.count},
                                            {"avg_ms", render_avg_ms}};

        json rate_limits = json::array();
        {
            std::lock_guard const lock{rate_limit_mutex_};
            for (auto const& entry : rate_limit_counts_) {
                rate_limits.push_back(json{{"scope", entry.first.scope},
                                           {"route", entry.first.route},
                                           {"count", entry.second}});
            }
        }
        snapshot["rate_limits"] = std::move(rate_limits);

        json sse_events = json::array();
        {
            std::lock_guard const lock{sse_event_mutex_};
            for (auto const& entry : sse_event_counts_) {
                sse_events.push_back(json{{"type", entry.first}, {"count", entry.second}});
            }
        }
        snapshot["sse_events"] = std::move(sse_events);

        return snapshot;
    }

private:
    struct RouteCounters {
        Histogram                         latency;
        std::atomic<std::uint64_t>        total{0};
        std::atomic<std::uint64_t>        errors{0};
    };

    std::array<RouteCounters, kRouteCount> routes_{};
    std::atomic<std::int64_t>              sse_connections_current_{0};
    std::atomic<std::uint64_t>             sse_connections_total_{0};
    std::atomic<std::uint64_t>             asset_cache_hits_{0};
    std::atomic<std::uint64_t>             asset_cache_misses_{0};
    std::atomic<std::uint64_t>             auth_failures_{0};
    mutable std::atomic<std::uint64_t>     metrics_scrapes_{0};
    Histogram                              render_trigger_latency_;

    mutable std::mutex                    rate_limit_mutex_;
    std::map<RateLimitKey, std::uint64_t> rate_limit_counts_;
    mutable std::mutex                    sse_event_mutex_;
    std::map<std::string, std::uint64_t>  sse_event_counts_;
};

class RequestMetricsScope {
public:
    RequestMetricsScope(MetricsCollector& metrics, RouteMetric route, httplib::Response& res)
        : metrics_{metrics}
        , route_{route}
        , response_{res}
        , start_{std::chrono::steady_clock::now()} {}

    ~RequestMetricsScope() {
        auto duration = std::chrono::steady_clock::now() - start_;
        metrics_.record_request(route_, response_.status, std::chrono::duration_cast<std::chrono::microseconds>(duration));
    }

private:
    MetricsCollector&                         metrics_;
    RouteMetric                               route_;
    httplib::Response&                        response_;
    std::chrono::steady_clock::time_point     start_;
};

class TokenBucketRateLimiter {
public:
    using Clock = std::chrono::steady_clock;

    TokenBucketRateLimiter(std::int64_t per_minute, std::int64_t burst)
        : capacity_{static_cast<double>(std::max<std::int64_t>(burst, 0))}
        , refill_per_second_{per_minute <= 0 ? 0.0
                                             : static_cast<double>(per_minute) / 60.0} {}

    auto allow(std::string_view key, Clock::time_point now = Clock::now()) -> bool {
        if (!enabled()) {
            return true;
        }

        std::string normalized_key = key.empty() ? std::string{"<unknown>"}
                                                 : std::string{key};

        std::lock_guard const lock{mutex_};
        auto& bucket = buckets_[normalized_key];

        if (bucket.last_refill.time_since_epoch().count() == 0) {
            bucket.tokens = capacity_;
            bucket.last_refill = now;
        } else if (now > bucket.last_refill) {
            auto const delta = std::chrono::duration<double>(now - bucket.last_refill).count();
            bucket.tokens = std::min(capacity_, bucket.tokens + delta * refill_per_second_);
            bucket.last_refill = now;
        }

        bucket.last_used = now;
        if (bucket.tokens < 1.0) {
            prune_locked(now);
            return false;
        }

        bucket.tokens -= 1.0;
        prune_locked(now);
        return true;
    }

private:
    struct Bucket {
        double             tokens{0.0};
        Clock::time_point  last_refill{};
        Clock::time_point  last_used{};
    };

    auto enabled() const -> bool {
        return capacity_ > 0.0 && refill_per_second_ > 0.0;
    }

    void prune_locked(Clock::time_point now) {
        if (++operations_since_prune_ < 512) {
            return;
        }
        operations_since_prune_ = 0;
        auto const max_idle = std::chrono::minutes{10};
        for (auto it = buckets_.begin(); it != buckets_.end();) {
            if ((now - it->second.last_used) > max_idle || buckets_.size() > 4096) {
                it = buckets_.erase(it);
            } else {
                ++it;
            }
        }
    }

    double                                     capacity_{0.0};
    double                                     refill_per_second_{0.0};
    std::unordered_map<std::string, Bucket>    buckets_;
    std::size_t                                operations_since_prune_{0};
    std::mutex                                 mutex_;
};

bool is_identifier(std::string_view value) {
    if (value.empty() || value == "." || value == "..") {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.';
    });
}

bool is_asset_component(std::string_view value) {
    if (value.empty() || value == "." || value == "..") {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.';
    });
}

bool is_asset_path(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    while (!value.empty() && value.front() == '/') {
        value.remove_prefix(1);
    }
    if (value.empty()) {
        return false;
    }
    std::size_t offset = 0;
    while (offset < value.size()) {
        auto next = value.find('/', offset);
        auto segment = value.substr(offset, next == std::string_view::npos ? std::string_view::npos
                                                                          : next - offset);
        if (!is_asset_component(segment)) {
            return false;
        }
        if (next == std::string_view::npos) {
            break;
        }
        offset = next + 1;
    }
    return true;
}

std::string make_asset_index_key(std::string_view app, std::string_view asset_path) {
    std::string key;
    key.reserve(app.size() + asset_path.size() + 1);
    key.append(app);
    key.push_back('\x1f');
    key.append(asset_path);
    return key;
}

std::optional<AssetLocator> lookup_asset_locator(std::string const& app,
                                                 std::string const& asset_path) {
    std::lock_guard const lock{g_asset_index_mutex};
    auto const key = make_asset_index_key(app, asset_path);
    auto       it  = g_asset_index.find(key);
    if (it == g_asset_index.end()) {
        return std::nullopt;
    }
    return it->second;
}

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

std::string_view trim_view(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return value;
}

std::string get_client_address(httplib::Request const& req) {
    auto forwarded = req.get_header_value("X-Forwarded-For");
    if (!forwarded.empty()) {
        std::string_view view{forwarded};
        auto             comma = view.find(',');
        auto             token = trim_view(view.substr(0, comma));
        if (!token.empty()) {
            return std::string{token};
        }
    }
    if (!req.remote_addr.empty()) {
        return req.remote_addr;
    }
    return std::string{"unknown"};
}

std::string abbreviate_token(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    constexpr std::size_t kPrefix = 12;
    if (value.size() <= kPrefix) {
        return std::string{value};
    }
    std::string shortened{value.substr(0, kPrefix)};
    shortened.append("...");
    return shortened;
}

std::optional<std::string> read_cookie_value(httplib::Request const& req,
                                             std::string const&      name) {
    auto cookie_header = req.get_header_value("Cookie");
    if (cookie_header.empty()) {
        return std::nullopt;
    }

    std::string_view remaining{cookie_header};
    while (!remaining.empty()) {
        auto separator = remaining.find(';');
        auto segment = trim_view(remaining.substr(0, separator));
        remaining = separator == std::string_view::npos
                        ? std::string_view{}
                        : remaining.substr(separator + 1);

        auto equals = segment.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }

        auto key = trim_view(segment.substr(0, equals));
        if (key == name) {
            auto value = trim_view(segment.substr(equals + 1));
            return std::string{value};
        }
    }

    return std::nullopt;
}

std::string build_cookie_header(std::string const&                         name,
                                std::string const&                         value,
                                std::optional<std::chrono::seconds> const& max_age,
                                bool                                        http_only = true) {
    std::ostringstream header;
    header << name << '=' << value << "; Path=/; SameSite=Lax";
    if (http_only) {
        header << "; HttpOnly";
    }
    if (max_age.has_value()) {
        header << "; Max-Age=" << max_age->count();
    }
    return header.str();
}

void write_json_response(httplib::Response& res,
                         json const&        payload,
                         int                status,
                         bool               no_store = false) {
    res.status = status;
    res.set_content(payload.dump(), "application/json; charset=utf-8");
    if (no_store) {
        res.set_header("Cache-Control", "no-store");
    }
}

void respond_unauthorized(httplib::Response& res) {
    write_json_response(res,
                        json{{"error", "unauthorized"},
                             {"message", "Authentication required"}},
                        401,
                        true);
}

void respond_bad_request(httplib::Response& res, std::string_view message) {
    write_json_response(res,
                        json{{"error", "bad_request"},
                             {"message", message}},
                        400,
                        true);
}

void respond_server_error(httplib::Response& res, std::string_view message) {
    write_json_response(res,
                        json{{"error", "internal"},
                             {"message", message}},
                        500);
}

void respond_payload_too_large(httplib::Response& res) {
    write_json_response(res,
                        json{{"error", "payload_too_large"},
                             {"message", "Request body exceeds 1 MiB limit"}},
                        413,
                        true);
}

void respond_unsupported_media_type(httplib::Response& res) {
    write_json_response(res,
                        json{{"error", "unsupported_media_type"},
                             {"message", "Expected Content-Type: application/json"}},
                        415,
                        true);
}

void respond_rate_limited(httplib::Response& res) {
    write_json_response(res,
                        json{{"error", "rate_limited"},
                             {"message", "Too many requests"}},
                        429,
                        true);
}

std::optional<std::string> generate_bcrypt_hash(std::string_view password,
                                                int                work_factor) {
    std::array<char, BCRYPT_HASHSIZE> salt{};
    std::array<char, BCRYPT_HASHSIZE> hash{};

    auto clamped_factor = std::clamp(work_factor, 4, 31);
    if (bcrypt_gensalt(clamped_factor, salt.data()) != 0) {
        return std::nullopt;
    }

    std::string password_storage{password};
    if (bcrypt_hashpw(password_storage.c_str(), salt.data(), hash.data()) != 0) {
        return std::nullopt;
    }

    return std::string{hash.data()};
}

template <typename T>
auto read_optional_value(SP::PathSpace const& space, std::string const& path)
    -> SP::Expected<std::optional<T>> {
    auto value = space.read<T, std::string>(path);
    if (value) {
        return std::optional<T>{*value};
    }
    auto const& error = value.error();
    if (error.code == SP::Error::Code::NoObjectFound || error.code == SP::Error::Code::NoSuchPath) {
        return std::optional<T>{};
    }
    return std::unexpected(error);
}

template <typename T>
auto clear_queue(SP::PathSpace& space, std::string const& path) -> SP::Expected<void> {
    while (true) {
        auto taken = space.take<T>(path);
        if (taken) {
            continue;
        }
        auto const& error = taken.error();
        if (error.code == SP::Error::Code::NoObjectFound
            || error.code == SP::Error::Code::NoSuchPath) {
            break;
        }
        return std::unexpected(error);
    }
    return {};
}

template <typename T>
auto replace_single_value(SP::PathSpace& space, std::string const& path, T const& value)
    -> SP::Expected<void> {
    if (auto cleared = clear_queue<T>(space, path); !cleared) {
        return cleared;
    }
    auto result = space.insert(path, value);
    if (!result.errors.empty()) {
        return std::unexpected(result.errors.front());
    }
    return {};
}

class PathSpaceSessionStore final : public SessionStore {
public:
    PathSpaceSessionStore(SessionConfig config,
                          ServeHtmlSpace& space,
                          std::string     root_path)
        : SessionStore(std::move(config))
        , space_{space}
        , root_path_{std::move(root_path)} {}

private:
    static auto to_epoch_seconds(Clock::time_point time) -> std::int64_t {
        return std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
    }

    static auto from_epoch_seconds(std::int64_t value) -> Clock::time_point {
        return Clock::time_point{std::chrono::seconds{value}};
    }

    auto build_session_path(std::string const& id) const -> std::string {
        if (root_path_.empty() || root_path_ == "/") {
            std::string path{"/"};
            path.append(id);
            return path;
        }
        std::string path = root_path_;
        path.push_back('/');
        path.append(id);
        return path;
    }

    auto read_session(std::string const& id) -> std::optional<SessionRecord> override {
        auto const path  = build_session_path(id);
        auto       value = space_.read<std::string, std::string>(path);
        if (!value) {
            auto const& error = value.error();
            if (error.code == SP::Error::Code::NoObjectFound
                || error.code == SP::Error::Code::NoSuchPath) {
                return std::nullopt;
            }
            std::cerr << "[serve_html] failed to read session " << id << ": "
                      << SP::describeError(error) << "\n";
            return std::nullopt;
        }

        auto payload = json::parse(*value, nullptr, false);
        if (payload.is_discarded()) {
            std::cerr << "[serve_html] session payload for " << id << " is invalid JSON\n";
            (void)clear_queue<std::string>(space_, path);
            return std::nullopt;
        }
        if (!payload.contains("username") || !payload["username"].is_string()
            || !payload.contains("created_at")
            || !payload["created_at"].is_number_integer()
            || !payload.contains("last_seen") || !payload["last_seen"].is_number_integer()) {
            std::cerr << "[serve_html] session payload for " << id
                      << " missing required fields\n";
            (void)clear_queue<std::string>(space_, path);
            return std::nullopt;
        }

        SessionRecord record{};
        record.id        = id;
        record.username  = payload["username"].get<std::string>();
        record.created_at = from_epoch_seconds(payload["created_at"].get<std::int64_t>());
        record.last_seen  = from_epoch_seconds(payload["last_seen"].get<std::int64_t>());
        return record;
    }

    auto write_session(SessionRecord const& record) -> bool override {
        auto const path = build_session_path(record.id);
        json       payload{{"version", 1},
                           {"username", record.username},
                           {"created_at", to_epoch_seconds(record.created_at)},
                           {"last_seen", to_epoch_seconds(record.last_seen)}};
        auto serialized = payload.dump();
        auto result = replace_single_value<std::string>(space_, path, serialized);
        if (!result) {
            std::cerr << "[serve_html] failed to persist session " << record.id << ": "
                      << SP::describeError(result.error()) << "\n";
            return false;
        }
        return true;
    }

    void delete_session(std::string const& id) override {
        auto const path = build_session_path(id);
        auto       result = clear_queue<std::string>(space_, path);
        if (!result) {
            std::cerr << "[serve_html] failed to clear session " << id << ": "
                      << SP::describeError(result.error()) << "\n";
        }
    }

    ServeHtmlSpace& space_;
    std::string     root_path_;
};

auto make_session_store(ServeHtmlSpace&          space,
                        ServeHtmlOptions const& options,
                        SessionConfig const&    config) -> std::unique_ptr<SessionStore> {
    auto backend = options.session_store_backend;
    if (backend == "pathspace") {
        return std::make_unique<PathSpaceSessionStore>(config, space, options.session_store_path);
    }
    if (backend != "memory") {
        std::cerr << "[serve_html] unsupported session store backend '" << backend
                  << "', defaulting to in-memory\n";
    }
    return std::make_unique<InMemorySessionStore>(config);
}

struct HtmlPayload {
    std::string                dom;
    std::optional<std::string> css;
    std::optional<std::string> js;
    std::optional<std::string> commands;
    std::optional<std::uint64_t> revision;
    std::vector<std::string>   asset_manifest;
};

void record_asset_manifest(std::string const& app,
                           std::string const& view,
                           HtmlPayload const& payload) {
    if (payload.asset_manifest.empty()) {
        return;
    }
    auto now = std::chrono::steady_clock::now();
    std::lock_guard const lock{g_asset_index_mutex};
    for (auto const& logical : payload.asset_manifest) {
        if (!is_asset_path(logical)) {
            continue;
        }
        AssetLocator locator{};
        locator.view       = view;
        locator.revision   = payload.revision;
        locator.updated_at = now;
        g_asset_index[make_asset_index_key(app, logical)] = std::move(locator);
    }
}

auto load_html_payload(SP::PathSpace const& space, std::string const& html_base)
    -> SP::Expected<HtmlPayload> {
    HtmlPayload payload{};
    auto dom_value = space.read<std::string, std::string>(html_base + "/dom");
    if (!dom_value) {
        return std::unexpected(dom_value.error());
    }
    payload.dom = *dom_value;

    auto css_value = read_optional_value<std::string>(space, html_base + "/css");
    if (!css_value) {
        return std::unexpected(css_value.error());
    }
    payload.css = std::move(*css_value);

    auto js_value = read_optional_value<std::string>(space, html_base + "/js");
    if (!js_value) {
        return std::unexpected(js_value.error());
    }
    payload.js = std::move(*js_value);

    auto commands_value = read_optional_value<std::string>(space, html_base + "/commands");
    if (!commands_value) {
        return std::unexpected(commands_value.error());
    }
    payload.commands = std::move(*commands_value);

    auto revision_value = read_optional_value<std::uint64_t>(space, html_base + "/revision");
    if (!revision_value) {
        return std::unexpected(revision_value.error());
    }
    payload.revision = std::move(*revision_value);

    auto manifest_value = read_optional_value<std::vector<std::string>>(space,
                                                                        html_base + "/assets/manifest");
    if (!manifest_value) {
        return std::unexpected(manifest_value.error());
    }
    if (manifest_value->has_value()) {
        payload.asset_manifest = std::move(**manifest_value);
    }

    return payload;
}

std::string make_target_base(ServeHtmlOptions const& options,
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

std::string make_html_base(ServeHtmlOptions const& options,
                           std::string_view app,
                           std::string_view view) {
    auto base = make_target_base(options, app, view);
    base.append("/output/v1/html");
    return base;
}

std::string make_common_base(ServeHtmlOptions const& options,
                             std::string_view     app,
                             std::string_view     view) {
    auto base = make_target_base(options, app, view);
    base.append("/output/v1/common");
    return base;
}

std::string make_diagnostics_path(ServeHtmlOptions const& options,
                                  std::string_view     app,
                                  std::string_view     view) {
    auto base = make_target_base(options, app, view);
    base.append("/diagnostics/errors/live");
    return base;
}

std::string make_watch_glob(ServeHtmlOptions const& options,
                            std::string_view     app,
                            std::string_view     view) {
    auto base = make_target_base(options, app, view);
    base.append("/**");
    return base;
}

std::string make_app_route(std::string_view app, std::string_view view) {
    std::string route = "/apps/";
    route.append(app);
    route.push_back('/');
    route.append(view);
    return route;
}

std::string make_app_root_path(ServeHtmlOptions const& options, std::string_view app) {
    std::string root = options.apps_root;
    root.append("/");
    root.append(app);
    return root;
}

std::string make_ops_queue_path(ServeHtmlOptions const& options,
                                std::string_view     app,
                                std::string_view     op) {
    auto root = make_app_root_path(options, app);
    root.append("/ops/");
    root.append(op);
    root.append("/inbox/queue");
    return root;
}

std::string make_events_url(std::string_view app, std::string_view view) {
    auto route = make_app_route(app, view);
    route.append("/events");
    return route;
}

std::string make_payload_url(std::string_view app, std::string_view view) {
    auto route = make_app_route(app, view);
    route.append("?format=json");
    return route;
}

std::string make_user_base(ServeHtmlOptions const& options, std::string_view username) {
    std::string base = options.users_root;
    base.append("/");
    base.append(username);
    return base;
}

std::string make_user_password_path(ServeHtmlOptions const& options,
                                    std::string_view       username) {
    auto base = make_user_base(options, username);
    base.append("/password_bcrypt");
    return base;
}

std::string escape_script_payload(std::string const& text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i + 8 < text.size() && text.compare(i, 9, "</script>") == 0) {
            escaped.append("<\\/script>");
            i += 8;
        } else {
            escaped.push_back(text[i]);
        }
    }
    return escaped;
}

std::string escape_html_attribute(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
        case '&':
            escaped.append("&amp;");
            break;
        case '"':
            escaped.append("&quot;");
            break;
        case '\'':
            escaped.append("&#39;");
            break;
        case '<':
            escaped.append("&lt;");
            break;
        case '>':
            escaped.append("&gt;");
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}

constexpr char kLiveReloadScript[] = R"JS((() => {
  const shell = document.getElementById('pathspace-html-shell');
  if (!shell || typeof window === 'undefined') {
    return;
  }
  const supportsSse = typeof window.EventSource !== 'undefined' && typeof fetch !== 'undefined';
  const status = document.createElement('div');
  status.id = 'pathspace-live-status';
  status.textContent = 'Connecting…';
  document.body.appendChild(status);

  function setStatus(text, variant) {
    status.textContent = text;
    status.dataset.variant = variant;
  }

  const diagBanner = document.createElement('div');
  diagBanner.id = 'pathspace-diagnostics-banner';
  diagBanner.style.display = 'none';
  document.body.appendChild(diagBanner);

  const state = {
    eventSource: null,
    reconnectDelay: 2000,
    revision: Number(shell.dataset.revision || '0') || 0,
    payloadUrl: shell.dataset.payloadUrl || '',
    eventsUrl: shell.dataset.eventsUrl || '',
    pending: null,
    lastInlineScript: (document.getElementById('pathspace-inline-js-source') || {}).textContent || ''
  };

  function updateDiagnostic(payload) {
    if (!payload || !payload.active) {
      diagBanner.style.display = 'none';
      diagBanner.textContent = '';
      diagBanner.dataset.severity = '';
      return;
    }
    const severity = payload.severity || 'info';
    diagBanner.style.display = 'block';
    diagBanner.dataset.severity = severity;
    const parts = [];
    if (payload.message) {
      parts.push(payload.message);
    }
    if (payload.detail) {
      parts.push(payload.detail);
    }
    diagBanner.textContent = parts.join(' — ') || 'Renderer diagnostic';
  }

  function runInlineScript(source) {
    if (!source || source === state.lastInlineScript) {
      return;
    }
    state.lastInlineScript = source;
    const script = document.createElement('script');
    script.type = 'module';
    script.textContent = source;
    document.body.appendChild(script);
    script.remove();
  }

  function applyPayload(data) {
    if (!data) {
      return;
    }
    const domHost = document.getElementById('pathspace-html-dom');
    if (domHost && typeof data.dom === 'string') {
      domHost.innerHTML = data.dom;
    }
    const styleTag = document.getElementById('pathspace-html-inline-style');
    if (styleTag && typeof data.css === 'string') {
      styleTag.textContent = data.css;
    }
    const commandsTag = document.getElementById('pathspace-html-commands');
    if (commandsTag && typeof data.commands === 'string') {
      commandsTag.textContent = data.commands;
    }
    const jsSource = document.getElementById('pathspace-inline-js-source');
    if (jsSource && typeof data.js === 'string') {
      jsSource.textContent = data.js;
    }
    if (typeof data.js === 'string' && data.js.length > 0) {
      runInlineScript(data.js);
    }
    if (typeof data.revision === 'number' && !Number.isNaN(data.revision)) {
      state.revision = data.revision;
      shell.dataset.revision = String(data.revision);
    }
  }

  async function fetchPayload() {
    if (!state.payloadUrl) {
      return;
    }
    if (state.pending) {
      return state.pending;
    }
    const url = state.payloadUrl + (state.payloadUrl.includes('?') ? '&' : '?') + 't=' + Date.now();
    state.pending = fetch(url, {
      credentials: 'same-origin',
      cache: 'no-store',
      headers: { 'Accept': 'application/json' }
    })
      .then((response) => {
        if (!response.ok) {
          throw new Error('HTTP ' + response.status);
        }
        return response.json();
      })
      .then((payload) => {
        applyPayload(payload);
        setStatus('Live updates', 'ok');
      })
      .catch((error) => {
        console.error('[pathspace_serve_html] refresh failed', error);
        setStatus('Update failed', 'error');
      })
      .finally(() => {
        state.pending = null;
      });
    return state.pending;
  }

  if (!supportsSse) {
    setStatus('Live updates unavailable', 'error');
    return;
  }

  function scheduleReconnect() {
    const delay = state.reconnectDelay;
    state.reconnectDelay = Math.min(state.reconnectDelay * 2, 15000);
    setTimeout(connectStream, delay);
  }

  function connectStream() {
    if (!state.eventsUrl) {
      return;
    }
    if (state.eventSource) {
      try {
        state.eventSource.close();
      } catch (error) {
        (void)error;
      }
    }
    setStatus('Connecting…', 'connecting');
    const source = new EventSource(state.eventsUrl);
    state.eventSource = source;
    source.addEventListener('frame', (event) => {
      state.reconnectDelay = 2000;
      setStatus('Live updates', 'ok');
      try {
        const payload = JSON.parse(event.data);
        const revision = Number(payload.revision || 0);
        if (revision && revision > state.revision) {
          fetchPayload();
        }
      } catch (error) {
        console.error('[pathspace_serve_html] frame parse failed', error);
      }
    });
    source.addEventListener('reload', () => {
      window.location.reload();
    });
    source.addEventListener('diagnostic', (event) => {
      try {
        updateDiagnostic(JSON.parse(event.data));
      } catch (error) {
        console.error('[pathspace_serve_html] diagnostic parse failed', error);
      }
    });
    source.onerror = () => {
      setStatus('Reconnecting…', 'error');
      try {
        source.close();
      } catch (error) {
        (void)error;
      }
      state.eventSource = null;
      scheduleReconnect();
    };
  }

  connectStream();
})();)JS";

std::string build_response_body(HtmlPayload const& payload,
                                std::string_view  app,
                                std::string_view  view) {
    std::string app_str{app};
    std::string view_str{view};
    auto        revision       = payload.revision.value_or(0);
    auto        events_url     = make_events_url(app_str, view_str);
    auto        payload_url    = make_payload_url(app_str, view_str);
    std::string commands_text  = payload.commands.value_or(std::string{});
    std::string js_text        = payload.js.value_or(std::string{});
    std::string css_text       = payload.css.value_or(std::string{});
    auto        escaped_app    = escape_html_attribute(app_str);
    auto        escaped_view   = escape_html_attribute(view_str);
    auto        escaped_events = escape_html_attribute(events_url);
    auto        escaped_payload = escape_html_attribute(payload_url);
    auto        revision_attr    = escape_html_attribute(std::to_string(revision));

    std::string body;
    body.reserve(payload.dom.size() + css_text.size() + commands_text.size() + js_text.size() + 2048);
    body.append("<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\"/>\n<title>PathSpace View</title>\n");
    body.append(
        "<style id=\"pathspace-live-style\">\n"
        "#pathspace-html-dom[data-pathspace-dom=\"1\"]{display:contents;}\n"
        "#pathspace-live-status{position:fixed;right:16px;bottom:16px;padding:6px 12px;border-radius:999px;font-family:system-ui,sans-serif;font-size:12px;background:rgba(11,27,43,0.85);color:#f5faff;z-index:2147483000;}\n"
        "#pathspace-live-status[data-variant=\"error\"]{background:#a02128;}\n"
        "#pathspace-live-status[data-variant=\"connecting\"]{background:#215ba0;}\n"
        "#pathspace-diagnostics-banner{position:fixed;top:0;left:0;right:0;padding:10px 16px;font-family:system-ui,sans-serif;font-size:14px;color:#fff;background:#a02128;z-index:2147483646;box-shadow:0 2px 16px rgba(0,0,0,0.35);display:none;}\n"
        "#pathspace-diagnostics-banner[data-severity=\"warning\"]{background:#b26a00;}\n"
        "#pathspace-diagnostics-banner[data-severity=\"info\"]{background:#215ba0;}\n"
        "#pathspace-diagnostics-banner[data-severity=\"fatal\"]{background:#6a041b;}\n"
        "</style>\n");
    body.append("<style id=\"pathspace-html-inline-style\">\n");
    body.append(css_text);
    body.append("\n</style>\n");
    body.append("</head>\n<body>\n");

    body.append("<div id=\"pathspace-html-shell\" data-pathspace-shell=\"1\" data-app=\"");
    body.append(escaped_app);
    body.append("\" data-view=\"");
    body.append(escaped_view);
    body.append("\" data-events-url=\"");
    body.append(escaped_events);
    body.append("\" data-payload-url=\"");
    body.append(escaped_payload);
    body.append("\" data-revision=\"");
    body.append(revision_attr);
    body.append("\">\n<div id=\"pathspace-html-dom\" data-pathspace-dom=\"1\">\n");
    body.append(payload.dom);
    body.append("\n</div>\n</div>\n");

    body.append("<script type=\"application/json\" id=\"pathspace-html-commands\">");
    body.append(escape_script_payload(commands_text));
    body.append("</script>\n");

    body.append("<script type=\"application/json\" id=\"pathspace-inline-js-source\">");
    body.append(escape_script_payload(js_text));
    body.append("</script>\n");

    if (!js_text.empty()) {
        body.append("<script>\n");
        body.append(escape_script_payload(js_text));
        body.append("\n</script>\n");
    }

    body.append("<script type=\"module\" id=\"pathspace-html-live\">\n");
    body.append(kLiveReloadScript);
    body.append("\n</script>\n");
    body.append("</body>\n</html>\n");
    return body;
}

constexpr std::string_view kDemoUser{"demo"};
constexpr std::string_view kDemoPassword{"demo"};
constexpr std::string_view kDemoApp{"demo_web"};
constexpr std::string_view kDemoView{"gallery"};
constexpr std::string_view kDemoAssetPath{"images/demo-badge.txt"};

void seed_demo_credentials(SP::PathSpace& space, ServeHtmlOptions const& options) {

    auto hash = generate_bcrypt_hash(kDemoPassword, 10);
    if (!hash) {
        std::cerr << "[serve_html] Failed to generate bcrypt hash for demo user\n";
        return;
    }

    auto password_path = make_user_password_path(options, kDemoUser);
    auto result = space.insert(password_path, *hash);
    if (!result.errors.empty()) {
        std::cerr << "[serve_html] Failed to seed demo credentials: "
                  << SP::describeError(result.errors.front()) << "\n";
    } else {
        std::cout << "[serve_html] Seeded demo credentials (user 'demo', password 'demo')\n";
    }
}

bool EnsureUserPassword(ServeHtmlSpace&           space,
                        ServeHtmlOptions const&  options,
                        std::string const&       username,
                        std::string const&       password,
                        int                      work_factor) {
    auto hash = generate_bcrypt_hash(password, work_factor);
    if (!hash) {
        std::cerr << "[serve_html] Failed to generate bcrypt hash for user '" << username << "'\n";
        return false;
    }
    auto password_path = make_user_password_path(options, username);
    auto result = space.insert(password_path, *hash);
    if (!result.errors.empty()) {
        std::cerr << "[serve_html] Failed to write credentials for user '"
                  << username << "': " << SP::describeError(result.errors.front()) << "\n";
        return false;
    }
    return true;
}

void seed_demo_application(SP::PathSpace& space, ServeHtmlOptions const& options) {
    auto html_base   = make_html_base(options, kDemoApp, kDemoView);
    auto common_base = make_common_base(options, kDemoApp, kDemoView);
    auto diag_path   = make_diagnostics_path(options, kDemoApp, kDemoView);

    auto insert_string = [&](std::string const& path, std::string value) {
        auto result = space.insert(path, std::move(value));
        if (!result.errors.empty()) {
            std::cerr << "[serve_html] Failed to seed " << path << ": "
                      << SP::describeError(result.errors.front()) << "\n";
        }
    };

    insert_string(html_base + "/dom",
                  R"(<main style="display:flex;min-height:100vh;align-items:center;justify-content:center;background:#0b1b2b;color:#f5faff;font-family:system-ui,sans-serif;">
<section style="text-align:center;padding:32px;border-radius:16px;background:rgba(255,255,255,0.05);box-shadow:0 12px 40px rgba(0,0,0,0.35);">
<p style="letter-spacing:0.2em;font-size:0.8rem;text-transform:uppercase;color:#9bb9d3;margin-bottom:8px;">PathSpace demo</p>
<h1 style="font-size:2.5rem;margin:0 0 16px;">widgets gallery</h1>
<p style="font-size:1.1rem;margin:0 0 24px;color:#d4e2f2;">HTML adapter output hosted by the prototype web server.</p>
<button style="font-size:1rem;padding:12px 24px;border-radius:999px;border:none;background:#2f7cf0;color:white;cursor:pointer;">Reload demo</button>
</section></main>)");

    insert_string(html_base + "/css",
                  "@media (max-width: 520px) { main section { margin: 16px; padding: 24px; } }\n"
                  "button:hover { background:#3d8bff; }\n");

    insert_string(html_base + "/js",
                  "document.addEventListener('DOMContentLoaded', () => {\n"
                  "  const button = document.querySelector('button');\n"
                  "  if (!button) return;\n"
                  "  button.addEventListener('click', () => {\n"
                  "    button.disabled = true;\n"
                  "    button.textContent = 'Refreshing...';\n"
                  "    fetch('/api/ops/demo_refresh', {\n"
                  "      method: 'POST',\n"
                  "      credentials: 'same-origin',\n"
                  "      headers: { 'Content-Type': 'application/json' },\n"
                  "      body: JSON.stringify({\n"
                  "        app: 'demo_web',\n"
                  "        schema: 'demo.refresh.v1',\n"
                  "        payload: { source: 'demo_button', requestedAt: Date.now() }\n"
                  "      })\n"
                  "    }).catch((error) => {\n"
                  "      console.warn('[pathspace demo] ops enqueue failed', error);\n"
                  "    }).finally(() => {\n"
                  "      setTimeout(() => {\n"
                  "        button.textContent = 'Reload demo';\n"
                  "        button.disabled = false;\n"
                  "      }, 600);\n"
                  "    });\n"
                  "  });\n"
                  "});\n");

    insert_string(html_base + "/commands",
                  "[{\"op\":\"fillRect\",\"args\":[32,32,256,128],\"color\":\"#1f4c94\"}]");

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
    std::vector<std::string> manifest_entries;
    manifest_entries.emplace_back(demo_asset_relative);
    if (auto status = replace_single_value(space,
                                           html_base + "/assets/manifest",
                                           manifest_entries);
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

    std::cout << "[serve_html] Seeded demo app at /apps/" << kDemoApp << "/" << kDemoView
              << " (renderer '" << options.renderer << "')\n";

    auto google_mapping_path = make_google_mapping_path(options, std::string{kDemoGoogleSub});
    if (!google_mapping_path.empty()) {
        auto mapping_result = space.insert(google_mapping_path, std::string{kDemoUser});
        if (!mapping_result.errors.empty()) {
            std::cerr << "[serve_html] Failed to seed demo Google mapping: "
                      << SP::describeError(mapping_result.errors.front()) << "\n";
        }
    }

    seed_demo_credentials(space, options);
}

bool gmtime_utc(std::time_t value, std::tm& out) {
#if defined(_WIN32)
    return gmtime_s(&out, &value) == 0;
#else
    return gmtime_r(&value, &out) != nullptr;
#endif
}

std::string format_timestamp(std::chrono::system_clock::time_point tp) {
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

std::string make_security_log_queue_path(std::string const& base_root) {
    if (base_root.empty()) {
        return {};
    }
    std::string path = base_root;
    path.append("/io/log/security/request_rejections/queue");
    return path;
}

std::string make_metrics_publish_path(std::string const& apps_root) {
    if (apps_root.empty()) {
        return {};
    }
    std::string path = apps_root;
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    path.append("/io/metrics/web_server/serve_html/live");
    return path;
}

void log_security_rejection(SP::PathSpace&           space,
                            std::string const&       base_root,
                            std::string_view         scope,
                            std::string_view         route,
                            std::string_view         remote_addr,
                            std::string_view         session_hint) {
    auto log_path = make_security_log_queue_path(base_root);
    if (log_path.empty()) {
        return;
    }

    json entry{{"ts", format_timestamp(std::chrono::system_clock::now())},
               {"scope", scope},
               {"route", route},
               {"remote_addr", remote_addr}};
    if (!session_hint.empty()) {
        entry["session"] = session_hint;
    }

    auto result = space.insert(log_path, entry.dump());
    if (!result.errors.empty()) {
        std::cerr << "[serve_html] Failed to append security log at " << log_path << ": "
                  << SP::describeError(result.errors.front()) << "\n";
    }
}

std::string format_timestamp_from_ns(std::uint64_t timestamp_ns) {
    if (timestamp_ns == 0) {
        return {};
    }
    auto duration = std::chrono::nanoseconds(timestamp_ns);
    auto tp = std::chrono::time_point<std::chrono::system_clock>(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(duration));
    return format_timestamp(tp);
}

std::string_view severity_to_string(UiRuntime::Diagnostics::PathSpaceError::Severity severity) {
    using Severity = UiRuntime::Diagnostics::PathSpaceError::Severity;
    switch (severity) {
    case Severity::Info:
        return "info";
    case Severity::Warning:
        return "warning";
    case Severity::Recoverable:
        return "recoverable";
    case Severity::Fatal:
        return "fatal";
    }
    return "info";
}

bool has_active_diagnostic(UiRuntime::Diagnostics::PathSpaceError const& error) {
    using Severity = UiRuntime::Diagnostics::PathSpaceError::Severity;
    if (error.code != 0) {
        return true;
    }
    if (error.severity != Severity::Info) {
        return true;
    }
    if (!error.message.empty() || !error.detail.empty()) {
        return true;
    }
    return false;
}

bool diagnostic_equals(UiRuntime::Diagnostics::PathSpaceError const& lhs,
                       UiRuntime::Diagnostics::PathSpaceError const& rhs) {
    return lhs.code == rhs.code && lhs.severity == rhs.severity && lhs.message == rhs.message
           && lhs.detail == rhs.detail && lhs.path == rhs.path && lhs.revision == rhs.revision
           && lhs.timestamp_ns == rhs.timestamp_ns;
}

void write_sse_event(httplib::DataSink& sink,
                     std::string_view   event_name,
                     std::string const& payload,
                     std::string const* event_id = nullptr) {
    std::string block;
    block.reserve(payload.size() + 64);
    if (event_id != nullptr && !event_id->empty()) {
        block.append("id: ");
        block.append(*event_id);
        block.append("\n");
    }
    block.append("event: ");
    block.append(event_name);
    block.append("\n");
    std::size_t start = 0U;
    while (start < payload.size()) {
        auto end = payload.find('\n', start);
        auto len = (end == std::string::npos ? payload.size() : end) - start;
        block.append("data: ");
        block.append(payload.data() + start, len);
        block.append("\n");
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    block.append("\n");
    sink.write(block.data(), block.size());
}

void write_sse_comment(httplib::DataSink& sink, std::string_view comment) {
    std::string block = ": ";
    block.append(comment.data(), comment.size());
    block.append("\n\n");
    sink.write(block.data(), block.size());
}

void write_sse_retry(httplib::DataSink& sink, int milliseconds) {
    std::string block = "retry: ";
    block.append(std::to_string(milliseconds));
    block.append("\n\n");
    sink.write(block.data(), block.size());
}

struct StreamSnapshot {
    std::optional<std::uint64_t> frame_index;
    std::optional<std::uint64_t> revision;
    std::optional<UiRuntime::Diagnostics::PathSpaceError> diagnostic;
};

class HtmlEventStreamSession {
public:
    HtmlEventStreamSession(ServeHtmlSpace& space,
                           std::string    html_base,
                           std::string    common_base,
                           std::string    diagnostics_path,
                           std::string    watch_glob,
                           std::uint64_t  resume_revision,
                           MetricsCollector* metrics)
        : space_(space)
        , context_(space.shared_context())
        , html_base_(std::move(html_base))
        , common_base_(std::move(common_base))
        , diagnostics_path_(std::move(diagnostics_path))
        , watch_glob_(std::move(watch_glob))
        , last_revision_sent_(resume_revision)
        , metrics_(metrics) {}

    auto pump(httplib::DataSink& sink) -> bool {
        if (cancelled_.load(std::memory_order_acquire) || g_should_stop.load(std::memory_order_acquire)) {
            return false;
        }

        if (!started_) {
            started_ = true;
            write_sse_retry(sink, 2000);
            auto snapshot = read_snapshot();
            if (!snapshot) {
                emit_error_event(sink, SP::describeError(snapshot.error()));
            } else {
                if (deliver_updates(*snapshot, sink, true)) {
                    last_keepalive_ = std::chrono::steady_clock::now();
                }
            }
            wait_for_change();
            return true;
        }

        wait_for_change();

        if (cancelled_.load(std::memory_order_acquire) || g_should_stop.load(std::memory_order_acquire)) {
            return false;
        }

        auto snapshot = read_snapshot();
        if (!snapshot) {
            emit_error_event(sink, SP::describeError(snapshot.error()));
            return true;
        }

        bool emitted = deliver_updates(*snapshot, sink, false);
        auto now      = std::chrono::steady_clock::now();
        if (emitted) {
            last_keepalive_ = now;
        } else if (now - last_keepalive_ >= kKeepAliveInterval) {
            emit_keepalive(sink);
            last_keepalive_ = now;
        }
        return true;
    }

    void cancel() {
        cancelled_.store(true, std::memory_order_release);
    }

    void finalize(bool) {
        cancelled_.store(true, std::memory_order_release);
    }

private:
    static constexpr auto kKeepAliveInterval = std::chrono::milliseconds(5000);
    static constexpr auto kWaitTimeout       = std::chrono::milliseconds(1500);

    auto read_snapshot() -> SP::Expected<StreamSnapshot> {
        StreamSnapshot snapshot{};
        auto frame_path = common_base_ + "/frameIndex";
        auto frame_val  = read_optional_value<std::uint64_t>(space_, frame_path);
        if (!frame_val) {
            return std::unexpected(frame_val.error());
        }
        snapshot.frame_index = std::move(*frame_val);

        auto revision_path = html_base_ + "/revision";
        auto revision_val  = read_optional_value<std::uint64_t>(space_, revision_path);
        if (!revision_val) {
            return std::unexpected(revision_val.error());
        }
        snapshot.revision = std::move(*revision_val);

        auto diag_val = read_optional_value<UiRuntime::Diagnostics::PathSpaceError>(space_, diagnostics_path_);
        if (!diag_val) {
            return std::unexpected(diag_val.error());
        }
        snapshot.diagnostic = std::move(*diag_val);
        return snapshot;
    }

    auto deliver_updates(StreamSnapshot const& snapshot,
                         httplib::DataSink&   sink,
                         bool                 initial) -> bool {
        bool emitted = false;
        if (snapshot.revision && snapshot.frame_index) {
            auto revision = *snapshot.revision;
            if (revision > 0) {
                bool should_emit = false;
                if (revision > last_revision_sent_) {
                    if (last_revision_sent_ > 0 && revision > last_revision_sent_ + 1) {
                        emit_reload_event(sink, last_revision_sent_, revision);
                        emitted = true;
                    }
                    should_emit = true;
                } else if (initial && last_revision_sent_ == 0) {
                    should_emit = true;
                }
                if (should_emit) {
                    emit_frame_event(sink, revision, *snapshot.frame_index);
                    emitted             = true;
                    last_revision_sent_ = revision;
                }
            }
        }

        bool diag_changed = initial;
        if (!initial) {
            if (!snapshot.diagnostic && !last_diagnostic_) {
                diag_changed = false;
            } else if (snapshot.diagnostic && last_diagnostic_) {
                diag_changed = !diagnostic_equals(*snapshot.diagnostic, *last_diagnostic_);
            } else {
                diag_changed = true;
            }
        }
        if (diag_changed) {
            emit_diagnostic_event(sink, snapshot.diagnostic);
            last_diagnostic_ = snapshot.diagnostic;
            emitted          = true;
        }
        return emitted;
    }

    void wait_for_change() {
        if (cancelled_.load(std::memory_order_acquire) || g_should_stop.load(std::memory_order_acquire)) {
            return;
        }
        if (!context_) {
            std::this_thread::sleep_for(kWaitTimeout);
            return;
        }
        auto guard = context_->wait(watch_glob_);
        guard.wait_until(std::chrono::system_clock::now() + kWaitTimeout);
    }

    void emit_frame_event(httplib::DataSink& sink,
                          std::uint64_t      revision,
                          std::uint64_t      frame_index) {
        json payload{{"type", "frame"},
                     {"revision", revision},
                     {"frameIndex", frame_index},
                     {"timestamp", format_timestamp(std::chrono::system_clock::now())}};
        auto id    = std::to_string(revision);
        auto body  = payload.dump();
        write_sse_event(sink, "frame", body, &id);
        record_event("frame");
    }

    void emit_reload_event(httplib::DataSink& sink,
                           std::uint64_t      from_revision,
                           std::uint64_t      to_revision) {
        json payload{{"type", "reload"},
                     {"fromRevision", from_revision},
                     {"toRevision", to_revision}};
        auto id   = std::to_string(to_revision);
        auto body = payload.dump();
        write_sse_event(sink, "reload", body, &id);
        record_event("reload");
    }

    void emit_diagnostic_event(httplib::DataSink& sink,
                               std::optional<UiRuntime::Diagnostics::PathSpaceError> const& diagnostic) {
        UiRuntime::Diagnostics::PathSpaceError value{};
        bool                                 has_value = false;
        if (diagnostic) {
            value      = *diagnostic;
            has_value  = true;
        }
        bool active = has_value && has_active_diagnostic(value);
        json payload{{"type", "diagnostic"},
                     {"active", active},
                     {"code", value.code},
                     {"severity", std::string(severity_to_string(value.severity))},
                     {"message", value.message},
                     {"path", value.path},
                     {"detail", value.detail},
                     {"revision", value.revision}};
        if (value.timestamp_ns != 0) {
            payload["timestamp"] = format_timestamp_from_ns(value.timestamp_ns);
        }
        auto body = payload.dump();
        write_sse_event(sink, "diagnostic", body, nullptr);
        record_event("diagnostic");
    }

    void emit_keepalive(httplib::DataSink& sink) {
        auto comment = std::string{"keep-alive "};
        comment.append(format_timestamp(std::chrono::system_clock::now()));
        write_sse_comment(sink, comment);
        record_event("keepalive");
    }

    void emit_error_event(httplib::DataSink& sink, std::string message) {
        json payload{{"type", "error"},
                     {"message", std::move(message)}};
        auto body = payload.dump();
        write_sse_event(sink, "error", body, nullptr);
        record_event("error");
    }

    void record_event(std::string_view type) {
        if (metrics_ != nullptr) {
            metrics_->record_sse_event(type);
        }
    }

    ServeHtmlSpace&                             space_;
    std::shared_ptr<SP::PathSpaceContext>       context_;
    std::string                                 html_base_;
    std::string                                 common_base_;
    std::string                                 diagnostics_path_;
    std::string                                 watch_glob_;
    std::uint64_t                               last_revision_sent_{0};
    std::optional<UiRuntime::Diagnostics::PathSpaceError> last_diagnostic_;
    bool                                        started_{false};
    std::atomic<bool>                           cancelled_{false};
    std::chrono::steady_clock::time_point       last_keepalive_{std::chrono::steady_clock::now()};
    MetricsCollector*                           metrics_{nullptr};
};

auto parse_last_event_id(httplib::Request const& req) -> std::optional<std::uint64_t> {
    auto header = req.get_header_value("Last-Event-ID");
    if (header.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    auto          result = std::from_chars(header.data(), header.data() + header.size(), value);
    if (result.ec != std::errc{}) {
        return std::nullopt;
    }
    return value;
}

void run_demo_refresh(ServeHtmlSpace&           space,
                      ServeHtmlOptions         options,
                      std::chrono::milliseconds interval,
                      std::atomic<bool>&       stop_flag) {
    if (interval <= std::chrono::milliseconds(0)) {
        return;
    }
    auto html_base   = make_html_base(options, kDemoApp, kDemoView);
    auto common_base = make_common_base(options, kDemoApp, kDemoView);
    auto diag_path   = make_diagnostics_path(options, kDemoApp, kDemoView);

    std::uint64_t revision    = 1;
    std::uint64_t frame_index = 1;
    bool          emit_error  = false;

    while (!stop_flag.load(std::memory_order_acquire) && !g_should_stop.load(std::memory_order_acquire)) {
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
        auto diag_result = replace_single_value<UiRuntime::Diagnostics::PathSpaceError>(space, diag_path, diag);
        if (!diag_result) {
            std::cerr << "[serve_html] demo refresh failed to update diagnostics: "
                      << SP::describeError(diag_result.error()) << "\n";
        }

        emit_error = !emit_error;
    }
}

} // namespace

bool EnsureUserPassword(ServeHtmlSpace&           space,
                        ServeHtmlOptions const&  options,
                        std::string const&       username,
                        std::string const&       password,
                        int                      work_factor) {
    auto hash = generate_bcrypt_hash(password, work_factor);
    if (!hash) {
        std::cerr << "[serve_html] Failed to generate bcrypt hash for user '" << username << "'\n";
        return false;
    }
    auto password_path = make_user_password_path(options, username);
    auto result        = space.insert(password_path, *hash);
    if (!result.errors.empty()) {
        std::cerr << "[serve_html] Failed to write credentials for user '"
                  << username << "': " << SP::describeError(result.errors.front()) << "\n";
        return false;
    }
    return true;
}

void RequestServeHtmlStop() {
    g_should_stop.store(true);
}

void ResetServeHtmlStopFlag() {
    g_should_stop.store(false);
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
                auto result = std::from_chars(value->data(), value->data() + value->size(), parsed);
                if (result.ec != std::errc{} || parsed <= 0 || parsed > 65535) {
                    std::cerr << "--port must be within 1-65535\n";
                    return std::nullopt;
                }
                options.port = parsed;
            } else {
                return std::nullopt;
            }
        } else if (arg == "--apps-root") {
            if (auto value = require_value(i, "--apps-root")) {
                if (value->empty() || value->front() != '/') {
                    std::cerr << "--apps-root must be an absolute path\n";
                    return std::nullopt;
                }
                options.apps_root = std::string{*value};
            } else {
                return std::nullopt;
            }
        } else if (arg == "--users-root") {
            if (auto value = require_value(i, "--users-root")) {
                if (value->empty() || value->front() != '/') {
                    std::cerr << "--users-root must be an absolute path\n";
                    return std::nullopt;
                }
                options.users_root = std::string{*value};
            } else {
                return std::nullopt;
            }
        } else if (arg == "--renderer") {
            if (auto value = require_value(i, "--renderer")) {
                if (!is_identifier(*value)) {
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
                long long parsed = options.session_idle_timeout_seconds;
                auto result = std::from_chars(value->data(), value->data() + value->size(), parsed);
                if (result.ec != std::errc{} || parsed < 0) {
                    std::cerr << "--session-timeout must be >= 0\n";
                    return std::nullopt;
                }
                options.session_idle_timeout_seconds = parsed;
            } else {
                return std::nullopt;
            }
        } else if (arg == "--session-max-age") {
            if (auto value = require_value(i, "--session-max-age")) {
                long long parsed = options.session_absolute_timeout_seconds;
                auto result = std::from_chars(value->data(), value->data() + value->size(), parsed);
                if (result.ec != std::errc{} || parsed < 0) {
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
                if (value->empty() || value->front() != '/') {
                    std::cerr << "--session-store-root must be an absolute path\n";
                    return std::nullopt;
                }
                options.session_store_path = std::string{*value};
            } else {
                return std::nullopt;
            }
        } else if (arg == "--rate-limit-ip-per-minute") {
            if (auto value = require_value(i, "--rate-limit-ip-per-minute")) {
                long long parsed = options.ip_rate_limit_per_minute;
                auto      result = std::from_chars(value->data(), value->data() + value->size(), parsed);
                if (result.ec != std::errc{} || parsed < 0) {
                    std::cerr << "--rate-limit-ip-per-minute must be >= 0\n";
                    return std::nullopt;
                }
                options.ip_rate_limit_per_minute = parsed;
            } else {
                return std::nullopt;
            }
        } else if (arg == "--rate-limit-ip-burst") {
            if (auto value = require_value(i, "--rate-limit-ip-burst")) {
                long long parsed = options.ip_rate_limit_burst;
                auto      result = std::from_chars(value->data(), value->data() + value->size(), parsed);
                if (result.ec != std::errc{} || parsed < 0) {
                    std::cerr << "--rate-limit-ip-burst must be >= 0\n";
                    return std::nullopt;
                }
                options.ip_rate_limit_burst = parsed;
            } else {
                return std::nullopt;
            }
        } else if (arg == "--rate-limit-session-per-minute") {
            if (auto value = require_value(i, "--rate-limit-session-per-minute")) {
                long long parsed = options.session_rate_limit_per_minute;
                auto      result = std::from_chars(value->data(), value->data() + value->size(), parsed);
                if (result.ec != std::errc{} || parsed < 0) {
                    std::cerr << "--rate-limit-session-per-minute must be >= 0\n";
                    return std::nullopt;
                }
                options.session_rate_limit_per_minute = parsed;
            } else {
                return std::nullopt;
            }
        } else if (arg == "--rate-limit-session-burst") {
            if (auto value = require_value(i, "--rate-limit-session-burst")) {
                long long parsed = options.session_rate_limit_burst;
                auto      result = std::from_chars(value->data(), value->data() + value->size(), parsed);
                if (result.ec != std::errc{} || parsed < 0) {
                    std::cerr << "--rate-limit-session-burst must be >= 0\n";
                    return std::nullopt;
                }
                options.session_rate_limit_burst = parsed;
            } else {
                return std::nullopt;
            }
        } else if (arg == "--demo-refresh-interval-ms") {
            if (auto value = require_value(i, "--demo-refresh-interval-ms")) {
                long long parsed = options.demo_refresh_interval_ms;
                auto result = std::from_chars(value->data(), value->data() + value->size(), parsed);
                if (result.ec != std::errc{} || parsed < 0) {
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
                if (value->empty()
                    || (!value->starts_with("http://") && !value->starts_with("https://"))) {
                    std::cerr << "--google-redirect-uri must be an absolute http(s) URL\n";
                    return std::nullopt;
                }
                options.google_redirect_uri = std::string{*value};
            } else {
                return std::nullopt;
            }
        } else if (arg == "--google-auth-endpoint") {
            if (auto value = require_value(i, "--google-auth-endpoint")) {
                if (value->empty()
                    || (!value->starts_with("http://") && !value->starts_with("https://"))) {
                    std::cerr << "--google-auth-endpoint must be http(s) URL\n";
                    return std::nullopt;
                }
                options.google_auth_endpoint = std::string{*value};
            } else {
                return std::nullopt;
            }
        } else if (arg == "--google-token-endpoint") {
            if (auto value = require_value(i, "--google-token-endpoint")) {
                if (value->empty()
                    || (!value->starts_with("http://") && !value->starts_with("https://"))) {
                    std::cerr << "--google-token-endpoint must be http(s) URL\n";
                    return std::nullopt;
                }
                options.google_token_endpoint = std::string{*value};
            } else {
                return std::nullopt;
            }
        } else if (arg == "--google-jwks-endpoint") {
            if (auto value = require_value(i, "--google-jwks-endpoint")) {
                if (value->empty()
                    || (!value->starts_with("http://") && !value->starts_with("https://"))) {
                    std::cerr << "--google-jwks-endpoint must be http(s) URL\n";
                    return std::nullopt;
                }
                options.google_jwks_endpoint = std::string{*value};
            } else {
                return std::nullopt;
            }
        } else if (arg == "--google-users-root") {
            if (auto value = require_value(i, "--google-users-root")) {
                if (value->empty() || value->front() != '/') {
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

    if (options.session_store_backend != "memory"
        && options.session_store_backend != "pathspace") {
        std::cerr << "Unsupported session store backend: " << options.session_store_backend << "\n";
        return std::nullopt;
    }
    if (options.session_store_backend == "pathspace"
        && (options.session_store_path.empty() || options.session_store_path.front() != '/')) {
        std::cerr << "PathSpace session store requires an absolute --session-store-root\n";
        return std::nullopt;
    }
    if (options.demo_refresh_interval_ms > 0 && !options.seed_demo) {
        std::cerr << "--demo-refresh-interval-ms requires --seed-demo\n";
        return std::nullopt;
    }
    bool google_fields_set = !options.google_client_id.empty()
                             || !options.google_client_secret.empty()
                             || !options.google_redirect_uri.empty();
    if (google_fields_set) {
        if (options.google_client_id.empty() || options.google_client_secret.empty()
            || options.google_redirect_uri.empty()) {
            std::cerr << "Google OAuth requires --google-client-id, --google-client-secret, and --google-redirect-uri\n";
            return std::nullopt;
        }
    }

    return options;
}

int RunServeHtmlServer(ServeHtmlSpace& space, ServeHtmlOptions const& options) {
    if (options.seed_demo) {
        seed_demo_application(space, options);
    }

    std::atomic<bool> demo_refresh_stop{false};
    std::thread       demo_refresh_thread;
    if (options.seed_demo && options.demo_refresh_interval_ms > 0) {
        auto interval = std::chrono::milliseconds(options.demo_refresh_interval_ms);
        if (interval <= std::chrono::milliseconds(0)) {
            interval = std::chrono::milliseconds(200);
        }
        ServeHtmlOptions demo_options = options;
        demo_refresh_thread = std::thread([&space, demo_options, interval, &demo_refresh_stop]() mutable {
            run_demo_refresh(space, demo_options, interval, demo_refresh_stop);
        });
    }

    SessionConfig session_config{
        .cookie_name = options.session_cookie_name,
        .idle_timeout = std::chrono::seconds{options.session_idle_timeout_seconds},
        .absolute_timeout = std::chrono::seconds{options.session_absolute_timeout_seconds},
    };
    auto session_store_ptr = make_session_store(space, options, session_config);
    if (!session_store_ptr) {
        std::cerr << "[serve_html] failed to initialize session store\n";
        return EXIT_FAILURE;
    }
    SessionStore& session_store = *session_store_ptr;
    MetricsCollector metrics;

    bool google_enabled = !options.google_client_id.empty()
                          && !options.google_client_secret.empty()
                          && !options.google_redirect_uri.empty();
    std::optional<UrlView> google_auth_url;
    std::optional<UrlView> google_token_url;
    std::optional<UrlView> google_jwks_url;
    GoogleAuthStateStore   google_state_store;
    GoogleJwksCache        google_jwks_cache;
    std::string            google_scope = options.google_scope.empty()
                                                ? std::string{"openid email profile"}
                                                : options.google_scope;
    if (google_enabled) {
        google_auth_url  = parse_url(options.google_auth_endpoint);
        google_token_url = parse_url(options.google_token_endpoint);
        google_jwks_url  = parse_url(options.google_jwks_endpoint);
        if (!google_auth_url || !google_token_url || !google_jwks_url) {
            std::cerr << "[serve_html] invalid Google OAuth endpoints; disable or fix configuration\n";
            return EXIT_FAILURE;
        }
        google_jwks_cache.set_endpoint(*google_jwks_url);
    }

    auto           metrics_publish_path = make_metrics_publish_path(options.apps_root);
    std::atomic<bool> metrics_publish_stop{false};
    std::thread        metrics_publish_thread;
    if (!metrics_publish_path.empty()) {
        metrics_publish_thread = std::thread([&space,
                                              metrics_publish_path,
                                              &metrics,
                                              &metrics_publish_stop]() {
            while (!metrics_publish_stop.load(std::memory_order_acquire)
                   && !g_should_stop.load(std::memory_order_acquire)) {
                auto snapshot   = metrics.snapshot_json();
                auto serialized = snapshot.dump();
                auto status = replace_single_value(space, metrics_publish_path, serialized);
                if (!status) {
                    std::cerr << "[serve_html] Failed to publish metrics at "
                              << metrics_publish_path << ": "
                              << SP::describeError(status.error()) << "\n";
                }
                for (int i = 0; i < 10 && !metrics_publish_stop.load(std::memory_order_acquire)
                                     && !g_should_stop.load(std::memory_order_acquire);
                     ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
            }
        });
    }

    auto set_session_cookie = [&](httplib::Response& res,
                                  std::string const& value,
                                  std::optional<std::chrono::seconds> max_age) {
        res.set_header("Set-Cookie",
                       build_cookie_header(session_store.cookie_name(), value, max_age));
    };

    auto default_cookie_age = [&]() -> std::optional<std::chrono::seconds> {
        auto age = session_store.cookie_max_age();
        if (age.count() <= 0) {
            return std::nullopt;
        }
        return age;
    };

    auto apply_session_cookie = [&](httplib::Response& res, std::string const& value) {
        set_session_cookie(res, value, default_cookie_age());
    };

    auto expire_session_cookie = [&](httplib::Response& res) {
        set_session_cookie(res,
                           "",
                           std::optional<std::chrono::seconds>{std::chrono::seconds{0}});
    };

    auto ensure_session = [&](httplib::Request const&        req,
                              httplib::Response&             res,
                              std::optional<std::string> const& cookie_hint = std::nullopt) {
        std::optional<std::string> cookie = cookie_hint;
        if (!cookie.has_value()) {
            cookie = read_cookie_value(req, session_store.cookie_name());
        }
        if (!cookie || cookie->empty()) {
            if (options.auth_optional) {
                return true;
            }
            respond_unauthorized(res);
            return false;
        }

        auto username = session_store.validate(*cookie);
        if (!username) {
            expire_session_cookie(res);
            if (options.auth_optional) {
                return true;
            }
            respond_unauthorized(res);
            return false;
        }

        (void)username;
        return true;
    };

    TokenBucketRateLimiter ip_rate_limiter{options.ip_rate_limit_per_minute,
                                           options.ip_rate_limit_burst};
    TokenBucketRateLimiter session_rate_limiter{options.session_rate_limit_per_minute,
                                                options.session_rate_limit_burst};

    auto apply_rate_limits = [&](std::string_view             route_name,
                                 httplib::Request const&      req,
                                 httplib::Response&           res,
                                 std::optional<std::string> const& session_cookie,
                                 std::string const*           app_root) -> bool {
        auto const now = TokenBucketRateLimiter::Clock::now();
        auto       remote_addr = get_client_address(req);
        auto       session_hint = session_cookie && !session_cookie->empty()
                                      ? abbreviate_token(*session_cookie)
                                      : std::string{};
        std::string const& log_root = (app_root != nullptr && !app_root->empty())
                                           ? *app_root
                                           : options.apps_root;

        if (!ip_rate_limiter.allow(remote_addr, now)) {
            respond_rate_limited(res);
            metrics.record_rate_limit("ip", route_name);
            log_security_rejection(space, log_root, "ip", route_name, remote_addr, session_hint);
            return false;
        }

        if (session_cookie && !session_cookie->empty()
            && !session_rate_limiter.allow(*session_cookie, now)) {
            respond_rate_limited(res);
            metrics.record_rate_limit("session", route_name);
            log_security_rejection(
                space, log_root, "session", route_name, remote_addr, session_hint);
            return false;
        }

        return true;
    };

    httplib::Server server;

    server.Get("/", [&](httplib::Request const&, httplib::Response& res) {
        [[maybe_unused]] RequestMetricsScope request_scope{metrics, RouteMetric::Root, res};
        res.set_content(
            "PathSpace Web Server prototype\n\nPOST /login with {\"username\",\"password\"} to obtain a session cookie, then visit /apps/<app>/<view> to fetch DOM / CSS data.\n",
            "text/plain; charset=utf-8");
    });

    server.Get("/healthz", [&](httplib::Request const&, httplib::Response& res) {
        [[maybe_unused]] RequestMetricsScope request_scope{metrics, RouteMetric::Healthz, res};
        res.status = 200;
        res.set_content("ok", "text/plain; charset=utf-8");
    });

    if (google_enabled) {
        server.Get("/login/google", [&](httplib::Request const& req, httplib::Response& res) {
            [[maybe_unused]] RequestMetricsScope request_scope{metrics, RouteMetric::LoginGoogle, res};
            auto session_cookie = read_cookie_value(req, session_store.cookie_name());
            if (!apply_rate_limits("login_google", req, res, session_cookie, nullptr)) {
                return;
            }

            auto redirect = req.has_param("redirect") ? req.get_param_value("redirect") : std::string{};
            auto issued   = google_state_store.issue(redirect);
            auto challenge = compute_code_challenge(issued.entry.code_verifier);

            std::vector<std::pair<std::string, std::string>> params{
                {"client_id", options.google_client_id},
                {"redirect_uri", options.google_redirect_uri},
                {"response_type", "code"},
                {"scope", google_scope},
                {"state", issued.state},
                {"code_challenge", challenge},
                {"code_challenge_method", "S256"},
                {"access_type", "online"},
            };
            auto location = build_absolute_url(*google_auth_url);
            location.push_back(location.find('?') == std::string::npos ? '?' : '&');
            location.append(build_query_string(params));

            res.status = 302;
            res.set_header("Location", location);
            res.set_header("Cache-Control", "no-store");
        });

        server.Get("/login/google/callback",
                   [&](httplib::Request const& req, httplib::Response& res) {
                       [[maybe_unused]] RequestMetricsScope request_scope{
                           metrics, RouteMetric::LoginGoogleCallback, res};
                       auto session_cookie = read_cookie_value(req, session_store.cookie_name());
                       if (!apply_rate_limits("login_google_callback", req, res, session_cookie, nullptr)) {
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
                       auto state_value = req.get_param_value("state");
                       auto entry       = google_state_store.take(state_value);
                       if (!entry) {
                           respond_bad_request(res, "invalid oauth state");
                           return;
                       }
                       auto code_value = req.get_param_value("code");
                       if (code_value.empty()) {
                           respond_bad_request(res, "authorization code missing");
                           return;
                       }

                       httplib::Headers token_headers{{"Accept", "application/json"}};
                       auto token_body = build_query_string({
                           {"code", code_value},
                           {"client_id", options.google_client_id},
                           {"client_secret", options.google_client_secret},
                           {"redirect_uri", options.google_redirect_uri},
                           {"grant_type", "authorization_code"},
                           {"code_verifier", entry->code_verifier},
                       });
                       auto token_response = http_post_form(*google_token_url, token_body, token_headers);
                       if (!token_response || token_response->status != 200) {
                           respond_server_error(res, "Google token exchange failed");
                           return;
                       }
                       auto token_json = json::parse(token_response->body, nullptr, false);
                       if (token_json.is_discarded() || !token_json.contains("id_token")) {
                           respond_server_error(res, "Invalid Google token payload");
                           return;
                       }
                       if (token_json.contains("error")) {
                           metrics.record_auth_failure();
                           respond_unauthorized(res);
                           return;
                       }
                       auto id_token = token_json["id_token"].get<std::string>();
                       auto payload  = decode_google_id_token(id_token, google_jwks_cache, options.google_client_id);
                       if (!payload) {
                           metrics.record_auth_failure();
                           respond_unauthorized(res);
                           return;
                       }
                       auto username = resolve_google_username(space, options, payload->sub);
                       if (!username) {
                           metrics.record_auth_failure();
                           respond_unauthorized(res);
                           return;
                       }
                       auto session_id = session_store.create_session(*username);
                       if (!session_id) {
                           respond_server_error(res, "failed to create session");
                           return;
                       }
                       apply_session_cookie(res, *session_id);
                       res.status = 302;
                       res.set_header("Location", entry->redirect_path);
                       res.set_header("Cache-Control", "no-store");
                   });
    }

    server.Post("/login", [&](httplib::Request const& req, httplib::Response& res) {
        [[maybe_unused]] RequestMetricsScope request_scope{metrics, RouteMetric::Login, res};
        auto session_cookie = read_cookie_value(req, session_store.cookie_name());
        if (!apply_rate_limits("login", req, res, session_cookie, nullptr)) {
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

        auto password_path = make_user_password_path(options, username);
        auto stored_hash = read_optional_value<std::string>(space, password_path);
        if (!stored_hash) {
            respond_server_error(res, "failed to read credentials");
            return;
        }
        if (!stored_hash->has_value()) {
            respond_unauthorized(res);
            metrics.record_auth_failure();
            return;
        }

        int check = bcrypt_checkpw(password.c_str(), stored_hash->value().c_str());
        if (check == -1) {
            respond_server_error(res, "bcrypt verification failed");
            return;
        }
        if (check != 0) {
            respond_unauthorized(res);
            metrics.record_auth_failure();
            return;
        }

        auto session_id = session_store.create_session(username);
        if (!session_id) {
            respond_server_error(res, "failed to create session");
            return;
        }
        apply_session_cookie(res, *session_id);
        write_json_response(res,
                            json{{"status", "ok"},
                                 {"username", username}},
                            200,
                            true);
    });

    server.Post("/logout", [&](httplib::Request const& req, httplib::Response& res) {
        [[maybe_unused]] RequestMetricsScope request_scope{metrics, RouteMetric::Logout, res};
        auto session_cookie = read_cookie_value(req, session_store.cookie_name());
        if (!apply_rate_limits("logout", req, res, session_cookie, nullptr)) {
            return;
        }
        if (session_cookie) {
            session_store.revoke(*session_cookie);
        }
        expire_session_cookie(res);
        write_json_response(res, json{{"status", "ok"}}, 200, true);
    });

    server.Get("/session", [&](httplib::Request const& req, httplib::Response& res) {
        [[maybe_unused]] RequestMetricsScope request_scope{metrics, RouteMetric::Session, res};
        auto session_cookie = read_cookie_value(req, session_store.cookie_name());
        if (!apply_rate_limits("session", req, res, session_cookie, nullptr)) {
            return;
        }

        bool authenticated = false;
        if (session_cookie) {
            authenticated = session_store.validate(*session_cookie).has_value();
            if (!authenticated) {
                expire_session_cookie(res);
            }
        }
        write_json_response(res, json{{"authenticated", authenticated}}, 200, true);
    });

    server.Get("/metrics", [&](httplib::Request const& req, httplib::Response& res) {
        [[maybe_unused]] RequestMetricsScope request_scope{metrics, RouteMetric::Metrics, res};
        auto session_cookie = read_cookie_value(req, session_store.cookie_name());
        if (!apply_rate_limits("metrics", req, res, session_cookie, nullptr)) {
            return;
        }
        if (!ensure_session(req, res, session_cookie)) {
            return;
        }
        auto body = metrics.render_prometheus();
        res.set_header("Cache-Control", "no-store");
        res.set_content(body, "text/plain; version=0.0.4");
    });

    server.Get(R"(/apps/([A-Za-z0-9_\-\.]+)/([A-Za-z0-9_\-\.]+))",
               [&](httplib::Request const& req, httplib::Response& res) {
                   [[maybe_unused]] RequestMetricsScope request_scope{metrics, RouteMetric::Apps, res};
                   if (req.matches.size() < 3) {
                       res.status = 400;
                       res.set_content("invalid route", "text/plain; charset=utf-8");
                       return;
                   }
                   std::string app = req.matches[1];
                   std::string view = req.matches[2];
                   if (!is_identifier(app) || !is_identifier(view)) {
                       res.status = 400;
                       res.set_content("invalid app or view", "text/plain; charset=utf-8");
                       return;
                   }

                   auto session_cookie = read_cookie_value(req, session_store.cookie_name());
                   auto app_root       = make_app_root_path(options, app);
                   if (!apply_rate_limits("apps", req, res, session_cookie, &app_root)) {
                       return;
                   }
                   if (!ensure_session(req, res, session_cookie)) {
                       return;
                   }

                   auto html_base = make_html_base(options, app, view);
                   auto payload = load_html_payload(space, html_base);
                   if (!payload) {
                       auto const& error = payload.error();
                       if (error.code == SP::Error::Code::NoObjectFound
                           || error.code == SP::Error::Code::NoSuchPath) {
                           res.status = 404;
                           res.set_content("no HTML output at " + html_base,
                                           "text/plain; charset=utf-8");
                       } else {
                           res.status = 500;
                           res.set_content("failed to read HTML output: " + SP::describeError(error),
                                           "text/plain; charset=utf-8");
                       }
                       return;
                   }

                   record_asset_manifest(app, view, *payload);

                   bool wants_json = false;
                   if (req.has_param("format")) {
                       wants_json = req.get_param_value("format") == "json";
                   } else {
                       auto accept = req.get_header_value("Accept");
                       if (!accept.empty()
                           && accept.find("application/json") != std::string::npos) {
                           wants_json = true;
                       }
                   }

                   res.set_header("X-PathSpace-App", app);
                   res.set_header("X-PathSpace-View", view);
                   if (payload->revision) {
                       res.set_header("ETag", "\"" + std::to_string(*payload->revision) + "\"");
                   }

                   if (wants_json) {
                       json payload_json{{"dom", payload->dom},
                                         {"css", payload->css.value_or(std::string{})},
                                         {"js", payload->js.value_or(std::string{})},
                                         {"commands", payload->commands.value_or(std::string{})},
                                         {"revision", payload->revision.value_or(0)}};
                       write_json_response(res, payload_json, 200, true);
                       return;
                   }

                   auto body = build_response_body(*payload, app, view);
                   res.set_content(body, "text/html; charset=utf-8");
                   res.set_header("Cache-Control", "no-store");
               });

    server.Get(R"(/assets/([A-Za-z0-9_\-\.]+)/(.+))",
               [&](httplib::Request const& req, httplib::Response& res) {
                   [[maybe_unused]] RequestMetricsScope request_scope{metrics, RouteMetric::Assets, res};
                   if (req.matches.size() < 3) {
                       res.status = 400;
                       res.set_content("invalid route", "text/plain; charset=utf-8");
                       return;
                   }

                   std::string app       = req.matches[1];
                   std::string asset_rel = req.matches[2];
                   if (!is_identifier(app) || !is_asset_path(asset_rel)) {
                       res.status = 400;
                       res.set_content("invalid app or asset path", "text/plain; charset=utf-8");
                       return;
                   }

                   auto session_cookie = read_cookie_value(req, session_store.cookie_name());
                   auto app_root       = make_app_root_path(options, app);
                   if (!apply_rate_limits("assets", req, res, session_cookie, &app_root)) {
                       return;
                   }
                   if (!ensure_session(req, res, session_cookie)) {
                       return;
                   }

                   auto locator = lookup_asset_locator(app, asset_rel);
                   if (!locator) {
                       res.status = 404;
                       res.set_content("asset not indexed", "text/plain; charset=utf-8");
                       return;
                   }

                   auto html_base  = make_html_base(options, app, locator->view);
                   auto data_path  = html_base + "/assets/data/" + asset_rel;
                   auto bytes      = space.read<std::vector<std::uint8_t>>(data_path);
                   if (!bytes) {
                       auto const error = bytes.error();
                       if (error.code == SP::Error::Code::NoObjectFound
                           || error.code == SP::Error::Code::NoSuchPath) {
                           res.status = 404;
                           res.set_content("asset not found", "text/plain; charset=utf-8");
                       } else {
                           res.status = 500;
                           res.set_content("failed to read asset: " + SP::describeError(error),
                                           "text/plain; charset=utf-8");
                       }
                       return;
                   }

                   auto mime_path = html_base + "/assets/meta/" + asset_rel;
                   auto mime      = read_optional_value<std::string>(space, mime_path);
                   if (!mime) {
                       res.status = 500;
                       res.set_content("failed to read asset metadata: " + SP::describeError(mime.error()),
                                       "text/plain; charset=utf-8");
                       return;
                   }
                   auto content_type = mime->value_or("application/octet-stream");

                   std::string etag;
                   if (locator->revision) {
                       etag = "\"r" + std::to_string(*locator->revision) + ":" + asset_rel + "\"";
                   } else {
                       etag = "\"asset:" + asset_rel + "\"";
                   }

                   auto if_none_match = req.get_header_value("If-None-Match");
                   auto cache_control = "public, max-age=31536000, immutable";
                   res.set_header("Cache-Control", cache_control);
                   res.set_header("X-PathSpace-App", app);
                   res.set_header("X-PathSpace-View", locator->view);
                   res.set_header("X-PathSpace-Asset", asset_rel);
                   if (!etag.empty()) {
                       res.set_header("ETag", etag);
                   }
                   if (!etag.empty() && !if_none_match.empty() && if_none_match == etag) {
                       res.status = 304;
                       metrics.record_asset_cache_hit();
                       return;
                   }

                   std::string body(bytes->begin(), bytes->end());
                   res.set_content(std::move(body), content_type.c_str());
                   metrics.record_asset_cache_miss();
               });

    server.Post(R"(/api/ops/([A-Za-z0-9_\-\.]+))",
                [&](httplib::Request const& req, httplib::Response& res) {
                    [[maybe_unused]] RequestMetricsScope request_scope{metrics, RouteMetric::ApiOps, res};
                    auto session_cookie = read_cookie_value(req, session_store.cookie_name());
                    if (!apply_rate_limits("api_ops", req, res, session_cookie, nullptr)) {
                        return;
                    }
                    if (!ensure_session(req, res, session_cookie)) {
                        return;
                    }
                    if (req.matches.size() < 2) {
                        respond_bad_request(res, "invalid op route");
                        return;
                    }

                    std::string op = req.matches[1];
                    if (!is_identifier(op)) {
                        respond_bad_request(res, "invalid op identifier");
                        return;
                    }

                    auto content_type = req.get_header_value("Content-Type");
                    if (content_type.find("application/json") == std::string::npos) {
                        respond_unsupported_media_type(res);
                        return;
                    }
                    if (req.body.empty()) {
                        respond_bad_request(res, "body must not be empty");
                        return;
                    }
                    if (req.body.size() > kMaxApiPayloadBytes) {
                        respond_payload_too_large(res);
                        return;
                    }

                    auto payload = json::parse(req.body, nullptr, false);
                    if (payload.is_discarded() || !payload.is_object()) {
                        respond_bad_request(res, "body must be a JSON object");
                        return;
                    }

                    auto app_it = payload.find("app");
                    auto schema_it = payload.find("schema");
                    if (app_it == payload.end() || !app_it->is_string() || schema_it == payload.end()
                        || !schema_it->is_string()) {
                        respond_bad_request(res, "app and schema fields are required");
                        return;
                    }

                    std::string app = app_it->get<std::string>();
                    std::string schema = schema_it->get<std::string>();
                    if (!is_identifier(app) || schema.empty()) {
                        respond_bad_request(res, "invalid app or schema");
                        return;
                    }

                    std::string serialized = payload.dump();
                    if (serialized.size() > kMaxApiPayloadBytes) {
                        respond_payload_too_large(res);
                        return;
                    }

                    auto queue_path = make_ops_queue_path(options, app, op);
                    auto enqueue_start = std::chrono::steady_clock::now();
                    auto inserted     = space.insert(queue_path, serialized);
                    auto enqueue_end  = std::chrono::steady_clock::now();
                    metrics.record_render_trigger_latency(
                        std::chrono::duration_cast<std::chrono::microseconds>(enqueue_end - enqueue_start));
                    if (!inserted.errors.empty()) {
                        respond_server_error(res,
                                             "failed to enqueue op: "
                                                 + SP::describeError(inserted.errors.front()));
                        return;
                    }

                    res.set_header("X-PathSpace-App", app);
                    res.set_header("X-PathSpace-Op", op);
                    res.set_header("X-PathSpace-Queue", queue_path);

                    write_json_response(res,
                                        json{{"status", "enqueued"},
                                             {"app", app},
                                             {"op", op},
                                             {"schema", schema},
                                             {"queue", queue_path},
                                             {"bytes", serialized.size()}},
                                        202,
                                        true);
                });

    server.Get(R"(/apps/([A-Za-z0-9_\-\.]+)/([A-Za-z0-9_\-\.]+)/events)",
               [&](httplib::Request const& req, httplib::Response& res) {
                   [[maybe_unused]] RequestMetricsScope request_scope{metrics, RouteMetric::Events, res};
                   if (req.matches.size() < 3) {
                       res.status = 400;
                       res.set_content("invalid route", "text/plain; charset=utf-8");
                       return;
                   }

                   std::string app  = req.matches[1];
                   std::string view = req.matches[2];
                   if (!is_identifier(app) || !is_identifier(view)) {
                       res.status = 400;
                       res.set_content("invalid app or view", "text/plain; charset=utf-8");
                       return;
                   }

                   auto session_cookie = read_cookie_value(req, session_store.cookie_name());
                   auto app_root       = make_app_root_path(options, app);
                   if (!apply_rate_limits("apps_events", req, res, session_cookie, &app_root)) {
                       return;
                   }
                   if (!ensure_session(req, res, session_cookie)) {
                       return;
                   }

                   auto html_base   = make_html_base(options, app, view);
                   auto common_base = make_common_base(options, app, view);
                   auto diag_path   = make_diagnostics_path(options, app, view);
                   auto watch_glob  = make_watch_glob(options, app, view);
                   auto resume_rev  = parse_last_event_id(req).value_or(0);

                   auto session = std::make_shared<HtmlEventStreamSession>(space,
                                                                           std::move(html_base),
                                                                           std::move(common_base),
                                                                           std::move(diag_path),
                                                                           std::move(watch_glob),
                                                                           resume_rev,
                                                                           &metrics);
                   res.set_header("Cache-Control", "no-store");
                   res.set_header("Connection", "keep-alive");
                   res.set_header("X-Accel-Buffering", "no");
                   metrics.record_sse_connection_open();
                   res.set_chunked_content_provider(
                       "text/event-stream",
                       [session](size_t, httplib::DataSink& sink) {
                           return session->pump(sink);
                       },
                       [session, &metrics](bool done) {
                           session->cancel();
                           session->finalize(done);
                           metrics.record_sse_connection_close();
                       });
               });

    std::atomic<bool> listen_failed{false};
    std::thread server_thread([&]() {
        if (!server.listen(options.host.c_str(), options.port)) {
            if (!g_should_stop.load()) {
                std::cerr << "[serve_html] Failed to bind " << options.host << ":" << options.port << "\n";
                listen_failed.store(true);
                g_should_stop.store(true);
            }
        }
    });

    std::cout << "[serve_html] Listening on http://" << options.host << ":" << options.port << "\n";
    if (options.seed_demo) {
        std::cout << "[serve_html] Try http://" << options.host << ":" << options.port
                  << "/apps/demo_web/gallery" << std::endl;
    }

    while (!g_should_stop.load() && !listen_failed.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    demo_refresh_stop.store(true, std::memory_order_release);
    server.stop();
    if (server_thread.joinable()) {
        server_thread.join();
    }

    if (demo_refresh_thread.joinable()) {
        demo_refresh_thread.join();
    }

    metrics_publish_stop.store(true, std::memory_order_release);
    if (metrics_publish_thread.joinable()) {
        metrics_publish_thread.join();
    }

    return listen_failed.load() ? EXIT_FAILURE : EXIT_SUCCESS;
}

} // namespace SP::ServeHtml
