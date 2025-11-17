#include <pathspace/io/IoEvents.hpp>
#include <pathspace/type/serialization.hpp>

#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace SP::IO {
namespace {

using BufferSpan = std::span<uint8_t const>;

inline auto write_bytes(std::vector<uint8_t>& out, void const* data, size_t size) {
    auto ptr = static_cast<uint8_t const*>(data);
    out.insert(out.end(), ptr, ptr + size);
}

template <typename T>
inline auto write_pod(std::vector<uint8_t>& out, T value) {
    write_bytes(out, &value, sizeof(T));
}

inline auto write_string(std::vector<uint8_t>& out, std::string const& value) {
    auto length = static_cast<std::uint32_t>(value.size());
    write_pod(out, length);
    if (!value.empty()) {
        write_bytes(out, value.data(), value.size());
    }
}

template <typename T, typename WriteFn>
inline auto write_optional(std::vector<uint8_t>& out,
                           std::optional<T> const& value,
                           WriteFn&& writer) {
    std::uint8_t has_value = value.has_value() ? 1u : 0u;
    write_pod(out, has_value);
    if (value) {
        writer(out, *value);
    }
}

inline auto write_pose(std::vector<uint8_t>& out, Pose const& pose) {
    write_bytes(out, pose.position, sizeof(pose.position));
    write_bytes(out, pose.orientation, sizeof(pose.orientation));
}

inline auto write_stylus(std::vector<uint8_t>& out, StylusInfo const& stylus) {
    write_pod(out, stylus.pressure);
    write_pod(out, stylus.tilt_x);
    write_pod(out, stylus.tilt_y);
    write_pod(out, stylus.twist);
    std::uint8_t eraser = stylus.eraser ? 1u : 0u;
    write_pod(out, eraser);
}

inline auto write_motion(std::vector<uint8_t>& out, PointerMotion const& motion) {
    write_pod(out, motion.delta_x);
    write_pod(out, motion.delta_y);
    write_pod(out, motion.absolute_x);
    write_pod(out, motion.absolute_y);
    std::uint8_t absolute = motion.absolute ? 1u : 0u;
    write_pod(out, absolute);
}

inline auto write_pointer_meta(std::vector<uint8_t>& out, PointerMeta const& meta) {
    auto mask = static_cast<std::uint32_t>(meta.modifiers);
    write_pod(out, mask);
    auto timestamp = static_cast<std::int64_t>(meta.timestamp.count());
    write_pod(out, timestamp);
}

inline auto write_button_state(std::vector<uint8_t>& out, ButtonState const& state) {
    std::uint8_t pressed = state.pressed ? 1u : 0u;
    std::uint8_t repeat = state.repeat ? 1u : 0u;
    write_pod(out, pressed);
    write_pod(out, repeat);
    write_pod(out, state.analog_value);
}

inline auto write_button_meta(std::vector<uint8_t>& out, ButtonMeta const& meta) {
    auto mask = static_cast<std::uint32_t>(meta.modifiers);
    write_pod(out, mask);
    auto timestamp = static_cast<std::int64_t>(meta.timestamp.count());
    write_pod(out, timestamp);
}

struct PayloadReader {
    BufferSpan data;
    size_t offset = 0;

    template <typename T>
    auto read_pod() -> SP::Expected<T> {
        if (offset + sizeof(T) > data.size()) {
            return std::unexpected(Error{Error::Code::MalformedInput, "Truncated payload"});
        }
        T value{};
        std::memcpy(&value, data.data() + offset, sizeof(T));
        offset += sizeof(T);
        return value;
    }

    auto read_string() -> SP::Expected<std::string> {
        auto length = read_pod<std::uint32_t>();
        if (!length) {
            return std::unexpected(length.error());
        }
        if (offset + *length > data.size()) {
            return std::unexpected(Error{Error::Code::MalformedInput, "String exceeds payload"});
        }
        std::string value(reinterpret_cast<char const*>(data.data() + offset), *length);
        offset += *length;
        return value;
    }

    template <typename T, typename ReadFn>
    auto read_optional(ReadFn&& fn) -> SP::Expected<std::optional<T>> {
        auto present = read_pod<std::uint8_t>();
        if (!present) {
            return std::unexpected(present.error());
        }
        if (*present == 0) {
            return std::optional<T>{};
        }
        if (auto parsed = fn(); !parsed) {
            return std::unexpected(parsed.error());
        } else {
            return std::optional<T>{*parsed};
        }
    }

    auto read_pose() -> SP::Expected<Pose> {
        Pose pose{};
        if (offset + sizeof(pose.position) + sizeof(pose.orientation) > data.size()) {
            return std::unexpected(Error{Error::Code::MalformedInput, "Pose exceeds payload"});
        }
        std::memcpy(pose.position, data.data() + offset, sizeof(pose.position));
        offset += sizeof(pose.position);
        std::memcpy(pose.orientation, data.data() + offset, sizeof(pose.orientation));
        offset += sizeof(pose.orientation);
        return pose;
    }

    auto read_stylus() -> SP::Expected<StylusInfo> {
        StylusInfo info{};
        auto pressure = read_pod<float>();
        if (!pressure) return std::unexpected(pressure.error());
        info.pressure = *pressure;
        auto tilt_x = read_pod<float>();
        if (!tilt_x) return std::unexpected(tilt_x.error());
        info.tilt_x = *tilt_x;
        auto tilt_y = read_pod<float>();
        if (!tilt_y) return std::unexpected(tilt_y.error());
        info.tilt_y = *tilt_y;
        auto twist = read_pod<float>();
        if (!twist) return std::unexpected(twist.error());
        info.twist = *twist;
        auto eraser = read_pod<std::uint8_t>();
        if (!eraser) return std::unexpected(eraser.error());
        info.eraser = *eraser != 0;
        return info;
    }

    auto read_motion() -> SP::Expected<PointerMotion> {
        PointerMotion motion{};
        auto dx = read_pod<float>();
        if (!dx) return std::unexpected(dx.error());
        motion.delta_x = *dx;
        auto dy = read_pod<float>();
        if (!dy) return std::unexpected(dy.error());
        motion.delta_y = *dy;
        auto ax = read_pod<float>();
        if (!ax) return std::unexpected(ax.error());
        motion.absolute_x = *ax;
        auto ay = read_pod<float>();
        if (!ay) return std::unexpected(ay.error());
        motion.absolute_y = *ay;
        auto absolute = read_pod<std::uint8_t>();
        if (!absolute) return std::unexpected(absolute.error());
        motion.absolute = *absolute != 0;
        return motion;
    }

    auto read_pointer_meta() -> SP::Expected<PointerMeta> {
        PointerMeta meta{};
        auto mask = read_pod<std::uint32_t>();
        if (!mask) return std::unexpected(mask.error());
        meta.modifiers = static_cast<ButtonModifiers>(*mask);
        auto ns = read_pod<std::int64_t>();
        if (!ns) return std::unexpected(ns.error());
        meta.timestamp = std::chrono::nanoseconds{*ns};
        return meta;
    }

    auto read_button_state() -> SP::Expected<ButtonState> {
        ButtonState state{};
        auto pressed = read_pod<std::uint8_t>();
        if (!pressed) return std::unexpected(pressed.error());
        state.pressed = *pressed != 0;
        auto repeat = read_pod<std::uint8_t>();
        if (!repeat) return std::unexpected(repeat.error());
        state.repeat = *repeat != 0;
        auto analog = read_pod<float>();
        if (!analog) return std::unexpected(analog.error());
        state.analog_value = *analog;
        return state;
    }

    auto read_button_meta() -> SP::Expected<ButtonMeta> {
        ButtonMeta meta{};
        auto mask = read_pod<std::uint32_t>();
        if (!mask) return std::unexpected(mask.error());
        meta.modifiers = static_cast<ButtonModifiers>(*mask);
        auto ns = read_pod<std::int64_t>();
        if (!ns) return std::unexpected(ns.error());
        meta.timestamp = std::chrono::nanoseconds{*ns};
        return meta;
    }
};

inline auto append_payload(SlidingBuffer& buffer, std::vector<uint8_t> const& payload) -> std::optional<Error> {
    if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        return Error{Error::Code::CapacityExceeded, "Payload too large"};
    }
    Header header{static_cast<std::uint32_t>(payload.size())};
    buffer.append(reinterpret_cast<uint8_t const*>(&header), sizeof(header));
    if (!payload.empty()) {
        buffer.append(payload.data(), payload.size());
    }
    return std::nullopt;
}

inline auto extract_payload(SlidingBuffer const& buffer) -> SP::Expected<std::pair<BufferSpan, size_t>> {
    if (buffer.size() < sizeof(Header)) {
        return std::unexpected(Error{Error::Code::MalformedInput, "Missing payload header"});
    }
    Header header{};
    std::memcpy(&header, buffer.data(), sizeof(header));
    size_t total = sizeof(header) + header.size;
    if (buffer.size() < total) {
        return std::unexpected(Error{Error::Code::MalformedInput, "Payload truncated"});
    }
    BufferSpan span(buffer.data() + sizeof(header), header.size);
    return std::pair<BufferSpan, size_t>{span, total};
}

inline auto parse_pointer_event(BufferSpan payload) -> SP::Expected<IO::PointerEvent> {
    PayloadReader reader{payload, 0};
    IO::PointerEvent event{};
    auto device_path = reader.read_string();
    if (!device_path) return std::unexpected(device_path.error());
    event.device_path = std::move(*device_path);
    auto pointer_id = reader.read_pod<std::uint64_t>();
    if (!pointer_id) return std::unexpected(pointer_id.error());
    event.pointer_id = *pointer_id;
    auto motion = reader.read_motion();
    if (!motion) return std::unexpected(motion.error());
    event.motion = *motion;
    auto type = reader.read_pod<std::uint8_t>();
    if (!type) return std::unexpected(type.error());
    event.type = static_cast<PointerType>(*type);
    auto pose = reader.read_optional<Pose>([&]() { return reader.read_pose(); });
    if (!pose) return std::unexpected(pose.error());
    event.pose = std::move(*pose);
    auto stylus = reader.read_optional<StylusInfo>([&]() { return reader.read_stylus(); });
    if (!stylus) return std::unexpected(stylus.error());
    event.stylus = std::move(*stylus);
    auto meta = reader.read_pointer_meta();
    if (!meta) return std::unexpected(meta.error());
    event.meta = *meta;
    return event;
}

inline auto parse_button_event(BufferSpan payload) -> SP::Expected<IO::ButtonEvent> {
    PayloadReader reader{payload, 0};
    IO::ButtonEvent event{};
    auto source = reader.read_pod<std::uint8_t>();
    if (!source) return std::unexpected(source.error());
    event.source = static_cast<ButtonSource>(*source);
    auto device_path = reader.read_string();
    if (!device_path) return std::unexpected(device_path.error());
    event.device_path = std::move(*device_path);
    auto code = reader.read_pod<std::uint32_t>();
    if (!code) return std::unexpected(code.error());
    event.button_code = *code;
    auto id = reader.read_pod<int>();
    if (!id) return std::unexpected(id.error());
    event.button_id = *id;
    auto state = reader.read_button_state();
    if (!state) return std::unexpected(state.error());
    event.state = *state;
    auto meta = reader.read_button_meta();
    if (!meta) return std::unexpected(meta.error());
    event.meta = *meta;
    return event;
}

inline auto parse_text_event(BufferSpan payload) -> SP::Expected<IO::TextEvent> {
    PayloadReader reader{payload, 0};
    IO::TextEvent event{};
    auto device_path = reader.read_string();
    if (!device_path) return std::unexpected(device_path.error());
    event.device_path = std::move(*device_path);
    auto codepoint = reader.read_pod<std::uint32_t>();
    if (!codepoint) return std::unexpected(codepoint.error());
    event.codepoint = static_cast<char32_t>(*codepoint);
    auto modifiers = reader.read_pod<std::uint32_t>();
    if (!modifiers) return std::unexpected(modifiers.error());
    event.modifiers = static_cast<ButtonModifiers>(*modifiers);
    auto repeat = reader.read_pod<std::uint8_t>();
    if (!repeat) return std::unexpected(repeat.error());
    event.repeat = *repeat != 0;
    auto timestamp = reader.read_pod<std::int64_t>();
    if (!timestamp) return std::unexpected(timestamp.error());
    event.timestamp = std::chrono::nanoseconds{*timestamp};
    return event;
}

} // namespace

} // namespace SP::IO

namespace SP {

template <>
auto serialize(IO::PointerEvent const& event, SlidingBuffer& buffer) -> std::optional<Error> {
    std::vector<uint8_t> payload;
    IO::write_string(payload, event.device_path);
    IO::write_pod(payload, event.pointer_id);
    IO::write_motion(payload, event.motion);
    auto type = static_cast<std::uint8_t>(event.type);
    IO::write_pod(payload, type);
    IO::write_optional(payload, event.pose, IO::write_pose);
    IO::write_optional(payload, event.stylus, IO::write_stylus);
    IO::write_pointer_meta(payload, event.meta);
    return IO::append_payload(buffer, payload);
}

template <>
auto serialize(IO::ButtonEvent const& event, SlidingBuffer& buffer) -> std::optional<Error> {
    std::vector<uint8_t> payload;
    IO::write_pod(payload, static_cast<std::uint8_t>(event.source));
    IO::write_string(payload, event.device_path);
    IO::write_pod(payload, event.button_code);
    IO::write_pod(payload, event.button_id);
    IO::write_button_state(payload, event.state);
    IO::write_button_meta(payload, event.meta);
    return IO::append_payload(buffer, payload);
}

template <>
auto serialize(IO::TextEvent const& event, SlidingBuffer& buffer) -> std::optional<Error> {
    std::vector<uint8_t> payload;
    IO::write_string(payload, event.device_path);
    IO::write_pod(payload, static_cast<std::uint32_t>(event.codepoint));
    IO::write_pod(payload, static_cast<std::uint32_t>(event.modifiers));
    std::uint8_t repeat = event.repeat ? 1u : 0u;
    IO::write_pod(payload, repeat);
    auto timestamp = static_cast<std::int64_t>(event.timestamp.count());
    IO::write_pod(payload, timestamp);
    return IO::append_payload(buffer, payload);
}

template <>
auto deserialize<IO::PointerEvent>(SlidingBuffer const& buffer) -> Expected<IO::PointerEvent> {
    auto payload = IO::extract_payload(buffer);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    auto parsed = IO::parse_pointer_event(payload->first);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return *parsed;
}

template <>
auto deserialize_pop<IO::PointerEvent>(SlidingBuffer& buffer) -> Expected<IO::PointerEvent> {
    auto payload = IO::extract_payload(buffer);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    auto parsed = IO::parse_pointer_event(payload->first);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    buffer.advance(payload->second);
    return *parsed;
}

template <>
auto deserialize<IO::ButtonEvent>(SlidingBuffer const& buffer) -> Expected<IO::ButtonEvent> {
    auto payload = IO::extract_payload(buffer);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    auto parsed = IO::parse_button_event(payload->first);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return *parsed;
}

template <>
auto deserialize_pop<IO::ButtonEvent>(SlidingBuffer& buffer) -> Expected<IO::ButtonEvent> {
    auto payload = IO::extract_payload(buffer);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    auto parsed = IO::parse_button_event(payload->first);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    buffer.advance(payload->second);
    return *parsed;
}

template <>
auto deserialize<IO::TextEvent>(SlidingBuffer const& buffer) -> Expected<IO::TextEvent> {
    auto payload = IO::extract_payload(buffer);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    auto parsed = IO::parse_text_event(payload->first);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return *parsed;
}

template <>
auto deserialize_pop<IO::TextEvent>(SlidingBuffer& buffer) -> Expected<IO::TextEvent> {
    auto payload = IO::extract_payload(buffer);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    auto parsed = IO::parse_text_event(payload->first);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    buffer.advance(payload->second);
    return *parsed;
}

} // namespace SP
