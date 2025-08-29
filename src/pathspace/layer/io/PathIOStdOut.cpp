#include "layer/io/PathIOStdOut.hpp"

#include <iostream>
#include <typeinfo>
#include <string>

namespace SP {

auto PathIOStdOut::in(Iterator const& /*path*/, InputData const& data) -> InsertReturn {
    InsertReturn ret;

    // Only accept string payloads (InputMetadataT maps string-like to std::string)
    if (data.metadata.typeInfo != &typeid(std::string)) {
        ret.errors.emplace_back(Error::Code::InvalidType, "PathIOStdOut only accepts std::string");
        return ret;
    }

    auto const* str = reinterpret_cast<std::string const*>(data.obj);
    if (str == nullptr) {
        ret.errors.emplace_back(Error::Code::MalformedInput, "Null string pointer for PathIOStdOut");
        return ret;
    }

    {
        std::lock_guard<std::mutex> lg(mutex_);
        if (!prefix_.empty()) {
            std::cout << prefix_;
        }
        std::cout << *str;
        if (addNewline_) {
            std::cout << '\n';
        }
        std::cout.flush();
    }

    // Report as one processed value (nothing is stored)
    ret.nbrValuesInserted = 1;
    return ret;
}

auto PathIOStdOut::out(Iterator const& /*path*/,
                       InputMetadata const& /*inputMetadata*/,
                       Out const& /*options*/,
                       void* /*obj*/) -> std::optional<Error> {
    return Error{Error::Code::InvalidPermissions, "PathIOStdOut does not support out()"};
}

auto PathIOStdOut::shutdown() -> void {
    // no-op
}

auto PathIOStdOut::notify(std::string const& /*notificationPath*/) -> void {
    // no-op
}

} // namespace SP