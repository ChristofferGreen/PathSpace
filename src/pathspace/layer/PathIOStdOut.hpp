#pragma once
#include "layer/PathIO.hpp"
#include "core/InsertReturn.hpp"
#include "type/InputData.hpp"
#include "path/Iterator.hpp"
#include "core/Error.hpp"

#include <iostream>
#include <mutex>
#include <string>
#include <typeinfo>

namespace SP {

/**
 * PathIOStdOut — a simple sink that prints inserted strings to stdout.
 *
 * Semantics:
 * - in(path, data):
 *   - Accepts std::string (and string-like types that map to std::string in InputMetadataT).
 *   - Prints the string to std::cout, optionally with a configurable prefix and trailing newline.
 *   - Returns success (nbrValuesInserted=1) without storing anything in a trie.
 * - out(...): unsupported (inherits PathIO base behavior and returns an error).
 * - notify/shutdown: no-ops (inherits PathIO base behavior).
 *
 * Notes:
 * - This class is mount-agnostic: it doesn't care where it lives in a parent PathSpace.
 * - Thread-safe printing: a local mutex guards access to std::cout to avoid interleaving.
 */
class PathIOStdOut final : public PathIO {
public:
    explicit PathIOStdOut(bool addNewline = true, std::string prefix = {})
        : addNewline_(addNewline), prefix_(std::move(prefix)) {}

    // Print inserted strings to stdout; reject non-string types.
    auto in(Iterator const& /*path*/, InputData const& data) -> InsertReturn override {
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

private:
    bool        addNewline_{true};
    std::string prefix_;
    std::mutex  mutex_;
};

} // namespace SP