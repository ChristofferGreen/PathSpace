#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace SP {
class PathSpace;
} // namespace SP

namespace SP::ServeHtml {

class ServeHtmlSpace;
struct ServeHtmlOptions;

auto generate_bcrypt_hash(std::string_view password, int work_factor)
    -> std::optional<std::string>;

bool EnsureUserPassword(ServeHtmlSpace&           space,
                        ServeHtmlOptions const&  options,
                        std::string const&       username,
                        std::string const&       password,
                        int                      work_factor = 10);

void SeedDemoCredentials(SP::PathSpace& space, ServeHtmlOptions const& options);

} // namespace SP::ServeHtml
