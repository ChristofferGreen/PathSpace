#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/web/ServeHtmlOptions.hpp>
#include <pathspace/web/serve_html/auth/Credentials.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace SP::ServeHtml {

class ServeHtmlSpace : public SP::PathSpace {
public:
    using SP::PathSpace::PathSpace;

    auto shared_context() const -> std::shared_ptr<SP::PathSpaceContext> { return this->getContext(); }
};

int RunServeHtmlServer(ServeHtmlSpace& space, ServeHtmlOptions const& options);

void RequestServeHtmlStop();
void ResetServeHtmlStopFlag();

} // namespace SP::ServeHtml
