// xlings's command tree. The model it is declared with (and validation,
// help, completion) is xlings.cli.model, shared with luban.
export module xlings.cli.spec;

import std;
import xlings.libs.json;
export import xlings.cli.model;

export namespace xlings::cli::spec {

const CommandSpec& root();

const CommandSpec* find(std::span<const std::string_view> path);

// The subset of root's options every command accepts. Kept as a view over
// root() rather than a second list so `--help`, the generated reference and
// this predicate cannot disagree about what "global" means. `-h`/`--version`
// are NOT global: they are commands in their own right, and accepting them
// mid-argv would make `xlings subos new foo --version` silently create a subos.
const std::vector<const OptionSpec*>& global_options();

bool is_global_option(std::string_view token);

std::expected<ParsedManualArgs, CliError> validate_manual_argv(
    const CommandSpec& command,
    std::span<const std::string_view> argv);

nlohmann::json reference_json();

std::string agent_reference();

}  // namespace xlings::cli::spec
