module;
#include <cstdlib>
export module xlings.core.utils;

import std;

export namespace xlings::utils {

[[nodiscard]] std::string get_env_or_default(std::string_view name, std::string_view defaultValue = "");

// Whether the variable EXISTS in the environment -- empty or not.
//
// `get_env_or_default` cannot answer this: it returns "" both for a variable
// that is unset and for one the user deliberately exported empty. The `set`
// env op needs the difference. An explicit `export FOO=` is a value the user
// chose, so `set` must leave it alone; collapsing the two made the four
// backends disagree, since `${VAR:=v}` and a `-not $env:VAR` test overwrite an
// empty value while fish's `set -q` does not.
[[nodiscard]] bool env_is_set(const std::string& name);

[[nodiscard]] std::string strip_ansi(const std::string& str);

[[nodiscard]] std::string trim_string(const std::string& str);

[[nodiscard]] std::vector<std::string> split_string(const std::string& str, char delimiter);

bool ask_yes_no(const std::string& question, bool defaultYes = false);

std::string ask_input(const std::string& prompt, const std::string& defaultValue = "");

void print_separator(const std::string& title);

} // export namespace xlings::utils
