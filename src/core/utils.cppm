module;
#include <cstdlib>
export module xlings.core.utils;

import std;

export namespace xlings::utils {

[[nodiscard]] std::string get_env_or_default(std::string_view name, std::string_view defaultValue = "") {
    if (const char* value = std::getenv(name.data()); value != nullptr) {
        return value;
    }
    return std::string{defaultValue};
}

// Whether the variable EXISTS in the environment -- empty or not.
//
// `get_env_or_default` cannot answer this: it returns "" both for a variable
// that is unset and for one the user deliberately exported empty. The `set`
// env op needs the difference. An explicit `export FOO=` is a value the user
// chose, so `set` must leave it alone; collapsing the two made the four
// backends disagree, since `${VAR:=v}` and a `-not $env:VAR` test overwrite an
// empty value while fish's `set -q` does not.
[[nodiscard]] bool env_is_set(const std::string& name) {
    return std::getenv(name.c_str()) != nullptr;
}

[[nodiscard]] std::string strip_ansi(const std::string& str) {
    std::string cleaned;
    cleaned.reserve(str.size());

    for (std::size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '\x1b' && i + 1 < str.size() && str[i + 1] == '[') {
            i += 2;
            while (i < str.size() && str[i] != 'm') {
                ++i;
            }
        } else {
            cleaned.push_back(str[i]);
        }
    }

    return cleaned;
}

[[nodiscard]] std::string trim_string(const std::string& str) {
    auto cleaned = strip_ansi(str);

    auto start = std::ranges::find_if_not(cleaned,
        [](unsigned char ch) { return std::isspace(ch); });
    auto endIt = std::ranges::find_if_not(cleaned | std::views::reverse,
        [](unsigned char ch) { return std::isspace(ch); }).base();

    return std::string(start, endIt);
}

[[nodiscard]] std::vector<std::string> split_string(const std::string& str, char delimiter) {
    std::vector<std::string> result;
    auto view = str | std::views::split(delimiter);
    for (const auto& part : view) {
        result.push_back(std::string(part.begin(), part.end()));
    }
    return result;
}

bool ask_yes_no(const std::string& question, bool defaultYes = false) {
    std::string prompt = defaultYes ? "[Y/n] " : "[y/N] ";
    std::print("{}{}", question, prompt);
    std::cout.flush();

    std::string input;
    if (!std::getline(std::cin, input)) return defaultYes;
    if (input.empty()) return defaultYes;

    return input[0] == 'y' || input[0] == 'Y';
}

std::string ask_input(const std::string& prompt, const std::string& defaultValue = "") {
    std::string displayDefault = defaultValue.empty() ? "(empty)" : defaultValue;
    std::print("{} [{}]: ", prompt, displayDefault);
    std::cout.flush();

    std::string input;
    if (!std::getline(std::cin, input)) return defaultValue;

    return input.empty() ? defaultValue : input;
}

void print_separator(const std::string& title) {
    std::println("\n=== {} ===", title);
}

} // export namespace xlings::utils
