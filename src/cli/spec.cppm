export module xlings.cli.spec;

import std;
import xlings.libs.json;

export namespace xlings::cli::spec {

struct ArgSpec {
    std::string name;
    std::string description;
    bool required { false };
    bool variadic { false };
};

struct OptionSpec {
    std::string syntax;
    std::string description;
    // Accepted by every command, not only the one that lists it. `xlings
    // --help` documents these without qualification, so a user -- or an agent
    // told to "ALWAYS add --yes" -- will append them to commands that have
    // nothing to confirm. Refusing those with exit 2 makes a documented
    // spelling a trap; a command that cannot act on a global option ignores
    // it. `-h`/`--version` are NOT global: they are commands in their own
    // right, and accepting them mid-argv would make
    // `xlings subos new foo --version` silently create a subos.
    bool global { false };
};

struct CommandSpec {
    std::string name;
    std::string description;
    std::vector<std::string> aliases;
    std::vector<ArgSpec> arguments;
    std::vector<OptionSpec> options;
    std::vector<CommandSpec> children;
};

const CommandSpec& root();

const CommandSpec* find(std::span<const std::string_view> path);

struct ParsedManualArgs {
    std::vector<std::string> positional;
    std::set<std::string> options;
};

struct CliError {
    std::string message;
};

// `-g, --global` / `--ttl <SECONDS>` -> {"-g", "--global"} / {"--ttl"}.
std::vector<std::string_view> option_aliases(const OptionSpec& option);

// The subset of root's options every command accepts. Kept as a view over
// root() rather than a second list so `--help`, the generated reference and
// this predicate cannot disagree about what "global" means.
const std::vector<const OptionSpec*>& global_options();

bool is_global_option(std::string_view token);

namespace detail_ {

// Non-flag tokens from `from` onward. Used to decide whether an optional
// value may eat the next token or whether a required positional still needs it.
std::size_t free_tokens_(std::span<const std::string_view> argv,
                         std::size_t from);

std::expected<ParsedManualArgs, CliError> validate_(
    const CommandSpec& command,
    std::span<const std::string_view> argv,
    // Full invocation path, so a diagnostic names the command the user typed.
    // `command.name` alone turns `xlings subos use` into `xlings use` -- a
    // different, existing command -- and `xlings self doctor` into
    // `xlings doctor`, which does not exist at all.
    const std::string& path);

}  // namespace detail_

std::expected<ParsedManualArgs, CliError> validate_manual_argv(
    const CommandSpec& command,
    std::span<const std::string_view> argv);

nlohmann::json help_json(const CommandSpec& command);

nlohmann::json reference_json();

std::string agent_reference();

}  // namespace xlings::cli::spec
