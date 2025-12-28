#include "SceneSnapshotBuilderDetail.hpp"

#include <pathspace/ui/RendererSnapshotStore.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <string>
#include <vector>

namespace SP::UI::Scene {

namespace {

constexpr std::string_view kBuildsSegment = "/builds/";

auto parse_revision_base(std::string const& revisionBase)
    -> Expected<std::pair<std::string, std::uint64_t>> {
    auto pos = revisionBase.rfind(kBuildsSegment);
    if (pos == std::string::npos) {
        return std::unexpected(make_error("revision base missing '/builds/' segment",
                                          Error::Code::InvalidPath));
    }
    auto scene_path = revisionBase.substr(0, pos);
    auto revision_text = revisionBase.substr(pos + kBuildsSegment.size());
    if (revision_text.empty()) {
        return std::unexpected(make_error("revision base missing revision component",
                                          Error::Code::InvalidPath));
    }
    std::uint64_t revision = 0;
    auto first = revision_text.data();
    auto last = first + revision_text.size();
    auto parsed = std::from_chars(first, last, revision);
    if (parsed.ec != std::errc() || parsed.ptr != last) {
        return std::unexpected(make_error("revision component is not numeric",
                                          Error::Code::InvalidPath));
    }
    return std::make_pair(scene_path, revision);
}

} // namespace

auto SceneSnapshotBuilder::decode_bucket(PathSpace const& space,
                                         std::string const& revisionBase) -> Expected<DrawableBucketSnapshot> {
    (void)space;
    auto parsed = parse_revision_base(revisionBase);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    auto bucket = RendererSnapshotStore::instance().get_bucket(parsed->first, parsed->second);
    if (!bucket) {
        return std::unexpected(bucket.error());
    }
    return *bucket;
}

auto SceneSnapshotBuilder::decode_metadata(std::span<std::byte const> bytes)
    -> Expected<SnapshotMetadata> {
    std::vector<std::uint8_t> buffer(bytes.size());
    std::transform(bytes.begin(), bytes.end(), buffer.begin(), [](std::byte b) {
        return static_cast<std::uint8_t>(b);
    });
    auto decoded = from_bytes<EncodedSnapshotMetadata>(buffer);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    SnapshotMetadata meta{};
    meta.author = std::move(decoded->author);
    meta.tool_version = std::move(decoded->tool_version);
    meta.created_at = std::chrono::system_clock::time_point{std::chrono::milliseconds{decoded->created_at_ms}};
    meta.drawable_count = static_cast<std::size_t>(decoded->drawable_count);
    meta.command_count = static_cast<std::size_t>(decoded->command_count);
    meta.fingerprint_digests = std::move(decoded->fingerprint_digests);
    return meta;
}

} // namespace SP::UI::Scene
