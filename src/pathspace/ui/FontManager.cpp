#include <pathspace/ui/FontManager.hpp>

#include "BuildersDetail.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

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
