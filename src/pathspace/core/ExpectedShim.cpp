#include <expected>

#if defined(PATHSPACE_NEEDS_EXPECTED_SHIM)
namespace std {
__attribute__((weak)) const char* bad_expected_access<void>::what() const noexcept {
    return "bad access to std::expected";
}
}  // namespace std
#endif
