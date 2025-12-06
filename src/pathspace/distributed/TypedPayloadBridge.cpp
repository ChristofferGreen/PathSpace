#include "distributed/TypedPayloadBridge.hpp"

#include "core/Error.hpp"
#include "pathspace/PathSpace.hpp"
#include "type/SlidingBuffer.hpp"
#include "type/TypeMetadataRegistry.hpp"

#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace SP::Distributed {
namespace {

auto make_error(Error::Code code, std::string message) -> Error {
    return Error{code, std::move(message)};
}

class TypeErasedValue {
public:
    explicit TypeErasedValue(TypeOperations const& ops)
        : ops_(ops) {}

    ~TypeErasedValue() { reset(); }

    auto construct() -> Expected<void*> {
        auto const alignment = ops_.alignment == 0 ? alignof(std::max_align_t) : ops_.alignment;
        if (ops_.size == 0 || ops_.construct == nullptr) {
            return std::unexpected(make_error(Error::Code::InvalidType, "type is not constructible"));
        }
        storage_.resize(ops_.size + alignment);
        void*       raw   = storage_.data();
        std::size_t space = storage_.size();
        void*       aligned = std::align(alignment, ops_.size, raw, space);
        if (aligned == nullptr) {
            return std::unexpected(make_error(Error::Code::InvalidType, "unable to align storage"));
        }
        ptr_ = aligned;
        ops_.construct(ptr_);
        constructed_ = true;
        return ptr_;
    }

    auto data() const -> void* { return ptr_; }

private:
    void reset() {
        if (constructed_ && ops_.destroy) {
            ops_.destroy(ptr_);
        }
        constructed_ = false;
        ptr_         = nullptr;
        storage_.clear();
    }

    TypeOperations const&      ops_;
    std::vector<std::byte>     storage_;
    void*                      ptr_         = nullptr;
    bool                       constructed_ = false;
};

auto require_type(std::string_view type_name) -> Expected<TypeMetadataView> {
    if (type_name.empty()) {
        return std::unexpected(make_error(Error::Code::InvalidType, "type name is required"));
    }
    auto view = TypeMetadataRegistry::instance().findByName(type_name);
    if (!view.has_value()) {
        return std::unexpected(make_error(Error::Code::InvalidType, "unregistered type"));
    }
    return *view;
}

auto append_bytes(SlidingBuffer& buffer, std::span<const std::byte> bytes) -> void {
    if (bytes.empty()) {
        return;
    }
    auto span_u8 = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                                  bytes.size());
    buffer.append(span_u8);
}

auto buffer_to_vector(SlidingBuffer const& buffer) -> std::vector<std::byte> {
    auto size = buffer.size();
    std::vector<std::byte> output(size);
    if (size == 0) {
        return output;
    }
    auto raw    = buffer.rawData();
    auto offset = buffer.virtualFront();
    auto source = raw.data() + offset;
    std::memcpy(output.data(), source, size);
    return output;
}

auto wrap_exception(std::string_view context) -> Error {
    try {
        throw;
    } catch (std::exception const& ex) {
        return make_error(Error::Code::InvalidType,
                          std::string(context) + ": " + ex.what());
    } catch (...) {
        return make_error(Error::Code::InvalidType, std::string(context));
    }
}

} // namespace

auto insertTypedPayloadFromBytes(PathSpace&        space,
                                 std::string_view path,
                                 std::string_view type_name,
                                 std::span<const std::byte> bytes,
                                 In const& options) -> Expected<InsertReturn> {
    auto view = require_type(type_name);
    if (!view) {
        return std::unexpected(view.error());
    }
    if (view->metadata.deserialize == nullptr) {
        return std::unexpected(make_error(Error::Code::InvalidType, "type is not deserializable"));
    }
    if (view->operations.insert == nullptr) {
        return std::unexpected(make_error(Error::Code::InvalidType, "type cannot be inserted"));
    }

    TypeErasedValue value(view->operations);
    auto            storage = value.construct();
    if (!storage) {
        return std::unexpected(storage.error());
    }
    SlidingBuffer buffer;
    append_bytes(buffer, bytes);

    try {
        view->metadata.deserialize(*storage, buffer);
    } catch (...) {
        return std::unexpected(wrap_exception("typed payload decode failed"));
    }

    return view->operations.insert(space, path, *storage, options);
}

auto takeTypedPayloadToBytes(PathSpace&        space,
                             std::string_view path,
                             std::string_view type_name,
                             Out const& options) -> Expected<std::vector<std::byte>> {
    auto view = require_type(type_name);
    if (!view) {
        return std::unexpected(view.error());
    }
    if (view->metadata.serialize == nullptr) {
        return std::unexpected(make_error(Error::Code::InvalidType, "type is not serializable"));
    }
    if (view->operations.take == nullptr) {
        return std::unexpected(make_error(Error::Code::InvalidType, "type cannot be taken"));
    }

    TypeErasedValue value(view->operations);
    auto            storage = value.construct();
    if (!storage) {
        return std::unexpected(storage.error());
    }

    if (auto taken = view->operations.take(space, path, options, *storage); !taken) {
        return std::unexpected(taken.error());
    }

    SlidingBuffer buffer;
    try {
        view->metadata.serialize(*storage, buffer);
    } catch (...) {
        return std::unexpected(wrap_exception("typed payload encode failed"));
    }
    return buffer_to_vector(buffer);
}

} // namespace SP::Distributed
