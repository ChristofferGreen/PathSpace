#include <pathspace/web/serve_html/auth/Credentials.hpp>

#include <pathspace/web/ServeHtmlOptions.hpp>
#include <pathspace/web/ServeHtmlServer.hpp>
#include <pathspace/web/serve_html/Routes.hpp>

#include "core/Error.hpp"

#include <bcrypt/bcrypt.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <optional>
#include <string>

namespace SP::ServeHtml {

auto generate_bcrypt_hash(std::string_view password, int work_factor)
    -> std::optional<std::string> {
    std::array<char, BCRYPT_HASHSIZE> salt{};
    std::array<char, BCRYPT_HASHSIZE> hash{};

    auto clamped_factor = std::clamp(work_factor, 4, 31);
    if (bcrypt_gensalt(clamped_factor, salt.data()) != 0) {
        return std::nullopt;
    }

    std::string password_storage{password};
    if (bcrypt_hashpw(password_storage.c_str(), salt.data(), hash.data()) != 0) {
        return std::nullopt;
    }

    return std::string{hash.data()};
}

bool EnsureUserPassword(ServeHtmlSpace&           space,
                        ServeHtmlOptions const&  options,
                        std::string const&       username,
                        std::string const&       password,
                        int                      work_factor) {
    auto hash = generate_bcrypt_hash(password, work_factor);
    if (!hash) {
        std::cerr << "[serve_html] Failed to generate bcrypt hash for user '" << username << "'\n";
        return false;
    }
    auto password_path = make_user_password_path(options, username);
    auto result        = space.insert(password_path, *hash);
    if (!result.errors.empty()) {
        std::cerr << "[serve_html] Failed to write credentials for user '"
                  << username << "': " << SP::describeError(result.errors.front()) << "\n";
        return false;
    }
    return true;
}

void SeedDemoCredentials(SP::PathSpace& space, ServeHtmlOptions const& options) {
    auto hash = generate_bcrypt_hash("demo", 10);
    if (!hash) {
        std::cerr << "[serve_html] Failed to generate bcrypt hash for demo user\n";
        return;
    }

    auto password_path = make_user_password_path(options, "demo");
    auto result        = space.insert(password_path, *hash);
    if (!result.errors.empty()) {
        std::cerr << "[serve_html] Failed to seed demo credentials: "
                  << SP::describeError(result.errors.front()) << "\n";
    } else {
        std::cout << "[serve_html] Seeded demo credentials (user 'demo', password 'demo')\n";
    }
}

} // namespace SP::ServeHtml
