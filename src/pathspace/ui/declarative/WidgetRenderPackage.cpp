#include <pathspace/ui/declarative/WidgetRenderPackage.hpp>

#include <pathspace/type/SlidingBuffer.hpp>
#include <pathspace/type/serialization.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

namespace {

constexpr std::uint32_t kRenderPackageMagic = 0x5752504Bu; // 'WRPK'
constexpr std::uint16_t kRenderPackageVersion = 1u;

using BufferSpan = std::span<uint8_t const>;

template <typename T>
inline void write_pod(std::vector<uint8_t>& out, T value) {
    auto const* data = reinterpret_cast<uint8_t const*>(&value);
    out.insert(out.end(), data, data + sizeof(T));
}

template <typename T>
inline auto write_vector(std::vector<uint8_t>& out, std::vector<T> const& values)
    -> std::optional<SP::Error> {
    static_assert(std::is_trivially_copyable_v<T>, "write_vector requires trivially copyable type");
    if (values.size() > std::numeric_limits<std::uint32_t>::max()) {
        return SP::Error{SP::Error::Code::CapacityExceeded, "Vector too large for serialization"};
    }
    auto const count = static_cast<std::uint32_t>(values.size());
    write_pod(out, count);
    if (!values.empty()) {
        auto const* data = reinterpret_cast<uint8_t const*>(values.data());
        out.insert(out.end(), data, data + sizeof(T) * values.size());
    }
    return std::nullopt;
}

inline auto write_bytes(std::vector<uint8_t>& out, std::vector<std::uint8_t> const& bytes)
    -> std::optional<SP::Error> {
    if (bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
        return SP::Error{SP::Error::Code::CapacityExceeded, "Payload too large for serialization"};
    }
    write_pod(out, static_cast<std::uint32_t>(bytes.size()));
    if (!bytes.empty()) {
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    return std::nullopt;
}

inline void write_dirty_rect(std::vector<uint8_t>& out, SP::UI::Runtime::DirtyRectHint const& rect) {
    write_pod(out, rect.min_x);
    write_pod(out, rect.min_y);
    write_pod(out, rect.max_x);
    write_pod(out, rect.max_y);
}

inline auto write_surfaces(std::vector<uint8_t>& out,
                           std::vector<SP::UI::Declarative::WidgetSurface> const& surfaces)
    -> std::optional<SP::Error> {
    if (surfaces.size() > std::numeric_limits<std::uint32_t>::max()) {
        return SP::Error{SP::Error::Code::CapacityExceeded, "Surface list too large"};
    }
    write_pod(out, static_cast<std::uint32_t>(surfaces.size()));
    for (auto const& surface : surfaces) {
        write_pod(out, static_cast<std::uint8_t>(surface.kind));
        write_pod(out, static_cast<std::uint32_t>(surface.flags));
        write_pod(out, surface.width);
        write_pod(out, surface.height);
        write_pod(out, surface.fingerprint);
        for (auto value : surface.logical_bounds) {
            write_pod(out, value);
        }
    }
    return std::nullopt;
}

inline auto append_payload(SP::SlidingBuffer& buffer, std::vector<uint8_t> const& payload)
    -> std::optional<SP::Error> {
    if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        return SP::Error{SP::Error::Code::CapacityExceeded, "Serialized payload exceeds 4 GiB"};
    }
    SP::Header header{static_cast<std::uint32_t>(payload.size())};
    buffer.append(reinterpret_cast<uint8_t const*>(&header), sizeof(header));
    if (!payload.empty()) {
        buffer.append(payload.data(), payload.size());
    }
    return std::nullopt;
}

inline auto extract_payload(SP::SlidingBuffer const& buffer)
    -> SP::Expected<std::pair<BufferSpan, size_t>> {
    if (buffer.size() < sizeof(SP::Header)) {
        return std::unexpected(SP::Error{SP::Error::Code::MalformedInput, "Missing render package header"});
    }
    SP::Header header{};
    std::memcpy(&header, buffer.data(), sizeof(header));
    auto total = static_cast<size_t>(sizeof(header)) + static_cast<size_t>(header.size);
    if (buffer.size() < total) {
        return std::unexpected(SP::Error{SP::Error::Code::MalformedInput, "Render package payload truncated"});
    }
    BufferSpan span(buffer.data() + sizeof(header), header.size);
    return std::pair<BufferSpan, size_t>{span, total};
}

struct PayloadReader {
    BufferSpan data;
    size_t     offset = 0;

    template <typename T>
    auto read_pod() -> SP::Expected<T> {
        if (offset + sizeof(T) > data.size()) {
            return std::unexpected(SP::Error{SP::Error::Code::MalformedInput, "Render package truncated"});
        }
        T value{};
        std::memcpy(&value, data.data() + offset, sizeof(T));
        offset += sizeof(T);
        return value;
    }

    auto read_bytes(std::vector<std::uint8_t>& out) -> SP::Expected<void> {
        auto length = read_pod<std::uint32_t>();
        if (!length) {
            return std::unexpected(length.error());
        }
        if (offset + *length > data.size()) {
            return std::unexpected(SP::Error{SP::Error::Code::MalformedInput, "Render package bytes exceed payload"});
        }
        out.resize(*length);
        if (*length > 0) {
            std::memcpy(out.data(), data.data() + offset, *length);
        }
        offset += *length;
        return {};
    }

    template <typename T>
    auto read_vector(std::vector<T>& out) -> SP::Expected<void> {
        static_assert(std::is_trivially_copyable_v<T>, "read_vector requires trivially copyable type");
        auto count = read_pod<std::uint32_t>();
        if (!count) {
            return std::unexpected(count.error());
        }
        auto required = static_cast<size_t>(*count) * sizeof(T);
        if (offset + required > data.size()) {
            return std::unexpected(SP::Error{SP::Error::Code::MalformedInput, "Render package vector exceeds payload"});
        }
        out.resize(*count);
        if (required > 0) {
            std::memcpy(out.data(), data.data() + offset, required);
        }
        offset += required;
        return {};
    }
};

auto parse_surfaces(PayloadReader& reader)
    -> SP::Expected<std::vector<SP::UI::Declarative::WidgetSurface>> {
    auto count = reader.read_pod<std::uint32_t>();
    if (!count) {
        return std::unexpected(count.error());
    }
    std::vector<SP::UI::Declarative::WidgetSurface> surfaces;
    surfaces.reserve(*count);
    for (std::uint32_t index = 0; index < *count; ++index) {
        auto kind_raw = reader.read_pod<std::uint8_t>();
        if (!kind_raw) return std::unexpected(kind_raw.error());
        if (*kind_raw > static_cast<std::uint8_t>(SP::UI::Declarative::WidgetSurfaceKind::External)) {
            return std::unexpected(SP::Error{SP::Error::Code::UnserializableType, "Unknown surface kind"});
        }
        auto flags_raw = reader.read_pod<std::uint32_t>();
        if (!flags_raw) return std::unexpected(flags_raw.error());
        auto width = reader.read_pod<std::uint32_t>();
        if (!width) return std::unexpected(width.error());
        auto height = reader.read_pod<std::uint32_t>();
        if (!height) return std::unexpected(height.error());
        auto fingerprint = reader.read_pod<std::uint64_t>();
        if (!fingerprint) return std::unexpected(fingerprint.error());

        SP::UI::Declarative::WidgetSurface surface{};
        surface.kind = static_cast<SP::UI::Declarative::WidgetSurfaceKind>(*kind_raw);
        surface.flags = static_cast<SP::UI::Declarative::WidgetSurfaceFlags>(*flags_raw);
        surface.width = *width;
        surface.height = *height;
        surface.fingerprint = *fingerprint;
        for (auto& value : surface.logical_bounds) {
            auto bound = reader.read_pod<float>();
            if (!bound) return std::unexpected(bound.error());
            value = *bound;
        }
        surfaces.push_back(surface);
    }
    return surfaces;
}

auto parse_package(BufferSpan payload) -> SP::Expected<SP::UI::Declarative::WidgetRenderPackage> {
    PayloadReader reader{payload, 0};

    auto magic = reader.read_pod<std::uint32_t>();
    if (!magic) {
        return std::unexpected(magic.error());
    }
    if (*magic != kRenderPackageMagic) {
        return std::unexpected(SP::Error{SP::Error::Code::UnserializableType,
                                         "Invalid widget render package magic"});
    }

    auto version = reader.read_pod<std::uint16_t>();
    if (!version) {
        return std::unexpected(version.error());
    }
    if (*version != kRenderPackageVersion) {
        return std::unexpected(SP::Error{SP::Error::Code::UnserializableType,
                                         "Unsupported widget render package version"});
    }

    auto reserved = reader.read_pod<std::uint16_t>();
    if (!reserved) {
        return std::unexpected(reserved.error());
    }
    (void)reserved;

    SP::UI::Declarative::WidgetRenderPackage package{};

    auto capsule_revision = reader.read_pod<std::uint64_t>();
    if (!capsule_revision) return std::unexpected(capsule_revision.error());
    package.capsule_revision = *capsule_revision;

    auto render_sequence = reader.read_pod<std::uint64_t>();
    if (!render_sequence) return std::unexpected(render_sequence.error());
    package.render_sequence = *render_sequence;

    auto content_hash = reader.read_pod<std::uint64_t>();
    if (!content_hash) return std::unexpected(content_hash.error());
    package.content_hash = *content_hash;

    auto min_x = reader.read_pod<float>();
    if (!min_x) return std::unexpected(min_x.error());
    auto min_y = reader.read_pod<float>();
    if (!min_y) return std::unexpected(min_y.error());
    auto max_x = reader.read_pod<float>();
    if (!max_x) return std::unexpected(max_x.error());
    auto max_y = reader.read_pod<float>();
    if (!max_y) return std::unexpected(max_y.error());
    package.dirty_rect = SP::UI::Runtime::DirtyRectHint{*min_x, *min_y, *max_x, *max_y};

    if (auto status = reader.read_vector(package.command_kinds); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = reader.read_bytes(package.command_payload); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = reader.read_vector(package.texture_fingerprints); !status) {
        return std::unexpected(status.error());
    }

    auto surfaces = parse_surfaces(reader);
    if (!surfaces) {
        return std::unexpected(surfaces.error());
    }
    package.surfaces = std::move(*surfaces);

    return package;
}

} // namespace

namespace SP {

template <>
auto serialize(UI::Declarative::WidgetRenderPackage const& package, SlidingBuffer& buffer)
    -> std::optional<Error> {
    std::vector<uint8_t> payload;
    payload.reserve(256
                    + package.command_payload.size()
                    + package.command_kinds.size() * sizeof(std::uint32_t)
                    + package.texture_fingerprints.size() * sizeof(std::uint64_t)
                    + package.surfaces.size() * (sizeof(std::uint64_t) + sizeof(std::uint32_t) * 4 + sizeof(float) * 4));

    write_pod(payload, kRenderPackageMagic);
    write_pod(payload, kRenderPackageVersion);
    write_pod(payload, static_cast<std::uint16_t>(0));

    write_pod(payload, package.capsule_revision);
    write_pod(payload, package.render_sequence);
    write_pod(payload, package.content_hash);
    write_dirty_rect(payload, package.dirty_rect);

    if (auto status = write_vector(payload, package.command_kinds); status) {
        return status;
    }
    if (auto status = write_bytes(payload, package.command_payload); status) {
        return status;
    }
    if (auto status = write_vector(payload, package.texture_fingerprints); status) {
        return status;
    }
    if (auto status = write_surfaces(payload, package.surfaces); status) {
        return status;
    }

    return append_payload(buffer, payload);
}

template <>
auto deserialize<UI::Declarative::WidgetRenderPackage>(SlidingBuffer const& buffer)
    -> Expected<UI::Declarative::WidgetRenderPackage> {
    auto payload = extract_payload(buffer);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    return parse_package(payload->first);
}

template <>
auto deserialize_pop<UI::Declarative::WidgetRenderPackage>(SlidingBuffer& buffer)
    -> Expected<UI::Declarative::WidgetRenderPackage> {
    auto payload = extract_payload(buffer);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    auto parsed = parse_package(payload->first);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    buffer.advance(payload->second);
    return parsed;
}

} // namespace SP
