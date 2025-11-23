#include "ExampleCli.hpp"

#include <charconv>
#include <iostream>
#include <sstream>
#include <string>

namespace SP::Examples::CLI {

ExampleCli::ExampleCli() {
    unknown_handler_ = [this](std::string_view token) {
        std::string message = "ignoring unknown argument '";
        message.append(token.begin(), token.end());
        message.push_back('\'');
        log_error(message);
        return true;
    };
}

void ExampleCli::set_program_name(std::string_view name) {
    program_name_.assign(name.begin(), name.end());
}

void ExampleCli::set_unknown_argument_handler(std::function<bool(std::string_view)> handler) {
    unknown_handler_ = std::move(handler);
}

void ExampleCli::set_error_logger(std::function<void(std::string const&)> logger) {
    error_logger_ = std::move(logger);
}

void ExampleCli::add_flag(std::string_view name, FlagOption option) {
    OptionEntry entry;
    entry.name.assign(name.begin(), name.end());
    entry.expects_value = false;
    entry.flag_handler = std::move(option.on_set);
    register_option(std::move(entry));
}

void ExampleCli::add_value(std::string_view name, ValueOption option) {
    OptionEntry entry;
    entry.name.assign(name.begin(), name.end());
    entry.expects_value = true;
    entry.value_optional = option.value_optional;
    entry.consume_next_token = option.consume_next_token;
    entry.allow_leading_dash_value = option.allow_leading_dash_value;
    entry.value_handler = std::move(option.on_value);
    register_option(std::move(entry));
}

void ExampleCli::add_int(std::string_view name, IntOption option) {
    ValueOption value_opt{};
    value_opt.on_value = [stored = std::string(name), handler = std::move(option.on_value)](std::optional<std::string_view> token) -> ParseError {
        if (!token || token->empty()) {
            return stored + " requires an integer value";
        }
        int value = 0;
        auto begin = token->data();
        auto end = begin + token->size();
        auto result = std::from_chars(begin, end, value);
        if (result.ec != std::errc{}) {
            return stored + " expects a numeric value";
        }
        handler(value);
        return std::nullopt;
    };
    add_value(name, value_opt);
}

void ExampleCli::add_double(std::string_view name, DoubleOption option) {
    ValueOption value_opt{};
    value_opt.on_value = [stored = std::string(name), handler = std::move(option.on_value)](std::optional<std::string_view> token) -> ParseError {
        if (!token || token->empty()) {
            return stored + " requires a floating-point value";
        }
        std::string buffer(token->begin(), token->end());
        std::stringstream stream(buffer);
        double value = 0.0;
        stream >> value;
        if (stream.fail() || !stream.eof()) {
            return stored + " expects a floating-point value";
        }
        handler(value);
        return std::nullopt;
    };
    add_value(name, value_opt);
}

void ExampleCli::add_alias(std::string_view alias, std::string_view target) {
    auto target_it = option_lookup_.find(std::string(target));
    if (target_it == option_lookup_.end()) {
        std::string message = "missing option for alias '";
        message.append(target.begin(), target.end());
        message.push_back('\'');
        log_error(message);
        mark_error();
        return;
    }
    option_lookup_.emplace(std::string(alias), target_it->second);
}

bool ExampleCli::parse(int argc, char** argv) {
    had_error_ = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view raw_token{argv[i]};
        std::optional<std::string_view> attached_value;
        std::string_view name = raw_token;
        auto equals_pos = raw_token.find('=');
        if (equals_pos != std::string_view::npos) {
            name = raw_token.substr(0, equals_pos);
            attached_value = raw_token.substr(equals_pos + 1);
        }

        OptionEntry* entry = find_option(name);
        if (entry == nullptr) {
            if (unknown_handler_) {
                bool ok = unknown_handler_(raw_token);
                if (!ok) {
                    mark_error();
                }
            }
            continue;
        }

        if (!entry->expects_value) {
            if (attached_value && !attached_value->empty()) {
                log_error(entry->name + " does not accept a value");
                mark_error();
                continue;
            }
            if (entry->flag_handler) {
                entry->flag_handler();
            }
            continue;
        }

        std::optional<std::string_view> resolved_value = attached_value;
        if (!resolved_value) {
            if (entry->value_optional) {
                bool consumed = false;
                if (entry->consume_next_token && (i + 1) < argc) {
                    std::string_view candidate{argv[i + 1]};
                    bool treat_as_option = looks_like_option(candidate) && !entry->allow_leading_dash_value;
                    if (!treat_as_option) {
                        resolved_value = candidate;
                        consumed = true;
                    }
                }
                if (consumed) {
                    ++i;
                }
                if (!resolved_value) {
                    if (entry->value_handler) {
                        auto error = entry->value_handler(std::nullopt);
                        if (error) {
                            log_error(*error);
                            mark_error();
                        }
                    }
                    continue;
                }
            } else {
                if ((i + 1) >= argc) {
                    log_error(entry->name + " requires a value");
                    mark_error();
                    continue;
                }
                ++i;
                resolved_value = std::string_view{argv[i]};
            }
        }

        if (entry->value_handler) {
            auto error = entry->value_handler(resolved_value);
            if (error) {
                log_error(*error);
                mark_error();
            }
        }
    }
    return !had_error_;
}

bool ExampleCli::had_errors() const {
    return had_error_;
}

ExampleCli::OptionEntry* ExampleCli::find_option(std::string_view name) {
    auto it = option_lookup_.find(std::string(name));
    if (it == option_lookup_.end()) {
        return nullptr;
    }
    return &options_[it->second];
}

void ExampleCli::register_option(OptionEntry entry) {
    options_.push_back(std::move(entry));
    auto index = options_.size() - 1;
    option_lookup_.emplace(options_.back().name, index);
}

void ExampleCli::log_error(std::string_view message) {
    std::string text;
    if (program_name_.empty()) {
        text.assign("example_cli");
    } else {
        text = program_name_;
    }
    text.append(": ");
    text.append(message.begin(), message.end());
    if (error_logger_) {
        error_logger_(text);
    } else {
        std::cerr << text << '\n';
    }
}

bool ExampleCli::looks_like_option(std::string_view token) const {
    return token.size() > 1 && token.front() == '-';
}

void ExampleCli::mark_error() {
    had_error_ = true;
}

} // namespace SP::Examples::CLI
