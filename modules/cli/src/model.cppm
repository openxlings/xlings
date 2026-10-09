// A command tree, declared once, and everything derived from it.
export module xlings.cli.model;

import std;
import xlings.libs.json;

export namespace xlings::cli::spec {

// How far into the help a command or an option is shown. `--help` lists the
// common ones; `help --all` adds more; the expert ones are in `help --all
// --expert` and the reference. Progressive disclosure: a person reads only
// what the task in front of them needs.
enum class Level { Common, More, Expert };

struct ArgSpec {
    std::string name;
    std::string description;
    bool required { false };
    bool variadic { false };
};

struct OptionSpec {
    std::string syntax;
    std::string description;
    // Accepted by every command, not only the one that lists it (a command
    // that cannot act on it ignores it). Declared on the root.
    bool global { false };
    Level level { Level::Common };
};

struct CommandSpec {
    std::string name;
    std::string description;
    std::vector<std::string> aliases;
    std::vector<ArgSpec> arguments;
    std::vector<OptionSpec> options;
    std::vector<CommandSpec> children;
    Level level { Level::Common };
};

struct ParsedManualArgs {
    std::vector<std::string> positional;
    std::set<std::string> options;
};

struct CliError {
    std::string message;
};

// The command at `path` below `root` (names or aliases), or null.
const CommandSpec* find_in(const CommandSpec& root, std::span<const std::string_view> path);

// `-g, --global` / `--ttl <SECONDS>` -> {"-g", "--global"} / {"--ttl"}.
std::vector<std::string_view> option_aliases(const OptionSpec& option);

// The root's options every command accepts.
std::vector<const OptionSpec*> global_options_of(const CommandSpec& root);

// argv after `command`'s own name, checked against the tree. `path` is the
// invocation as typed (`xlings subos use`), for the diagnostic.
std::expected<ParsedManualArgs, CliError> validate_in(const CommandSpec& root, const CommandSpec& command,
                                                      std::span<const std::string_view> argv,
                                                      const std::string& path);

nlohmann::json help_json(const CommandSpec& command);

// Help text for `command`, as `program` (its full invocation, `luban new`),
// showing what is at `upto` or below. When something is held back, the last
// line says how to see it.
std::string render_help(const CommandSpec& root, const CommandSpec& command, std::string_view program,
                        Level upto);

// Names close to a mistyped one (a command's children, or its options when
// `token` starts with '-'), nearest first.
std::vector<std::string> suggest(const CommandSpec& root, const CommandSpec& command, std::string_view token);

}  // namespace xlings::cli::spec

export namespace xlings::cli::completion {

struct Candidate {
    std::string value;
    std::string description;
};

// What the engine resolved, handed to the Provider so it can answer from live
// state. Exactly one of `argument` and `option` is normally set.
struct Request {
    std::string command;    // space-joined path, e.g. "subos use"; empty at the root
    std::string argument;   // the positional argument being filled
    std::string option;     // the option whose value is being filled
};

using Provider = std::function<std::vector<Candidate>(const Request&)>;

// `words` is argv after the program name, the LAST element the token under the
// cursor (possibly empty). Candidates already match that token as a prefix.
std::vector<Candidate> complete_in(const spec::CommandSpec& root, std::span<const std::string> words,
                                   const Provider& provider);

}  // namespace xlings::cli::completion
