#pragma once

#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <map>

namespace SP {

class SpacePath {
public:
    // Empty constructor
    SpacePath() = default;

    SpacePath(std::string_view path) : path_(path), pattern_string_(convertToRegex(path)), pattern_(pattern_string_) {}

    SpacePath(char const * path)
        : SpacePath(std::string_view(path)) {}

    // Method to match another SpacePath with wildcards
    bool matches(const SpacePath& other) const {
        if (pattern_string_.empty()) return false; // No pattern compiled
        return std::regex_match(other.path_, pattern_);
    }

    // Method to get the string representation of the path
    std::string toString() const {
        return path_;
    }

    // Comparison operator for std::map
    bool operator<(const SpacePath& other) const {
        return path_ < other.path_;
    }

    // Equality operator for std::unordered_map
    bool operator==(const SpacePath& other) const {
        if (pattern_string_.empty()) return path_ == other.path_;  // Direct comparison if no pattern
        return std::regex_match(other.path_, pattern_);
    }

    static bool bidirectionalMatch(const SpacePath& a, const SpacePath& b) {
        return std::regex_match(a.path_, b.pattern_) || std::regex_match(b.path_, a.pattern_);
    }

    // Static function to find a matching path with wildcards in a map
    static bool containsWithWildcard(const auto& map, const SpacePath& searchPath) {
        for (const auto& [key, value] : map) {
            if (bidirectionalMatch(key, searchPath)) {
                return true;
            }
        }
        return false;
    }

    template<typename MapType>
    static auto findWithWildcard(MapType& map, const SpacePath& searchPath) -> typename MapType::const_iterator {
        for (auto it = map.begin(); it != map.end(); ++it) {
            if (bidirectionalMatch(it->first, searchPath)) {
                return it;
            }
        }
        return map.end();
    }

private:
    std::string path_;
    std::string pattern_string_;
    std::regex pattern_;

    // Helper function to convert wildcards to regex pattern
    static std::string convertToRegex(std::string_view path) {
        std::string regex_pattern;
        regex_pattern.reserve(path.length() * 2); // Reserve to avoid frequent allocations

        for (const auto& ch : path) {
            switch (ch) {
                case '*': regex_pattern += ".*"; break;
                case '?': regex_pattern += '.'; break;
                case '/': regex_pattern += "\\/"; break;
                default: regex_pattern += ch;
            }
        }
        
        // Replace "**" with ".*" which crosses directories
        std::string special_star = "\\*\\*";
        regex_pattern = std::regex_replace(regex_pattern, std::regex(special_star), ".*");

        return regex_pattern;
    }
};

} // namespace SP

// Custom hash function for unordered_map
struct SpacePathHash {
    std::size_t operator()(const SP::SpacePath& sp) const {
        return std::hash<std::string>()(sp.toString());
    }
};

// Custom equality function for unordered_map
struct SpacePathEqual {
    bool operator()(const SP::SpacePath& lhs, const SP::SpacePath& rhs) const {
        return lhs == rhs;
    }
};
