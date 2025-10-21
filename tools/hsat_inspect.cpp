#include <pathspace/ui/HtmlSerialization.hpp>
#include <pathspace/ui/HtmlAdapter.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace SP;

namespace {

struct CommandLineOptions {
    bool        pretty_output = false;
    bool        read_stdin    = false;
    std::string input_path;
};

void print_usage() {
    std::cout << "Usage: hsat_inspect <file> [--pretty]\n"
                 "       hsat_inspect --input <file> [--pretty]\n"
                 "       hsat_inspect - [--pretty]\n"
                 "       hsat_inspect --stdin [--pretty]\n\n"
                 "Decode an HSAT (Html Serialized Asset Table) payload and print a JSON summary.\n"
                 "Options:\n"
                 "  --input <file>   Read HSAT payload from the given file (binary)\n"
                 "  --stdin          Read payload bytes from standard input\n"
                 "  -                Shorthand for --stdin\n"
                 "  --pretty         Pretty-print JSON output with indentation\n"
                 "  --help           Show this help message\n";
}

auto parse_arguments(int argc, char** argv) -> std::optional<CommandLineOptions> {
    CommandLineOptions options{};
    bool seen_input = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg{argv[i]};
        if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(EXIT_SUCCESS);
        } else if (arg == "--pretty") {
            options.pretty_output = true;
        } else if (arg == "--stdin") {
            if (seen_input) {
                std::cerr << "Multiple input sources specified\n";
                return std::nullopt;
            }
            options.read_stdin = true;
            seen_input = true;
        } else if (arg == "--input" || arg == "-i") {
            if (seen_input) {
                std::cerr << "Multiple input sources specified\n";
                return std::nullopt;
            }
            if (i + 1 >= argc) {
                std::cerr << "--input requires a file path\n";
                return std::nullopt;
            }
            options.input_path = argv[++i];
            seen_input = true;
        } else if (arg == "-") {
            if (seen_input) {
                std::cerr << "Multiple input sources specified\n";
                return std::nullopt;
            }
            options.read_stdin = true;
            seen_input = true;
        } else if (!arg.empty() && arg.front() == '-') {
            std::cerr << "Unknown argument: " << arg << "\n";
            return std::nullopt;
        } else {
            if (seen_input) {
                std::cerr << "Multiple positional inputs specified\n";
                return std::nullopt;
            }
            options.input_path = std::move(arg);
            seen_input = true;
        }
    }

    if (!seen_input) {
        std::cerr << "No input specified\n";
        return std::nullopt;
    }

    if (options.read_stdin && !options.input_path.empty()) {
        std::cerr << "Cannot combine --stdin with a file path\n";
        return std::nullopt;
    }

    return options;
}

auto read_all_stdin() -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> data;
    constexpr std::size_t chunk_size = 4096;
    std::vector<char>      buffer(chunk_size);

    while (std::cin) {
        std::cin.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        auto const read_bytes = std::cin.gcount();
        if (read_bytes > 0) {
            auto const begin = reinterpret_cast<std::uint8_t const*>(buffer.data());
            data.insert(data.end(), begin, begin + read_bytes);
        }
        if (read_bytes < static_cast<std::streamsize>(buffer.size())) {
            break;
        }
    }
    return data;
}

auto read_all_file(std::string const& path) -> std::optional<std::vector<std::uint8_t>> {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
    return data;
}

auto escape_json(std::string_view input) -> std::string {
    std::string escaped;
    escaped.reserve(input.size());
    for (char ch : input) {
        switch (ch) {
        case '\"':
            escaped.append("\\\"");
            break;
        case '\\':
            escaped.append("\\\\");
            break;
        case '\b':
            escaped.append("\\b");
            break;
        case '\f':
            escaped.append("\\f");
            break;
        case '\n':
            escaped.append("\\n");
            break;
        case '\r':
            escaped.append("\\r");
            break;
        case '\t':
            escaped.append("\\t");
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char buffer[7];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(ch));
                escaped.append(buffer);
            } else {
                escaped.push_back(ch);
            }
            break;
        }
    }
    return escaped;
}

auto preview_hex(std::vector<std::uint8_t> const& bytes, std::size_t limit = 16) -> std::string {
    if (bytes.empty()) {
        return {};
    }
    std::ostringstream oss;
    oss << std::hex;
    for (std::size_t index = 0; index < bytes.size() && index < limit; ++index) {
        if (index > 0) {
            oss << ' ';
        }
        oss.width(2);
        oss.fill('0');
        oss << static_cast<int>(bytes[index]);
    }
    if (bytes.size() > limit) {
        oss << " …";
    }
    return oss.str();
}

auto classify_asset(std::string_view mime, std::string_view logical, bool is_reference) -> std::string {
    if (is_reference) {
        if (mime == UI::Html::kImageAssetReferenceMime) {
            return "image-reference";
        }
        if (mime == UI::Html::kFontAssetReferenceMime) {
            return "font-reference";
        }
        return "reference";
    }
    auto starts_with = [](std::string_view value, std::string_view prefix) -> bool {
        return value.substr(0, prefix.size()) == prefix;
    };
    if (starts_with(mime, "image/")) {
        return "image";
    }
    if (starts_with(mime, "font/") || starts_with(mime, "application/font")) {
        return "font";
    }
    if (starts_with(mime, "text/")) {
        return "text";
    }
    if (starts_with(mime, "application/json")) {
        return "json";
    }
    if (starts_with(logical, "images/")) {
        return "image";
    }
    if (starts_with(logical, "fonts/")) {
        return "font";
    }
    return "binary";
}

auto is_reference_mime(std::string_view mime) -> bool {
    return mime == UI::Html::kImageAssetReferenceMime
        || mime == UI::Html::kFontAssetReferenceMime;
}

} // namespace

int main(int argc, char** argv) {
    auto options = parse_arguments(argc, argv);
    if (!options) {
        print_usage();
        return EXIT_FAILURE;
    }

    std::vector<std::uint8_t> payload;
    if (options->read_stdin) {
        payload = read_all_stdin();
    } else {
        auto data = read_all_file(options->input_path);
        if (!data) {
            std::cerr << "Failed to read HSAT payload from '" << options->input_path << "'\n";
            return EXIT_FAILURE;
        }
        payload = std::move(*data);
    }

    if (payload.empty()) {
        std::cerr << "Input payload is empty\n";
        return EXIT_FAILURE;
    }

    SlidingBuffer buffer;
    buffer.append(payload.data(), payload.size());

    auto decoded = decode_html_assets_payload(buffer);
    if (!decoded) {
        auto const& error = decoded.error();
        std::cerr << "Failed to decode HSAT payload (code="
                  << static_cast<int>(error.code) << "): "
                  << error.message.value_or("unspecified error") << "\n";
        return EXIT_FAILURE;
    }

    auto const& assets = decoded->assets;
    std::size_t total_bytes = 0;
    bool        has_references = false;
    for (auto const& asset : assets) {
        total_bytes += asset.bytes.size();
        if (is_reference_mime(asset.mime_type)) {
            has_references = true;
        }
    }
    auto const trailing_bytes = payload.size() > decoded->bytes_consumed
                                ? payload.size() - decoded->bytes_consumed
                                : 0;

    auto indent = [&](int level) {
        if (options->pretty_output) {
            std::cout << std::string(level * 2, ' ');
        }
    };

    auto newline = [&]() {
        if (options->pretty_output) {
            std::cout << "\n";
        }
    };

    std::cout << "{";
    newline();
    indent(1);
    std::cout << "\"assetCount\":" << assets.size() << ",";
    newline();
    indent(1);
    std::cout << "\"totalBytes\":" << total_bytes << ",";
    newline();
    indent(1);
    std::cout << "\"bytesConsumed\":" << decoded->bytes_consumed << ",";
    newline();
    indent(1);
    std::cout << "\"trailingBytes\":" << trailing_bytes << ",";
    newline();
    indent(1);
    std::cout << "\"hasReferences\":" << (has_references ? "true" : "false") << ",";
    newline();
    indent(1);
    std::cout << "\"assets\":";
    if (options->pretty_output) {
        std::cout << "[";
        newline();
    } else {
        std::cout << "[";
    }

    for (std::size_t index = 0; index < assets.size(); ++index) {
        auto const& asset = assets[index];
        auto const  reference = is_reference_mime(asset.mime_type);
        auto const  kind = classify_asset(asset.mime_type, asset.logical_path, reference);
        auto const  preview = preview_hex(asset.bytes);

        indent(2);
        std::cout << "{";
        newline();

        auto print_field = [&](std::string_view name, std::string_view value, bool comma) {
            indent(3);
            std::cout << "\"" << name << "\":\"" << escape_json(value) << "\"";
            if (comma) {
                std::cout << ",";
            }
            newline();
        };

        auto print_field_num = [&](std::string_view name, std::uint64_t value, bool comma) {
            indent(3);
            std::cout << "\"" << name << "\":" << value;
            if (comma) {
                std::cout << ",";
            }
            newline();
        };

        auto print_field_bool = [&](std::string_view name, bool value, bool comma) {
            indent(3);
            std::cout << "\"" << name << "\":" << (value ? "true" : "false");
            if (comma) {
                std::cout << ",";
            }
            newline();
        };

        print_field_num("index", index, true);
        print_field("logicalPath", asset.logical_path, true);
        print_field("mimeType", asset.mime_type, true);
        print_field_num("byteLength", static_cast<std::uint64_t>(asset.bytes.size()), true);
        print_field("kind", kind, true);
        print_field_bool("reference", reference, !preview.empty());
        if (!preview.empty()) {
            print_field("bytePreviewHex", preview, false);
        }

        indent(2);
        std::cout << "}";
        if (index + 1 < assets.size()) {
            std::cout << ",";
        }
        newline();
    }

    indent(1);
    std::cout << "]";
    newline();
    std::cout << "}";
    if (!options->pretty_output) {
        std::cout << "\n";
    }

    return EXIT_SUCCESS;
}
