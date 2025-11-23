#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace SP::Examples::CLI {

class ExampleCli {
public:
    using ParseError = std::optional<std::string>;

    ExampleCli();

    void set_program_name(std::string_view name);
    void set_unknown_argument_handler(std::function<bool(std::string_view)> handler);
    void set_error_logger(std::function<void(std::string const&)> logger);

    struct FlagOption {
        std::function<void()> on_set;
    };

    struct ValueOption {
        std::function<ParseError(std::optional<std::string_view>)> on_value;
        bool value_optional = false;
        bool consume_next_token = true;
        bool allow_leading_dash_value = false;
    };

    struct IntOption {
        std::function<void(int)> on_value;
    };

    struct DoubleOption {
        std::function<void(double)> on_value;
    };

    void add_flag(std::string_view name, FlagOption option);
    void add_value(std::string_view name, ValueOption option);
    void add_int(std::string_view name, IntOption option);
    void add_double(std::string_view name, DoubleOption option);
    void add_alias(std::string_view alias, std::string_view target);

    [[nodiscard]] bool parse(int argc, char** argv);
    [[nodiscard]] bool had_errors() const;

private:
    struct OptionEntry {
        std::string name;
        bool expects_value = false;
        bool value_optional = false;
        bool consume_next_token = true;
        bool allow_leading_dash_value = false;
        std::function<void()> flag_handler;
        std::function<ParseError(std::optional<std::string_view>)> value_handler;
    };

    OptionEntry* find_option(std::string_view name);
    void register_option(OptionEntry entry);
    void log_error(std::string_view message);
    bool looks_like_option(std::string_view token) const;
    void mark_error();

    std::vector<OptionEntry> options_;
    std::unordered_map<std::string, std::size_t> option_lookup_;
    std::string program_name_;
    std::function<bool(std::string_view)> unknown_handler_;
    std::function<void(std::string const&)> error_logger_;
    bool had_error_ = false;
};

} // namespace SP::Examples::CLI
