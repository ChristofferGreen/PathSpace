#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/core/Error.hpp>
#include <pathspace/web/ServeHtmlOptions.hpp>
#include <pathspace/web/serve_html/auth/Credentials.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace SP::ServeHtml {

class ServeHtmlSpace : public SP::PathSpace {
public:
    using SP::PathSpace::PathSpace;

    auto shared_context() const -> std::shared_ptr<SP::PathSpaceContext> { return this->getContext(); }
};

struct ServeHtmlLogHooks {
    std::function<void(std::string_view)> info;
    std::function<void(std::string_view)> error;
};

int RunServeHtmlServer(ServeHtmlSpace& space, ServeHtmlOptions const& options);

int RunServeHtmlServerWithStopFlag(ServeHtmlSpace&                     space,
                                   ServeHtmlOptions const&            options,
                                   std::atomic<bool>&                 should_stop,
                                   ServeHtmlLogHooks const&           log_hooks = {},
                                   std::function<void(SP::Expected<void>)> on_listen = {});

void RequestServeHtmlStop();
void ResetServeHtmlStopFlag();

} // namespace SP::ServeHtml
