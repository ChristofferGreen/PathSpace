#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <pathspace/core/Error.hpp>
#include <pathspace/type/SlidingBuffer.hpp>
#include <pathspace/type/serialization.hpp>
#include <pathspace/ui/HtmlAsset.hpp>

namespace SP {

namespace detail {
constexpr std::uint32_t kHtmlAssetMagic = 0x48534154u; // 'HSAT'
constexpr std::uint16_t kHtmlAssetVersion = 1u;

constexpr auto byteswap16(std::uint16_t value) -> std::uint16_t {
    return static_cast<std::uint16_t>((value >> 8u) | (value << 8u));
}

constexpr auto byteswap32(std::uint32_t value) -> std::uint32_t {
    return ((value & 0x000000FFu) << 24u)
         | ((value & 0x0000FF00u) << 8u)
         | ((value & 0x00FF0000u) >> 8u)
         | ((value & 0xFF000000u) >> 24u);
}

constexpr auto to_le16(std::uint16_t value) -> std::uint16_t {
    if constexpr (std::endian::native == std::endian::little) {
        return value;
    } else {
        return byteswap16(value);
    }
}

constexpr auto to_le32(std::uint32_t value) -> std::uint32_t {
    if constexpr (std::endian::native == std::endian::little) {
        return value;
    } else {
        return byteswap32(value);
    }
}

constexpr auto from_le16(std::uint16_t value) -> std::uint16_t {
    return to_le16(value);
}

constexpr auto from_le32(std::uint32_t value) -> std::uint32_t {
    return to_le32(value);
}

struct HtmlAssetDecodeResult {
    std::vector<UI::Html::Asset> assets;
    size_t                       bytes_consumed = 0;
};

inline auto check_length(std::string_view label, std::size_t length)
    -> std::optional<Error> {
    if (length > std::numeric_limits<std::uint32_t>::max()) {
        return Error{Error::Code::SerializationFunctionMissing,
                     std::string(label) + " exceeds 4 GiB limit for HTML asset serialization"};
    }
    return std::nullopt;
}

inline auto parse_length(std::string_view what,
                         std::uint8_t const* data,
                         std::size_t size,
                         std::size_t& offset) -> Expected<std::uint32_t> {
    if (offset + sizeof(std::uint32_t) > size) {
        return std::unexpected(Error{Error::Code::MalformedInput,
                                     std::string("Buffer too small reading ") + std::string(what)});
    }
    std::uint32_t raw = 0;
    std::memcpy(&raw, data + offset, sizeof(raw));
    offset += sizeof(raw);
    return from_le32(raw);
}

} // namespace detail

template <>
inline auto serialize<std::vector<UI::Html::Asset>>(std::vector<UI::Html::Asset> const& assets,
                                                    SlidingBuffer& buffer) -> std::optional<Error> {
    if (assets.size() > std::numeric_limits<std::uint32_t>::max()) {
        return Error{Error::Code::SerializationFunctionMissing,
                     "HTML asset count exceeds 32-bit limit"};
    }

    std::uint32_t const magic = detail::to_le32(detail::kHtmlAssetMagic);
    std::uint16_t const version = detail::to_le16(detail::kHtmlAssetVersion);
    std::uint32_t const count = detail::to_le32(static_cast<std::uint32_t>(assets.size()));

    buffer.append(reinterpret_cast<uint8_t const*>(&magic), sizeof(magic));
    buffer.append(reinterpret_cast<uint8_t const*>(&version), sizeof(version));
    buffer.append(reinterpret_cast<uint8_t const*>(&count), sizeof(count));

    for (auto const& asset : assets) {
        if (auto err = detail::check_length("logical_path", asset.logical_path.size())) {
            return err;
        }
        if (auto err = detail::check_length("mime_type", asset.mime_type.size())) {
            return err;
        }
        if (auto err = detail::check_length("asset bytes", asset.bytes.size())) {
            return err;
        }

        std::uint32_t const logical_len = detail::to_le32(static_cast<std::uint32_t>(asset.logical_path.size()));
        std::uint32_t const mime_len = detail::to_le32(static_cast<std::uint32_t>(asset.mime_type.size()));
        std::uint32_t const bytes_len = detail::to_le32(static_cast<std::uint32_t>(asset.bytes.size()));

        buffer.append(reinterpret_cast<uint8_t const*>(&logical_len), sizeof(logical_len));
        buffer.append(reinterpret_cast<uint8_t const*>(&mime_len), sizeof(mime_len));
        buffer.append(reinterpret_cast<uint8_t const*>(&bytes_len), sizeof(bytes_len));

        if (!asset.logical_path.empty()) {
            buffer.append(reinterpret_cast<uint8_t const*>(asset.logical_path.data()), asset.logical_path.size());
        }
        if (!asset.mime_type.empty()) {
            buffer.append(reinterpret_cast<uint8_t const*>(asset.mime_type.data()), asset.mime_type.size());
        }
        if (!asset.bytes.empty()) {
            buffer.append(asset.bytes.data(), asset.bytes.size());
        }
    }

    return std::nullopt;
}

inline auto decode_html_assets_payload(SlidingBuffer const& buffer)
    -> Expected<detail::HtmlAssetDecodeResult> {
    auto const total_size = buffer.size();
    if (total_size < sizeof(std::uint32_t) + sizeof(std::uint16_t) + sizeof(std::uint32_t)) {
        return std::unexpected(Error{Error::Code::MalformedInput,
                                     "Buffer too small for HTML asset header"});
    }

    auto const* raw = buffer.data();
    std::size_t offset = 0;

    std::uint32_t magic_raw = 0;
    std::memcpy(&magic_raw, raw + offset, sizeof(magic_raw));
    offset += sizeof(magic_raw);
    if (detail::from_le32(magic_raw) != detail::kHtmlAssetMagic) {
        return std::unexpected(Error{Error::Code::MalformedInput,
                                     "Invalid HTML asset magic"});
    }

    std::uint16_t version_raw = 0;
    std::memcpy(&version_raw, raw + offset, sizeof(version_raw));
    offset += sizeof(version_raw);
    if (detail::from_le16(version_raw) != detail::kHtmlAssetVersion) {
        return std::unexpected(Error{Error::Code::UnserializableType,
                                     "Unsupported HTML asset serialization version"});
    }

    std::uint32_t count_raw = 0;
    std::memcpy(&count_raw, raw + offset, sizeof(count_raw));
    offset += sizeof(count_raw);
    auto const count = detail::from_le32(count_raw);

    std::vector<UI::Html::Asset> assets;
    assets.reserve(count);

    for (std::uint32_t index = 0; index < count; ++index) {
        auto logical_len = detail::parse_length("logical path length", raw, total_size, offset);
        if (!logical_len) {
            return std::unexpected(logical_len.error());
        }
        auto mime_len = detail::parse_length("mime type length", raw, total_size, offset);
        if (!mime_len) {
            return std::unexpected(mime_len.error());
        }
        auto bytes_len = detail::parse_length("asset bytes length", raw, total_size, offset);
        if (!bytes_len) {
            return std::unexpected(bytes_len.error());
        }

        auto require_bytes = [&](std::uint32_t len, std::string_view label) -> Expected<void> {
            if (offset + len > total_size) {
                return std::unexpected(Error{Error::Code::MalformedInput,
                                             std::string("Buffer truncated while reading ") + std::string(label)});
            }
            return {};
        };

        if (auto status = require_bytes(*logical_len, "logical path"); !status) {
            return std::unexpected(status.error());
        }
        std::string logical;
        logical.resize(*logical_len);
        if (*logical_len > 0) {
            std::memcpy(logical.data(), raw + offset, *logical_len);
        }
        offset += *logical_len;

        if (auto status = require_bytes(*mime_len, "mime type"); !status) {
            return std::unexpected(status.error());
        }
        std::string mime;
        mime.resize(*mime_len);
        if (*mime_len > 0) {
            std::memcpy(mime.data(), raw + offset, *mime_len);
        }
        offset += *mime_len;

        if (auto status = require_bytes(*bytes_len, "asset bytes"); !status) {
            return std::unexpected(status.error());
        }
        std::vector<std::uint8_t> bytes;
        bytes.resize(*bytes_len);
        if (*bytes_len > 0) {
            std::memcpy(bytes.data(), raw + offset, *bytes_len);
        }
        offset += *bytes_len;

        UI::Html::Asset asset;
        asset.logical_path = std::move(logical);
        asset.mime_type = std::move(mime);
        asset.bytes = std::move(bytes);
        assets.emplace_back(std::move(asset));
    }

    return detail::HtmlAssetDecodeResult{
        .assets = std::move(assets),
        .bytes_consumed = offset,
    };
}

template <>
inline auto deserialize<std::vector<UI::Html::Asset>>(SlidingBuffer const& buffer)
    -> Expected<std::vector<UI::Html::Asset>> {
    auto decoded = decode_html_assets_payload(buffer);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    return std::move(decoded->assets);
}

template <>
inline auto deserialize_pop<std::vector<UI::Html::Asset>>(SlidingBuffer& buffer)
    -> Expected<std::vector<UI::Html::Asset>> {
    auto decoded = decode_html_assets_payload(buffer);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    buffer.advance(decoded->bytes_consumed);
    return std::move(decoded->assets);
}

} // namespace SP
