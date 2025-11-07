#include "ConcretePath.hpp"

#include "path/utils.hpp"

#include <string_view>
#include <utility>

namespace {

using SP::Error;
using SP::Expected;

struct ParsedConcretePath {
    std::string              canonical;
    std::vector<std::string> components;
};

auto make_path_error(Error::Code code, std::string message) -> Error {
    return Error{code, std::move(message)};
}

auto parse_concrete_path(std::string_view raw) -> Expected<ParsedConcretePath> {
    std::string working(raw);
    if (working.empty()) {
        working = "/";
    }

    if (working.front() != '/') {
        working.insert(working.begin(), '/');
    }

    while (working.size() > 1 && working.back() == '/') {
        working.pop_back();
    }

    std::vector<std::string> components;
    std::size_t              pos = 1;
    while (pos < working.size()) {
        auto next = working.find('/', pos);
        auto end  = (next == std::string::npos) ? working.size() : next;
        if (end == pos) {
            return std::unexpected(make_path_error(Error::Code::InvalidPathSubcomponent,
                                                   "Empty path component"));
        }

        std::string_view token{working.data() + pos, end - pos};
        if (token == "." || token == "..") {
            return std::unexpected(make_path_error(Error::Code::InvalidPathSubcomponent,
                                                   "Relative path components are not allowed"));
        }
        if (SP::is_glob(token)) {
            return std::unexpected(make_path_error(Error::Code::InvalidPathSubcomponent,
                                                   "Glob syntax is not allowed in concrete paths"));
        }

        components.emplace_back(token);
        if (next == std::string::npos) {
            break;
        }
        pos = next + 1;
    }

    return ParsedConcretePath{std::move(working), std::move(components)};
}

} // namespace

namespace SP {
template<typename T>
auto ConcretePath<T>::begin() const -> ConcretePathIterator<T> {
    return {this->path.begin(), this->path.end()};
}

template<typename T>
auto ConcretePath<T>::end() const -> ConcretePathIterator<T> {
    return {this->path.end(), this->path.end()};
}

template<typename T>
ConcretePath<T>::ConcretePath(T const &t) : Path<T>(t) {}

template<typename T>
ConcretePath<T>::ConcretePath(char const * const t) : Path<T>(t) {}

template<typename T>
auto ConcretePath<T>::operator==(std::string_view const &otherView) const -> bool {
    ConcretePathStringView const other{otherView};
    if(!this->isValid() || !other.isValid())
        return false;
    auto iterA = this->begin();
    auto iterB = other.begin();
    bool a = iterA != this->end();
    bool b = iterB != other.end();
    while(iterA != this->end() && iterB != other.end()) {
        if(*iterA != *iterB)
            return false;
        ++iterA;
        ++iterB;
    }
    if(iterA != this->end() || iterB != other.end())
        return false;
    return true;
}

template<typename T>
auto ConcretePath<T>::operator==(ConcretePath<T> const &other) const -> bool {
    return this->operator==(std::string_view{other.path});
}

template<typename T>
auto ConcretePath<T>::operator==(char const * const other) const -> bool {
    return this->operator==(std::string_view{other});
}

template<typename T>
auto ConcretePath<T>::canonicalized() const -> Expected<ConcretePath<std::string>> {
    auto parsed = parse_concrete_path(std::string_view{this->path});
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return ConcretePath<std::string>{parsed->canonical};
}

template<typename T>
auto ConcretePath<T>::components() const -> Expected<std::vector<std::string>> {
    auto parsed = parse_concrete_path(std::string_view{this->path});
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return std::move(parsed->components);
}

template<typename T>
auto ConcretePath<T>::isPrefixOf(ConcretePath<std::string_view> other) const -> Expected<bool> {
    auto lhsParsed = parse_concrete_path(std::string_view{this->path});
    if (!lhsParsed) {
        return std::unexpected(lhsParsed.error());
    }

    auto rhsParsed = parse_concrete_path(std::string_view{other.getPath()});
    if (!rhsParsed) {
        return std::unexpected(rhsParsed.error());
    }

    auto const& lhs = lhsParsed->components;
    auto const& rhs = rhsParsed->components;
    if (lhs.size() > rhs.size()) {
        return false;
    }
    for (std::size_t idx = 0; idx < lhs.size(); ++idx) {
        if (lhs[idx] != rhs[idx]) {
            return false;
        }
    }
    return true;
}

template struct ConcretePath<std::string>;
template struct ConcretePath<std::string_view>;
} // namespace SP
