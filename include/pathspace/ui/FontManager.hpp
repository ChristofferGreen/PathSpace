#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>
#include <pathspace/ui/FontAtlas.hpp>

#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SP::UI {

class FontManager {
public:
    using TypographyStyle = Runtime::Widgets::TypographyStyle;

    struct ResolvedFont {
        Runtime::Resources::Fonts::FontResourcePaths paths;
        std::string family;
        std::string style;
        std::string weight;
        std::vector<std::string> fallback_chain;
        std::uint64_t active_revision = 0;
        FontAtlasFormat preferred_format = FontAtlasFormat::Alpha8;
        bool has_color_atlas = false;
    };

    struct GlyphPlacement {
        std::uint32_t glyph_id = 0;
        char32_t codepoint = U'\0';
        float advance = 0.0f;
        float offset_x = 0.0f;
        float offset_y = 0.0f;
    };

    struct ShapedRun {
        std::vector<GlyphPlacement> glyphs;
        float total_advance = 0.0f;
        std::uint64_t descriptor_fingerprint = 0;
        std::uint64_t cache_key = 0;
    };

    struct Metrics {
        std::uint64_t registered_fonts = 0;
        std::uint64_t cache_hits = 0;
        std::uint64_t cache_misses = 0;
        std::uint64_t cache_evictions = 0;
        std::size_t cache_size = 0;
        std::size_t cache_capacity = 0;
        std::size_t cache_hard_capacity = 0;
        std::uint64_t atlas_soft_bytes = 0;
        std::uint64_t atlas_hard_bytes = 0;
        std::uint64_t shaped_run_approx_bytes = 0;
    };

    explicit FontManager(PathSpace& space);

    auto register_font(App::AppRootPathView appRoot,
                       Runtime::Resources::Fonts::RegisterFontParams const& params)
        -> SP::Expected<Runtime::Resources::Fonts::FontResourcePaths>;

    auto resolve_font(App::AppRootPathView appRoot,
                      std::string_view family,
                      std::string_view style) -> SP::Expected<ResolvedFont>;

    auto shape_text(App::AppRootPathView appRoot,
                    std::string_view text,
                    TypographyStyle const& typography) -> ShapedRun;

    auto metrics() const -> Metrics;

    void set_cache_capacity_for_testing(std::size_t capacity);

private:
    struct CacheEntry {
        std::uint64_t key = 0;
        std::string text;
        std::uint64_t descriptor_fingerprint = 0;
        ShapedRun run;
    };

    struct MetricsSnapshot {
        std::uint64_t registered_fonts = 0;
        std::uint64_t cache_hits = 0;
        std::uint64_t cache_misses = 0;
        std::uint64_t cache_evictions = 0;
        std::size_t cache_size = 0;
        std::size_t cache_capacity = 0;
        std::size_t cache_hard_capacity = 0;
        std::uint64_t atlas_soft_bytes = 0;
        std::uint64_t atlas_hard_bytes = 0;
        std::uint64_t shaped_run_approx_bytes = 0;
    };

    auto compute_descriptor_fingerprint(TypographyStyle const& typography) const -> std::uint64_t;
    auto compute_cache_key(std::string_view text, std::uint64_t descriptor_fingerprint) const -> std::uint64_t;
    auto shape_text_unlocked(std::string_view text,
                             TypographyStyle const& typography,
                             std::uint64_t descriptor_fingerprint,
                             std::uint64_t cache_key) -> ShapedRun;
    void publish_metrics(App::AppRootPathView appRoot, MetricsSnapshot const& snapshot);
    void apply_budget_locked(std::uint64_t soft_bytes, std::uint64_t hard_bytes, std::uint64_t approx_bytes);

    PathSpace* space_;
    mutable std::mutex mutex_;
    std::size_t cache_capacity_ = 256;
    std::size_t cache_hard_capacity_ = 512;
    std::uint64_t atlas_soft_bytes_ = 0;
    std::uint64_t atlas_hard_bytes_ = 0;
    std::uint64_t shaped_run_approx_bytes_ = 1;
    using CacheList = std::list<CacheEntry>;
    CacheList lru_list_;
    std::unordered_map<std::uint64_t, CacheList::iterator> cache_;
    std::unordered_set<std::string> registered_fonts_;
    std::uint64_t cache_hits_ = 0;
    std::uint64_t cache_misses_ = 0;
    std::uint64_t cache_evictions_ = 0;
};

} // namespace SP::UI
