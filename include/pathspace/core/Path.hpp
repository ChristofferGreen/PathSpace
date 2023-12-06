#pragma once

#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <map>

namespace SP {

/*
Path Specification
A path is very similar to a typical linux path and can contain glob patterns such as for example a* to match against anything starting with a.
Glob patterns supported:
    * Match one or more characters
    ? Match one character
    ** Match one or more characters, but can cross directory separators
If the filename contains characters that would be parsed as a glob patterns the path should be encased in '' characters like so: 'examples*.txt'
*/

class Path {
public:
    // Empty constructor
    Path() = default;

    Path(std::string_view path) : path(stripEscaped(path)), pattern(convertToRegex(path)) {}

    Path(char const * path)
        : Path(std::string_view(path)) {}

    bool matches(const Path& other) const {
        if (!this->pattern) return this->path==other.path; // No pattern compiled
        return std::regex_match(other.path, *pattern);
    }

    // Method to get the string representation of the path
    std::string toString() const {
        return this->path;
    }

    // Comparison operator for std::map
    bool operator<(const Path& other) const {
        return this->path < other.path;
    }

    // Equality operator for std::unordered_map
    bool operator==(const Path& other) const {
        if (!this->pattern) return path == other.path;  // Direct comparison if no pattern
        return std::regex_match(other.path, *pattern);
    }

    static bool bidirectionalMatch(const Path& a, const Path& b) {
        if(b.pattern && std::regex_match(a.path, *b.pattern))
            return true;
        if(a.pattern && std::regex_match(b.path, *a.pattern))
            return true;
        if (!b.pattern && !a.pattern)
            return a.path == b.path;  // Direct comparison if no pattern
        return false;
    }

    // Static function to find a matching path with wildcards in a map
    static bool containsWithWildcard(const auto& map, const Path& searchPath) {
        for (const auto& [key, value] : map) {
            if (bidirectionalMatch(key, searchPath)) {
                return true;
            }
        }
        return false;
    }

    template<typename MapType>
    static auto findWithWildcard(MapType& map, const Path& searchPath) -> typename MapType::const_iterator {
        for (auto it = map.begin(); it != map.end(); ++it) {
            if (bidirectionalMatch(it->first, searchPath)) {
                return it;
            }
        }
        return map.end();
    }

private:
    std::string path;
    std::optional<std::regex> pattern;

    static std::string stripEscaped(std::string_view path) {
        if(!path.contains('\\'))
            return std::string(path);
        std::string ret;
        bool prevWasEscape=false;
        for (const auto ch : path) {
            if(prevWasEscape && ch != '*' && ch != '?')
                ret += '\\';
            if(ch != '\\')
                ret += ch;
            prevWasEscape = (ch == '\\');
        }
        ret.reserve(path.length() * 2);
        return ret;
    }

    // Helper function to convert glob patterns to regex pattern
    static std::optional<std::regex> convertToRegex(std::string_view path) {
        bool foundGlobPattern = false;
        bool prevWasEscape = false;
        for (const auto ch : path) {
            if(ch == '\\') {
                prevWasEscape = true;
            }
            if(ch == '*' || ch == '?') {
                if(!prevWasEscape) {
                    foundGlobPattern = true;
                    break;
                }
                prevWasEscape = false;
            }
        }
        if(!foundGlobPattern)
            return std::nullopt;
        std::string regex_pattern;
        regex_pattern.reserve(path.length() * 2); // Reserve to avoid frequent allocations

        for (const auto ch : path) {
            switch (ch) {
                case '*': regex_pattern += ".*"; break;
                case '?': regex_pattern += '.'; break;
                default: regex_pattern += ch;
            }
        }
        
        // Replace "**" with ".*" which crosses directories
        std::string special_star = "\\*\\*";
        regex_pattern = std::regex_replace(regex_pattern, std::regex(special_star), ".*");

        return std::regex(regex_pattern);
    }
};

} // namespace SP

// Custom hash function for unordered_map
struct PathHash {
    std::size_t operator()(const SP::Path& sp) const {
        return std::hash<std::string>()(sp.toString());
    }
};

// Custom equality function for unordered_map
struct PathEqual {
    bool operator()(const SP::Path& lhs, const SP::Path& rhs) const {
        return lhs == rhs;
    }
};
