#include <pathspace/ui/FontManager.hpp>

#include "BuildersDetail.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <optional>
#include <unordered_set>
#include <cctype>

namespace {

constexpr std::uint64_t kFNVOffset = 1469598103934665603ull;
constexpr std::uint64_t kFNVPrime = 1099511628211ull;
constexpr float kFallbackAdvanceUnits = 8.0f;
constexpr float kFallbackMinScale = 0.1f;

auto fnv_mix(std::uint64_t hash, std::string_view bytes) -> std::uint64_t {
    for (unsigned char ch : bytes) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= kFNVPrime;
    }
    return hash;
}

auto fnv_mix(std::uint64_t hash, std::uint64_t value) -> std::uint64_t {
    for (int i = 0; i < 8; ++i) {
        auto byte = static_cast<unsigned char>((value >> (i * 8)) & 0xFFu);
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= kFNVPrime;
    }
    return hash;
}

auto sanitize_cache_key(std::uint64_t hash) -> std::uint64_t {
    return (hash == 0) ? kFNVPrime : hash;
}

auto make_font_registry_key(std::string_view app_root,
                            std::string_view family,
                            std::string_view style) -> std::string {
    std::string key;
    key.reserve(app_root.size() + family.size() + style.size() + 2);
    key.append(app_root);
    key.push_back(':');
    key.append(family);
    key.push_back(':');
    key.append(style);
    return key;
}

auto make_manifest_error(std::string message) -> SP::Error {
    return SP::Error{SP::Error::Code::MalformedInput, std::move(message)};
}

auto skip_whitespace(std::string_view text, std::size_t pos) -> std::size_t {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
        ++pos;
    }
    return pos;
}

auto parse_json_string(std::string_view text,
                       std::size_t pos) -> SP::Expected<std::pair<std::string, std::size_t>> {
    if (pos >= text.size() || text[pos] != '"') {
        return std::unexpected(make_manifest_error("expected string value"));
    }

    std::string value;
    ++pos;
    while (pos < text.size()) {
        char ch = text[pos];
        if (ch == '\\') {
            ++pos;
            if (pos >= text.size()) {
                return std::unexpected(make_manifest_error("unterminated escape sequence"));
            }
            char escaped = text[pos];
            switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    value.push_back(escaped);
                    break;
                case 'b':
                    value.push_back('\b');
                    break;
                case 'f':
                    value.push_back('\f');
                    break;
                case 'n':
                    value.push_back('\n');
                    break;
                case 'r':
                    value.push_back('\r');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                default:
                    return std::unexpected(make_manifest_error("unsupported escape sequence in string"));
            }
            ++pos;
            continue;
        }
        if (ch == '"') {
            ++pos;
            return std::pair<std::string, std::size_t>{std::move(value), pos};
        }
        value.push_back(ch);
        ++pos;
    }
    return std::unexpected(make_manifest_error("unterminated string literal"));
}

auto find_key(std::string_view text, std::string_view key) -> std::size_t {
    auto needle = std::string{"\""};
    needle.append(key);
    needle.push_back('"');
    return text.find(needle);
}

auto parse_string_field(std::string_view text,
                        std::string_view key) -> SP::Expected<std::optional<std::string>> {
    auto pos = find_key(text, key);
    if (pos == std::string_view::npos) {
        return std::optional<std::string>{};
    }
    pos = text.find(':', pos + key.size() + 2);
    if (pos == std::string_view::npos) {
        return std::unexpected(make_manifest_error("missing ':' after key"));
    }
    pos = skip_whitespace(text, pos + 1);
    auto parsed = parse_json_string(text, pos);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return std::optional<std::string>{std::move(parsed->first)};
}

auto parse_string_array_field(std::string_view text,
                              std::string_view key)
    -> SP::Expected<std::optional<std::vector<std::string>>> {
    auto pos = find_key(text, key);
    if (pos == std::string_view::npos) {
        return std::optional<std::vector<std::string>>{};
    }
    pos = text.find(':', pos + key.size() + 2);
    if (pos == std::string_view::npos) {
        return std::unexpected(make_manifest_error("missing ':' after key"));
    }
    pos = skip_whitespace(text, pos + 1);
    if (pos >= text.size() || text[pos] != '[') {
        return std::unexpected(make_manifest_error("expected '[' for array value"));
    }
    ++pos;
    std::vector<std::string> values;
    while (true) {
        pos = skip_whitespace(text, pos);
        if (pos >= text.size()) {
            return std::unexpected(make_manifest_error("unterminated array"));
        }
        if (text[pos] == ']') {
            ++pos;
            break;
        }
        auto parsed = parse_json_string(text, pos);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        values.emplace_back(std::move(parsed->first));
        pos = skip_whitespace(text, parsed->second);
        if (pos >= text.size()) {
            return std::unexpected(make_manifest_error("unterminated array"));
        }
        if (text[pos] == ',') {
            ++pos;
            continue;
        }
        if (text[pos] == ']') {
            ++pos;
            break;
        }
        return std::unexpected(make_manifest_error("expected ',' or ']' in array"));
    }
    return std::optional<std::vector<std::string>>{std::move(values)};
}

} // namespace

namespace SP::UI {

FontManager::FontManager(PathSpace& space)
    : space_(&space) {
}

auto FontManager::register_font(App::AppRootPathView appRoot,
                                Builders::Resources::Fonts::RegisterFontParams const& params)
    -> SP::Expected<Builders::Resources::Fonts::FontResourcePaths> {
    auto registered = Builders::Resources::Fonts::Register(*space_, appRoot, params);
    if (!registered) {
        return registered;
    }

    MetricsSnapshot snapshot{};
    {
        std::lock_guard<std::mutex> lock{mutex_};
        auto key = make_font_registry_key(appRoot.getPath(), params.family, params.style);
        registered_fonts_.insert(std::move(key));
        snapshot.registered_fonts = static_cast<std::uint64_t>(registered_fonts_.size());
        snapshot.cache_hits = cache_hits_;
        snapshot.cache_misses = cache_misses_;
        snapshot.cache_evictions = cache_evictions_;
        snapshot.cache_size = lru_list_.size();
        snapshot.cache_capacity = cache_capacity_;
    }

    publish_metrics(appRoot, snapshot);
    return registered;
}

auto FontManager::resolve_font(App::AppRootPathView appRoot,
                               std::string_view family,
                               std::string_view style) -> SP::Expected<ResolvedFont> {
    if (space_ == nullptr) {
        return std::unexpected(SP::Error{SP::Error::Code::UnknownError,
                                         "FontManager requires a valid PathSpace"});
    }

    auto paths = Builders::Resources::Fonts::Resolve(appRoot, family, style);
    if (!paths) {
        return std::unexpected(paths.error());
    }

    auto manifest_text = space_->read<std::string, std::string>(paths->manifest.getPath());
    if (!manifest_text) {
        return std::unexpected(manifest_text.error());
    }

    std::string_view manifest_view{*manifest_text};
    auto trimmed = skip_whitespace(manifest_view, 0);
    if (trimmed >= manifest_view.size() || manifest_view[trimmed] != '{') {
        return std::unexpected(make_manifest_error("font manifest must begin with '{'"));
    }

    auto family_field = parse_string_field(manifest_view, "family");
    if (!family_field) {
        return std::unexpected(family_field.error());
    }
    auto style_field = parse_string_field(manifest_view, "style");
    if (!style_field) {
        return std::unexpected(style_field.error());
    }
    auto weight_field = parse_string_field(manifest_view, "weight");
    if (!weight_field) {
        return std::unexpected(weight_field.error());
    }
    auto fallback_field = parse_string_array_field(manifest_view, "fallback");
    if (!fallback_field) {
        return std::unexpected(fallback_field.error());
    }

    ResolvedFont resolved{};
    resolved.paths = *paths;
    resolved.family = family_field->value_or(std::string{family});
    resolved.style = style_field->value_or(std::string{style});
    resolved.weight = weight_field->value_or(std::string{"400"});
    if (resolved.family.empty()) {
        resolved.family = std::string{family};
    }
    if (resolved.style.empty()) {
        resolved.style = std::string{style};
    }
    if (resolved.weight.empty()) {
        resolved.weight = "400";
    }

    if (fallback_field->has_value()) {
        auto const& entries = fallback_field->value();
        std::unordered_set<std::string> seen{};
        for (auto const& candidate : entries) {
            if (candidate.empty()) {
                continue;
            }
            if (candidate == resolved.family) {
                continue;
            }
            if (seen.insert(candidate).second) {
                resolved.fallback_chain.emplace_back(candidate);
            }
        }
    }

    auto active_revision = space_->read<std::uint64_t, std::string>(paths->active_revision.getPath());
    if (active_revision) {
        resolved.active_revision = *active_revision;
    } else {
        auto const code = active_revision.error().code;
        if (code != SP::Error::Code::NoSuchPath && code != SP::Error::Code::NoObjectFound) {
            return std::unexpected(active_revision.error());
        }
    }

    return resolved;
}

auto FontManager::shape_text(App::AppRootPathView appRoot,
                             std::string_view text,
                             TypographyStyle const& typography) -> ShapedRun {
    MetricsSnapshot snapshot{};
    ShapedRun result{};

    {
        std::lock_guard<std::mutex> lock{mutex_};

        auto descriptor_fp = compute_descriptor_fingerprint(typography);
        auto cache_key = compute_cache_key(text, descriptor_fp);
        auto it = cache_.find(cache_key);
        if (it != cache_.end()) {
            auto& entry = *it->second;
            if (entry.text == text && entry.descriptor_fingerprint == descriptor_fp) {
                ++cache_hits_;
                lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
                result = entry.run;
            } else {
                ++cache_misses_;
                auto run = shape_text_unlocked(text, typography, descriptor_fp, cache_key);
                entry.text = std::string{text};
                entry.descriptor_fingerprint = descriptor_fp;
                entry.run = run;
                lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
                result = run;
            }
        } else {
            ++cache_misses_;
            auto run = shape_text_unlocked(text, typography, descriptor_fp, cache_key);
            CacheEntry entry{};
            entry.key = cache_key;
            entry.text = std::string{text};
            entry.descriptor_fingerprint = descriptor_fp;
            entry.run = run;
            lru_list_.emplace_front(std::move(entry));
            cache_[cache_key] = lru_list_.begin();
            if (lru_list_.size() > cache_capacity_) {
                auto& back = lru_list_.back();
                cache_.erase(back.key);
                lru_list_.pop_back();
                ++cache_evictions_;
            }
            result = run;
        }

        snapshot.registered_fonts = static_cast<std::uint64_t>(registered_fonts_.size());
        snapshot.cache_hits = cache_hits_;
        snapshot.cache_misses = cache_misses_;
        snapshot.cache_evictions = cache_evictions_;
        snapshot.cache_size = lru_list_.size();
        snapshot.cache_capacity = cache_capacity_;
    }

    publish_metrics(appRoot, snapshot);
    return result;
}

auto FontManager::metrics() const -> Metrics {
    std::lock_guard<std::mutex> lock{mutex_};
    Metrics m{};
    m.registered_fonts = static_cast<std::uint64_t>(registered_fonts_.size());
    m.cache_hits = cache_hits_;
    m.cache_misses = cache_misses_;
    m.cache_evictions = cache_evictions_;
    m.cache_size = lru_list_.size();
    m.cache_capacity = cache_capacity_;
    return m;
}

void FontManager::set_cache_capacity_for_testing(std::size_t capacity) {
    if (capacity == 0) {
        capacity = 1;
    }

    std::lock_guard<std::mutex> lock{mutex_};
    cache_capacity_ = capacity;
    while (lru_list_.size() > cache_capacity_) {
        auto& back = lru_list_.back();
        cache_.erase(back.key);
        lru_list_.pop_back();
        ++cache_evictions_;
    }
}

auto FontManager::compute_descriptor_fingerprint(
    TypographyStyle const& typography) const -> std::uint64_t {
    auto hash = kFNVOffset;
    hash = fnv_mix(hash, typography.font_resource_root);
    hash = fnv_mix(hash, typography.font_family);
    hash = fnv_mix(hash, typography.font_style);
    hash = fnv_mix(hash, typography.font_weight);
    hash = fnv_mix(hash, typography.language);
    hash = fnv_mix(hash, typography.direction);
    hash = fnv_mix(hash, typography.font_active_revision);
    hash = fnv_mix(hash, static_cast<std::uint64_t>(typography.font_size * 100.0f));
    hash = fnv_mix(hash, static_cast<std::uint64_t>(typography.line_height * 100.0f));
    hash = fnv_mix(hash, static_cast<std::uint64_t>(typography.letter_spacing * 100.0f));
    hash = fnv_mix(hash, static_cast<std::uint64_t>(typography.baseline_shift * 100.0f));
    for (auto const& fallback : typography.fallback_families) {
        hash = fnv_mix(hash, fallback);
    }
    for (auto const& feature : typography.font_features) {
        hash = fnv_mix(hash, feature);
    }
    return sanitize_cache_key(hash);
}

auto FontManager::compute_cache_key(std::string_view text,
                                    std::uint64_t descriptor_fingerprint) const -> std::uint64_t {
    auto hash = kFNVOffset;
    hash = fnv_mix(hash, descriptor_fingerprint);
    hash = fnv_mix(hash, static_cast<std::uint64_t>(text.size()));
    hash = fnv_mix(hash, text);
    return sanitize_cache_key(hash);
}

auto FontManager::shape_text_unlocked(std::string_view text,
                                      TypographyStyle const& typography,
                                      std::uint64_t descriptor_fingerprint,
                                      std::uint64_t cache_key) -> ShapedRun {
    ShapedRun run{};
    run.descriptor_fingerprint = descriptor_fingerprint;
    run.cache_key = cache_key;
    run.glyphs.reserve(text.size());

    float scale = std::max(kFallbackMinScale, typography.font_size / 16.0f);
    float advance_units = scale * kFallbackAdvanceUnits;
    float spacing = std::max(0.0f, typography.letter_spacing);
    float cursor = 0.0f;

    for (unsigned char byte : text) {
        GlyphPlacement glyph{};
        glyph.codepoint = static_cast<char32_t>(byte);
        glyph.glyph_id = static_cast<std::uint32_t>(glyph.codepoint);
        glyph.offset_x = cursor;
        glyph.offset_y = typography.baseline_shift;
        glyph.advance = advance_units;
        run.glyphs.emplace_back(glyph);
        cursor += advance_units + spacing;
    }

    if (!text.empty()) {
        cursor -= spacing;
    }
    run.total_advance = std::max(0.0f, cursor);
    return run;
}

void FontManager::publish_metrics(App::AppRootPathView appRoot,
                                  MetricsSnapshot const& snapshot) {
    if (space_ == nullptr) {
        return;
    }

    auto root_view = appRoot.getPath();
    if (root_view.empty()) {
        return;
    }

    std::string base{root_view};
    base += "/diagnostics/metrics/fonts";

    using SP::UI::Builders::Detail::replace_single;

    (void)replace_single<std::uint64_t>(*space_, base + "/registeredFonts", snapshot.registered_fonts);
    (void)replace_single<std::uint64_t>(*space_, base + "/cacheHits", snapshot.cache_hits);
    (void)replace_single<std::uint64_t>(*space_, base + "/cacheMisses", snapshot.cache_misses);
    (void)replace_single<std::uint64_t>(*space_, base + "/cacheEvictions", snapshot.cache_evictions);
    (void)replace_single<std::uint64_t>(*space_, base + "/cacheSize", static_cast<std::uint64_t>(snapshot.cache_size));
    (void)replace_single<std::uint64_t>(*space_, base + "/cacheCapacity",
                                        static_cast<std::uint64_t>(snapshot.cache_capacity));
}

} // namespace SP::UI
