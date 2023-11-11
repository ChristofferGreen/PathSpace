#pragma once

#include <string>
#include <vector>
#include <string_view>
#include <optional>
#include <sstream>
#include <iterator>
#include <algorithm>
#include <map>
#include <regex>

#include "Error.hpp"

namespace SP {

class SpacePathParser {
public:
    static std::optional<Error> validatePath(std::string_view path) {
        if (path.empty() || path[0] != '/') {
            return Error{Error::Code::InvalidPath, "Path must start with '/'"};
        }
        // Additional validation checks can be added here
        return std::nullopt;
    }

    static std::vector<std::string> splitPath(std::string_view path) {
        std::vector<std::string> components;
        if (!path.empty() && path[0] == '/') {
            path.remove_prefix(1); // Remove the leading '/'
        }
        std::stringstream ss{std::string(path)};
        std::string component;
        while (std::getline(ss, component, '/')) {
            if (!component.empty()) {
                components.push_back(component);
            }
        }
        return components;
    }
};

class SpacePath {
    std::vector<std::string> pathComponents;

public:
    SpacePath() {}
    explicit SpacePath(std::string_view path) {
        if (auto error = SpacePathParser::validatePath(path); error.has_value()) {
            throw std::invalid_argument(error->message.value_or("Invalid path"));
        }
        pathComponents = SpacePathParser::splitPath(path);
    }

    const std::vector<std::string>& getPathComponents() const {
        return this->pathComponents;
    }

    std::string toString() const {
        std::string fullPath = "/";
        for (const auto& comp : pathComponents) {
            fullPath += comp + "/";
        }
        fullPath.pop_back(); // Remove the trailing '/'
        return fullPath;
    }

    bool operator<(const SpacePath& other) const {
        return std::lexicographical_compare(
            this->pathComponents.begin(), this->pathComponents.end(),
            other.pathComponents.begin(), other.pathComponents.end());
    }

    auto matches(const SpacePath& other) const -> bool {
        // If the number of components is different, they do not match
        if (pathComponents.size() != other.pathComponents.size())
            return false;

        // Check each component for a match using regex
        for (size_t i = 0; i < pathComponents.size(); ++i) {
            std::regex componentRegex(wildcardToRegex(pathComponents[i]));
            if (!std::regex_match(other.pathComponents[i], componentRegex)) {
                return false;
            }
        }
        return true;
    }

private:
// Converts a path component with wildcards to a regex
    std::string wildcardToRegex(const std::string& wildcard) const {
        // Escape all regex characters except for '*'
        std::string regexStr;
        for (char c : wildcard) {
            switch (c) {
                case '*':
                    regexStr += ".*"; // '*' is replaced with '.*' (regex wildcard)
                    break;
                case '.':
                case '\\':
                case '+':
                case '?':
                case '{':
                case '}':
                case '(':
                case ')':
                case '[':
                case ']':
                case '^':
                case '$':
                case '|':
                    regexStr += '\\'; // Fallthrough intended
                default:
                    regexStr += c; // Regular characters remain the same
                    break;
            }
        }
        return regexStr;
    }
    // Other methods like removeLastComponent(), etc., can be added as needed.
};

}