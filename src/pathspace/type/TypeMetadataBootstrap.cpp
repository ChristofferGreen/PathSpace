#include "type/TypeMetadataRegistry.hpp"

#include <cstdint>
#include <string>

namespace {

template <typename T>
void RegisterType(SP::TypeMetadataRegistry& registry) {
    [[maybe_unused]] bool registered = registry.registerType<T>();
    (void)registered;
}

} // namespace

namespace SP {

// Fills the given registry. Safe to repeat: registerType rejects a type that is already present.
// Whether the process-wide registry has been filled is tracked by TypeMetadataRegistry::instance().
void RegisterBuiltinTypeMetadata(TypeMetadataRegistry& registry) {
    RegisterType<bool>(registry);
    RegisterType<char>(registry);
    RegisterType<signed char>(registry);
    RegisterType<unsigned char>(registry);
    RegisterType<short>(registry);
    RegisterType<unsigned short>(registry);
    RegisterType<int>(registry);
    RegisterType<unsigned int>(registry);
    RegisterType<long>(registry);
    RegisterType<unsigned long>(registry);
    RegisterType<long long>(registry);
    RegisterType<unsigned long long>(registry);
    RegisterType<float>(registry);
    RegisterType<double>(registry);
    RegisterType<long double>(registry);
    RegisterType<std::int8_t>(registry);
    RegisterType<std::uint8_t>(registry);
    RegisterType<std::int16_t>(registry);
    RegisterType<std::uint16_t>(registry);
    RegisterType<std::int32_t>(registry);
    RegisterType<std::uint32_t>(registry);
    RegisterType<std::int64_t>(registry);
    RegisterType<std::uint64_t>(registry);
    RegisterType<std::string>(registry);
}

} // namespace SP
