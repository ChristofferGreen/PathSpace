#include <expected>

#if defined(PATHSPACE_NEEDS_EXPECTED_SHIM)
namespace std {
__attribute__((weak)) const char* bad_expected_access<void>::what() const noexcept {
    return "bad access to std::expected";
}
}  // namespace std

// Test-only helper to exercise the shimmed implementation reliably.
namespace SP::testing {
// Derive to make the protected constructor/dtor public on newer libc++.
struct PublicBadExpected : std::bad_expected_access<void> {
    using std::bad_expected_access<void>::bad_expected_access;
    using std::bad_expected_access<void>::what;
    PublicBadExpected() noexcept : std::bad_expected_access<void>() {}
};

const char* callExpectedShimWhat() {
    PublicBadExpected ex;
    return ex.what();
}
}  // namespace SP::testing
#endif
