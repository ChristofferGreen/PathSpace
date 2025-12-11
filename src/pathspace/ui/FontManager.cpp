#include <pathspace/ui/FontManager.hpp>

#include "RuntimeDetail.hpp"

#include <algorithm>
#include <cmath>
#include <charconv>
#include <limits>
#include <optional>
#include <utility>
#include <unordered_set>

#if defined(PATHSPACE_HAS_HARFBUZZ)
#include <hb.h>
#if defined(__APPLE__)
#include <hb-coretext.h>
#include <CoreText/CoreText.h>
#endif
#endif

namespace {

constexpr std::uint64_t kFNVOffset = 1469598103934665603ull;
constexpr std::uint64_t kFNVPrime = 1099511628211ull;
constexpr float kFallbackAdvanceUnits = 8.0f;
constexpr float kFallbackMinScale = 0.1f;
constexpr std::uint64_t kDefaultAtlasSoftBytes = 4ull * 1024ull * 1024ull;
constexpr std::uint64_t kDefaultAtlasHardBytes = 8ull * 1024ull * 1024ull;
constexpr std::uint64_t kDefaultApproxRunBytes = 512ull;

struct AtlasBudget {
    std::uint64_t soft_bytes = kDefaultAtlasSoftBytes;
    std::uint64_t hard_bytes = kDefaultAtlasHardBytes;
    std::uint64_t approx_run_bytes = kDefaultApproxRunBytes;
};

using SP::PathSpace;
using SP::UI::FontManager;

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

auto sanitize_fallbacks(std::vector<std::string> fallbacks,
                        std::string_view primary_family) -> std::vector<std::string> {
    std::vector<std::string> sanitized;
    sanitized.reserve(fallbacks.size());
    std::unordered_set<std::string> seen{};

    for (auto& entry : fallbacks) {
        if (entry.empty()) {
            continue;
        }
        if (entry == primary_family) {
            continue;
        }
        if (seen.insert(entry).second) {
            sanitized.emplace_back(entry);
        }
    }

    if (sanitized.empty()) {
        sanitized.emplace_back("system-ui");
    }

    return sanitized;
}

auto load_string_with_default(PathSpace& space,
                              std::string const& path,
                              std::string_view fallback) -> SP::Expected<std::string> {
    auto value = space.read<std::string, std::string>(path);
    if (value) {
        if (value->empty()) {
            return std::string{fallback};
        }
        return *value;
    }
    auto const code = value.error().code;
    if (code == SP::Error::Code::NoSuchPath || code == SP::Error::Code::NoObjectFound) {
        return std::string{fallback};
    }
    return std::unexpected(value.error());
}

auto load_string_vector(PathSpace& space,
                        std::string const& path) -> SP::Expected<std::vector<std::string>> {
    auto value = space.read<std::vector<std::string>, std::string>(path);
    if (value) {
        return *value;
    }
    auto const code = value.error().code;
    if (code == SP::Error::Code::NoSuchPath || code == SP::Error::Code::NoObjectFound) {
        return std::vector<std::string>{};
    }
    return std::unexpected(value.error());
}

auto load_atlas_budget(PathSpace& space,
                       std::string const& meta_base) -> SP::Expected<std::optional<AtlasBudget>> {
    AtlasBudget budget{};
    bool found = false;
    auto atlas_base = meta_base + "/atlas";

    if (auto soft = space.read<std::uint64_t, std::string>(atlas_base + "/softBytes"); soft) {
        budget.soft_bytes = *soft;
        found = true;
    } else {
        auto const code = soft.error().code;
        if (code != SP::Error::Code::NoSuchPath && code != SP::Error::Code::NoObjectFound) {
            return std::unexpected(soft.error());
        }
    }

    if (auto hard = space.read<std::uint64_t, std::string>(atlas_base + "/hardBytes"); hard) {
        budget.hard_bytes = *hard;
        found = true;
    } else {
        auto const code = hard.error().code;
        if (code != SP::Error::Code::NoSuchPath && code != SP::Error::Code::NoObjectFound) {
            return std::unexpected(hard.error());
        }
    }

    if (auto approx = space.read<std::uint64_t, std::string>(atlas_base + "/shapedRunApproxBytes"); approx) {
        budget.approx_run_bytes = std::max<std::uint64_t>(1, *approx);
        found = true;
    } else {
        auto const code = approx.error().code;
        if (code != SP::Error::Code::NoSuchPath && code != SP::Error::Code::NoObjectFound) {
            return std::unexpected(approx.error());
        }
    }

    if (!found) {
        return std::optional<AtlasBudget>{};
    }

    budget.approx_run_bytes = std::max<std::uint64_t>(budget.approx_run_bytes, 1);
    budget.soft_bytes = std::max<std::uint64_t>(budget.soft_bytes, budget.approx_run_bytes);
    budget.hard_bytes = std::max<std::uint64_t>(budget.hard_bytes, budget.soft_bytes);

    return std::optional<AtlasBudget>{budget};
}

#if defined(PATHSPACE_HAS_HARFBUZZ)

auto to_lower_copy(std::string_view value) -> std::string {
    std::string out(value.begin(), value.end());
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

auto equals_ignore_case(std::string_view lhs, std::string_view rhs) -> bool {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
            std::tolower(static_cast<unsigned char>(rhs[i]))) {
            return false;
        }
    }
    return true;
}

auto parse_css_weight(std::string_view weight) -> double {
    int value = 400;
    auto begin = weight.data();
    auto end = weight.data() + weight.size();
    auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{}) {
        auto lowered = to_lower_copy(weight);
        if (lowered == "thin") {
            value = 100;
        } else if (lowered == "extralight" || lowered == "ultralight") {
            value = 200;
        } else if (lowered == "light") {
            value = 300;
        } else if (lowered == "normal" || lowered == "regular") {
            value = 400;
        } else if (lowered == "medium") {
            value = 500;
        } else if (lowered == "semibold" || lowered == "demibold") {
            value = 600;
        } else if (lowered == "bold") {
            value = 700;
        } else if (lowered == "extrabold" || lowered == "ultrabold") {
            value = 800;
        } else if (lowered == "black" || lowered == "heavy") {
            value = 900;
        }
    }
    value = std::clamp(value, 100, 900);
    return static_cast<double>(value);
}

auto is_italic_style(std::string_view style) -> bool {
    auto lowered = to_lower_copy(style);
    return lowered == "italic" || lowered == "oblique";
}

auto parse_atlas_format(std::string_view value) -> SP::UI::FontAtlasFormat {
    auto lowered = to_lower_copy(value);
    if (lowered == "rgba8" || lowered == "rgba" || lowered == "color") {
        return SP::UI::FontAtlasFormat::Rgba8;
    }
    return SP::UI::FontAtlasFormat::Alpha8;
}

auto build_family_candidates(std::string_view family,
                             std::string_view style) -> std::vector<std::string> {
    std::vector<std::string> candidates;
    candidates.emplace_back(family);

    if (!style.empty() && !equals_ignore_case(style, "normal")) {
        std::string variant{family};
        variant.push_back('-');
        variant.append(style.begin(), style.end());
        candidates.emplace_back(std::move(variant));

        std::string spaced{family};
        spaced.push_back(' ');
        spaced.append(style.begin(), style.end());
        candidates.emplace_back(std::move(spaced));
    }

    if (equals_ignore_case(family, "system-ui")) {
        candidates.emplace_back(".AppleSystemUIFont");
        candidates.emplace_back("San Francisco");
    } else if (equals_ignore_case(family, "sans-serif")) {
        candidates.emplace_back("Helvetica Neue");
        candidates.emplace_back("Helvetica");
    } else if (equals_ignore_case(family, "serif")) {
        candidates.emplace_back("Times New Roman");
        candidates.emplace_back("Times");
    } else if (equals_ignore_case(family, "monospace") || equals_ignore_case(family, "monospaced")) {
        candidates.emplace_back("Menlo");
        candidates.emplace_back("Monaco");
    }

    // Always fall back to system sans-serif.
    candidates.emplace_back(".AppleSystemUIFont");
    candidates.emplace_back("Helvetica Neue");

    // Deduplicate while preserving order.
    std::vector<std::string> unique;
    std::unordered_set<std::string> seen;
    unique.reserve(candidates.size());
    for (auto& entry : candidates) {
        if (seen.insert(to_lower_copy(entry)).second) {
            unique.emplace_back(std::move(entry));
        }
    }
    return unique;
}

auto to_hb_direction(std::string_view direction) -> hb_direction_t {
    if (equals_ignore_case(direction, "rtl")) {
        return HB_DIRECTION_RTL;
    }
    if (equals_ignore_case(direction, "ttb") || equals_ignore_case(direction, "ttb-ltr")) {
        return HB_DIRECTION_TTB;
    }
    if (equals_ignore_case(direction, "btt")) {
        return HB_DIRECTION_BTT;
    }
    return HB_DIRECTION_INVALID;
}

auto make_feature_vector(std::vector<std::string> const& features) -> std::vector<hb_feature_t> {
    std::vector<hb_feature_t> parsed;
    parsed.reserve(features.size());
    for (auto const& feature : features) {
        hb_feature_t hb_feature{};
        if (hb_feature_from_string(feature.c_str(), static_cast<int>(feature.size()), &hb_feature)) {
            parsed.emplace_back(hb_feature);
        }
    }
    return parsed;
}

#if defined(__APPLE__)

auto create_font_descriptor(std::string const& family_name,
                            double weight_value,
                            bool italic) -> CTFontDescriptorRef {
    CFStringRef family = CFStringCreateWithCString(nullptr, family_name.c_str(), kCFStringEncodingUTF8);
    if (!family) {
        return nullptr;
    }

    CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
        nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!attributes) {
        CFRelease(family);
        return nullptr;
    }

    CFDictionarySetValue(attributes, kCTFontFamilyNameAttribute, family);
    CFRelease(family);

    CFMutableDictionaryRef traits = CFDictionaryCreateMutable(
        nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (traits) {
        auto normalized_weight = std::clamp((weight_value - 400.0) / 400.0, -1.0, 1.0);
        CFNumberRef weight = CFNumberCreate(nullptr, kCFNumberDoubleType, &normalized_weight);
        if (weight) {
            CFDictionarySetValue(traits, kCTFontWeightTrait, weight);
            CFRelease(weight);
        }

        if (italic) {
            double slant = 1.0;
            CFNumberRef slant_value = CFNumberCreate(nullptr, kCFNumberDoubleType, &slant);
            if (slant_value) {
                CFDictionarySetValue(traits, kCTFontSlantTrait, slant_value);
                CFRelease(slant_value);
            }
        }

        CFDictionarySetValue(attributes, kCTFontTraitsAttribute, traits);
        CFRelease(traits);
    }

    CTFontDescriptorRef descriptor = CTFontDescriptorCreateWithAttributes(attributes);
    CFRelease(attributes);
    return descriptor;
}

auto create_ct_font_for_candidate(std::string const& candidate,
                                  std::string_view style,
                                  double weight_value,
                                  float point_size) -> CTFontRef {
    bool italic = is_italic_style(style);

    auto descriptor = create_font_descriptor(candidate, weight_value, italic);
    if (descriptor) {
        CTFontRef font = CTFontCreateWithFontDescriptor(descriptor, point_size, nullptr);
        CFRelease(descriptor);
        if (font) {
            return font;
        }
    }

    CFStringRef name = CFStringCreateWithCString(nullptr, candidate.c_str(), kCFStringEncodingUTF8);
    if (!name) {
        return nullptr;
    }
    CTFontRef direct = CTFontCreateWithName(name, point_size, nullptr);
    CFRelease(name);
    return direct;
}

auto create_hb_font_for_typography(FontManager::TypographyStyle const& typography) -> hb_font_t* {
    if (equals_ignore_case(typography.font_family, "PathSpaceSans")) {
        return nullptr;
    }

    auto weight = parse_css_weight(typography.font_weight);
    auto primary_candidates = build_family_candidates(typography.font_family, typography.font_style);

    std::vector<std::string> all_candidates;
    all_candidates.reserve(primary_candidates.size() + typography.fallback_families.size() * 3);
    all_candidates.insert(all_candidates.end(), primary_candidates.begin(), primary_candidates.end());

    for (auto const& fallback_family : typography.fallback_families) {
        auto variants = build_family_candidates(fallback_family, typography.font_style);
        all_candidates.insert(all_candidates.end(), variants.begin(), variants.end());
    }

    std::unordered_set<std::string> tried;

    for (auto const& candidate : all_candidates) {
        if (!tried.insert(to_lower_copy(candidate)).second) {
            continue;
        }

        if (candidate.empty()) {
            continue;
        }

        CTFontRef ct_font = create_ct_font_for_candidate(candidate, typography.font_style, weight,
                                                         typography.font_size);
        if (!ct_font) {
            continue;
        }

        hb_font_t* hb_font = hb_coretext_font_create(ct_font);
        CFRelease(ct_font);
        if (!hb_font) {
            continue;
        }

        auto scale = static_cast<int>(std::llround(static_cast<double>(typography.font_size) * 64.0));
        if (scale <= 0) {
            scale = 64;
        }

        hb_font_set_scale(hb_font, scale, scale);
        hb_font_set_ppem(hb_font,
                         static_cast<unsigned int>(std::max(1.0f, typography.font_size)),
                         static_cast<unsigned int>(std::max(1.0f, typography.font_size)));
        hb_font_set_ptem(hb_font, typography.font_size);
        hb_font_make_immutable(hb_font);
        return hb_font;
    }

    return nullptr;
}

#else

auto create_hb_font_for_typography(FontManager::TypographyStyle const&) -> hb_font_t* {
    return nullptr;
}

#endif // __APPLE__

auto shape_text_with_harfbuzz(std::string_view text,
                              FontManager::TypographyStyle const& typography,
                              std::uint64_t descriptor_fingerprint,
                              std::uint64_t cache_key) -> std::optional<FontManager::ShapedRun> {
    if (text.empty()) {
        FontManager::ShapedRun empty{};
        empty.descriptor_fingerprint = descriptor_fingerprint;
        empty.cache_key = cache_key;
        return empty;
    }

    hb_font_t* hb_font = create_hb_font_for_typography(typography);
    if (hb_font == nullptr) {
        return std::nullopt;
    }

    hb_buffer_t* buffer = hb_buffer_create();
    hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()), 0, static_cast<int>(text.size()));
    hb_buffer_guess_segment_properties(buffer);

    if (!typography.language.empty()) {
        hb_buffer_set_language(buffer, hb_language_from_string(typography.language.c_str(),
                                                               static_cast<int>(typography.language.size())));
    }

    if (!typography.direction.empty()) {
        auto dir = to_hb_direction(typography.direction);
        if (dir != HB_DIRECTION_INVALID) {
            hb_buffer_set_direction(buffer, dir);
        }
    }

    auto features = make_feature_vector(typography.font_features);
    hb_shape(hb_font, buffer, features.empty() ? nullptr : features.data(),
             static_cast<unsigned int>(features.size()));

    unsigned int glyph_count = hb_buffer_get_length(buffer);
    auto infos = hb_buffer_get_glyph_infos(buffer, nullptr);
    auto positions = hb_buffer_get_glyph_positions(buffer, nullptr);

    FontManager::ShapedRun run{};
    run.descriptor_fingerprint = descriptor_fingerprint;
    run.cache_key = cache_key;
    run.glyphs.reserve(glyph_count);

    float cursor = 0.0f;
    float spacing = std::max(0.0f, typography.letter_spacing);

    for (unsigned int i = 0; i < glyph_count; ++i) {
        auto const& info = infos[i];
        auto const& pos = positions[i];

        float x_advance = static_cast<float>(pos.x_advance) / 64.0f;
        float x_offset = static_cast<float>(pos.x_offset) / 64.0f;
        float y_offset = static_cast<float>(pos.y_offset) / 64.0f;

        FontManager::GlyphPlacement glyph{};
        glyph.glyph_id = info.codepoint;
        glyph.codepoint = static_cast<char32_t>(info.codepoint);
        glyph.offset_x = cursor + x_offset;
        glyph.offset_y = typography.baseline_shift - y_offset;
        glyph.advance = x_advance;
        run.glyphs.emplace_back(glyph);

        cursor += x_advance + spacing;
    }

    if (!run.glyphs.empty()) {
        cursor -= spacing;
    }

    run.total_advance = cursor;

    hb_buffer_destroy(buffer);
    hb_font_destroy(hb_font);

    if (run.glyphs.empty()) {
        return std::nullopt;
    }
    return run;
}

#endif // PATHSPACE_HAS_HARFBUZZ

} // namespace

namespace SP::UI {

FontManager::FontManager(PathSpace& space)
    : space_(&space) {
    shaped_run_approx_bytes_ = kDefaultApproxRunBytes;
    atlas_soft_bytes_ = kDefaultAtlasSoftBytes;
    atlas_hard_bytes_ = kDefaultAtlasHardBytes;
    cache_hard_capacity_ = std::max<std::size_t>(1, static_cast<std::size_t>(atlas_hard_bytes_ / shaped_run_approx_bytes_));
    cache_capacity_ = std::max<std::size_t>(1, static_cast<std::size_t>(atlas_soft_bytes_ / shaped_run_approx_bytes_));
}

auto FontManager::register_font(App::AppRootPathView appRoot,
                                Runtime::Resources::Fonts::RegisterFontParams const& params)
    -> SP::Expected<Runtime::Resources::Fonts::FontResourcePaths> {
    auto registered = Runtime::Resources::Fonts::Register(*space_, appRoot, params);
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
        snapshot.cache_hard_capacity = cache_hard_capacity_;
        snapshot.atlas_soft_bytes = atlas_soft_bytes_;
        snapshot.atlas_hard_bytes = atlas_hard_bytes_;
        snapshot.shaped_run_approx_bytes = shaped_run_approx_bytes_;
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

    auto paths = Runtime::Resources::Fonts::Resolve(appRoot, family, style);
    if (!paths) {
        return std::unexpected(paths.error());
    }

    auto const meta_base = std::string(paths->meta.getPath());
    auto family_result = load_string_with_default(*space_, meta_base + "/family", family);
    if (!family_result) {
        return std::unexpected(family_result.error());
    }
    auto style_result = load_string_with_default(*space_, meta_base + "/style", style);
    if (!style_result) {
        return std::unexpected(style_result.error());
    }
    auto weight_result = load_string_with_default(*space_, meta_base + "/weight", "400");
    if (!weight_result) {
        return std::unexpected(weight_result.error());
    }
    auto fallback_values = load_string_vector(*space_, meta_base + "/fallbacks");
    if (!fallback_values) {
        return std::unexpected(fallback_values.error());
    }

    ResolvedFont resolved{};
    resolved.paths = *paths;
    resolved.family = std::move(*family_result);
    resolved.style = std::move(*style_result);
    resolved.weight = std::move(*weight_result);
    auto fallbacks = std::move(*fallback_values);
    resolved.fallback_chain = sanitize_fallbacks(std::move(fallbacks), resolved.family);

    auto preferred_format = load_string_with_default(*space_,
                                                     meta_base + "/atlas/preferredFormat",
                                                     "alpha8");
    if (!preferred_format) {
        return std::unexpected(preferred_format.error());
    }
    resolved.preferred_format = parse_atlas_format(*preferred_format);

    auto has_color_path = meta_base + "/atlas/hasColor";
    auto has_color = space_->read<std::uint64_t, std::string>(has_color_path);
    if (has_color) {
        resolved.has_color_atlas = (*has_color != 0);
    } else {
        auto const code = has_color.error().code;
        if (code != SP::Error::Code::NoSuchPath && code != SP::Error::Code::NoObjectFound) {
            return std::unexpected(has_color.error());
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

    auto descriptor_fp = compute_descriptor_fingerprint(typography);
    auto cache_key = compute_cache_key(text, descriptor_fp);

    std::optional<AtlasBudget> budget;
    if (space_ != nullptr && !typography.font_resource_root.empty()) {
        auto meta_base = typography.font_resource_root + "/meta";
        if (auto loaded = load_atlas_budget(*space_, meta_base)) {
            budget = std::move(*loaded);
        }
    }

    {
        std::lock_guard<std::mutex> lock{mutex_};

        if (budget) {
            apply_budget_locked(budget->soft_bytes, budget->hard_bytes, budget->approx_run_bytes);
        }

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
            while (lru_list_.size() > cache_capacity_) {
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
        snapshot.cache_hard_capacity = cache_hard_capacity_;
        snapshot.atlas_soft_bytes = atlas_soft_bytes_;
        snapshot.atlas_hard_bytes = atlas_hard_bytes_;
        snapshot.shaped_run_approx_bytes = shaped_run_approx_bytes_;
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
    m.cache_hard_capacity = cache_hard_capacity_;
    m.atlas_soft_bytes = atlas_soft_bytes_;
    m.atlas_hard_bytes = atlas_hard_bytes_;
    m.shaped_run_approx_bytes = shaped_run_approx_bytes_;
    return m;
}

void FontManager::set_cache_capacity_for_testing(std::size_t capacity) {
    if (capacity == 0) {
        capacity = 1;
    }

    std::lock_guard<std::mutex> lock{mutex_};
    cache_hard_capacity_ = capacity;
    cache_capacity_ = capacity;
    atlas_soft_bytes_ = static_cast<std::uint64_t>(capacity) * shaped_run_approx_bytes_;
    atlas_hard_bytes_ = atlas_soft_bytes_;
    while (lru_list_.size() > cache_capacity_) {
        auto& back = lru_list_.back();
        cache_.erase(back.key);
        lru_list_.pop_back();
        ++cache_evictions_;
    }
}

void FontManager::apply_budget_locked(std::uint64_t soft_bytes,
                                      std::uint64_t hard_bytes,
                                      std::uint64_t approx_bytes) {
    auto approx = std::max<std::uint64_t>(1, approx_bytes);
    auto soft_cap = static_cast<std::size_t>(std::max<std::uint64_t>(1, soft_bytes / approx));
    auto hard_cap = static_cast<std::size_t>(std::max<std::uint64_t>(1, hard_bytes / approx));
    if (hard_cap < soft_cap) {
        hard_cap = soft_cap;
    }

    atlas_soft_bytes_ = soft_bytes;
    atlas_hard_bytes_ = hard_bytes;
    shaped_run_approx_bytes_ = approx;
    cache_hard_capacity_ = std::max<std::size_t>(1, hard_cap);
    cache_capacity_ = std::max<std::size_t>(1, std::min(soft_cap, cache_hard_capacity_));

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
#if defined(PATHSPACE_HAS_HARFBUZZ)
    if (auto shaped = shape_text_with_harfbuzz(text, typography, descriptor_fingerprint, cache_key)) {
        return *shaped;
    }
#endif

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

    using SP::UI::Runtime::Detail::replace_single;

    (void)replace_single<std::uint64_t>(*space_, base + "/registeredFonts", snapshot.registered_fonts);
    (void)replace_single<std::uint64_t>(*space_, base + "/cacheHits", snapshot.cache_hits);
    (void)replace_single<std::uint64_t>(*space_, base + "/cacheMisses", snapshot.cache_misses);
    (void)replace_single<std::uint64_t>(*space_, base + "/cacheEvictions", snapshot.cache_evictions);
    (void)replace_single<std::uint64_t>(*space_, base + "/cacheSize", static_cast<std::uint64_t>(snapshot.cache_size));
    (void)replace_single<std::uint64_t>(*space_, base + "/cacheCapacity",
                                        static_cast<std::uint64_t>(snapshot.cache_capacity));
    (void)replace_single<std::uint64_t>(*space_, base + "/cacheHardCapacity",
                                        static_cast<std::uint64_t>(snapshot.cache_hard_capacity));
    (void)replace_single<std::uint64_t>(*space_, base + "/atlasSoftBytes", snapshot.atlas_soft_bytes);
    (void)replace_single<std::uint64_t>(*space_, base + "/atlasHardBytes", snapshot.atlas_hard_bytes);
    (void)replace_single<std::uint64_t>(*space_, base + "/shapedRunApproxBytes",
                                        snapshot.shaped_run_approx_bytes);
}

} // namespace SP::UI
