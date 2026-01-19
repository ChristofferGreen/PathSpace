#include "path/validation.hpp"
#include "third_party/doctest.h"

using namespace SP;

TEST_SUITE("path.validation") {
TEST_CASE("validate_path_impl reports detailed errors") {
    CHECK(validate_path_impl("/").code == ValidationError::Code::EmptyPath);
    CHECK(validate_path_impl("no-slash").code == ValidationError::Code::MustStartWithSlash);
    CHECK(validate_path_impl("/trailing/").code == ValidationError::Code::EndsWithSlash);
    CHECK(validate_path_impl("//double").code == ValidationError::Code::EmptyPathComponent);
    CHECK(validate_path_impl("/[foo/bar]").code == ValidationError::Code::SlashInBrackets);
    CHECK(validate_path_impl("/../bad").code == ValidationError::Code::RelativePath);
    CHECK(validate_path_impl("/[[]").code == ValidationError::Code::NestedBrackets);
    CHECK(validate_path_impl("/[unclosed").code == ValidationError::Code::UnclosedBracket);
    CHECK(validate_path_impl("/[!").code == ValidationError::Code::EmptyNegatedBracket);
    CHECK(validate_path_impl("/[-").code == ValidationError::Code::InvalidRangeSpec);
    CHECK(validate_path_impl("/[!]").code == ValidationError::Code::EmptyBracket);
    CHECK(validate_path_impl("/path]").code == ValidationError::Code::UnmatchedClosingBracket);
    CHECK(validate_path_impl("/[]").code == ValidationError::Code::EmptyBracket);
    CHECK(validate_path_impl("/[z-a]").code == ValidationError::Code::InvalidCharRange);
    CHECK(validate_path_impl("/[a-]").code == ValidationError::Code::InvalidCharRange);

    // Valid path
    CHECK(validate_path_impl("/ok/path").code == ValidationError::Code::None);
}

TEST_CASE("get_error_message returns helpful strings") {
    CHECK(get_error_message(ValidationError::Code::None) == nullptr);
    CHECK(get_error_message(ValidationError::Code::EmptyPath) != nullptr);
    CHECK(get_error_message(ValidationError::Code::MustStartWithSlash) != nullptr);
    CHECK(get_error_message(ValidationError::Code::EndsWithSlash) != nullptr);
    CHECK(get_error_message(ValidationError::Code::EmptyPathComponent) != nullptr);
    CHECK(get_error_message(ValidationError::Code::SlashInBrackets) != nullptr);
    CHECK(get_error_message(ValidationError::Code::RelativePath) != nullptr);
    CHECK(get_error_message(ValidationError::Code::NestedBrackets) != nullptr);
    CHECK(get_error_message(ValidationError::Code::UnclosedBracket) != nullptr);
    CHECK(get_error_message(ValidationError::Code::EmptyNegatedBracket) != nullptr);
    CHECK(get_error_message(ValidationError::Code::UnmatchedClosingBracket) != nullptr);
    CHECK(get_error_message(ValidationError::Code::EmptyBracket) != nullptr);
    CHECK(get_error_message(ValidationError::Code::InvalidRangeSpec) != nullptr);
    CHECK(get_error_message(ValidationError::Code::InvalidCharRange) != nullptr);
    CHECK(get_error_message(ValidationError::Code::NoContent) != nullptr);
}
}
