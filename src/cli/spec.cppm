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

const CommandSpec& root() {
    static const CommandSpec value{
        .name = "xlings",
        .description = "Universal package management and SubOS environments",
        .options = {
            {"-h, --help", "Show help for the selected command"},
            {"--version", "Show version"},
            {"-y, --yes", "Skip confirmation prompts", true},
            {"--agent", "Use stable plain-text output", true},
            {"-v, --verbose", "Enable verbose output", true},
            {"-q, --quiet", "Suppress non-essential output", true},
        },
        .children = {
            {"install", "Install packages", {}, {{"packages", "Package names", false, true}},
                {{"-g, --global", "Use global scope"}, {"-u, --use", "Activate installed version"}}, {}},
            {"remove", "Remove a package", {}, {{"package", "Package name", true}, {"version", "Optional version", false}},
                {{"-g, --global", "Use global scope"}}, {}},
            {"update", "Update package index or package", {}, {{"package", "Optional package", false}, {"version", "Optional version", false}}, {}, {}},
            {"search", "Search for packages", {}, {{"keyword", "Search keyword", true}}, {}, {}},
            {"list", "List installed packages", {}, {{"filter", "Filter pattern", false}},
                {{"-a, --all", "Show every subos"}}, {}},
            {"info", "Show package information", {}, {{"package", "Package name", true}, {"version", "Optional version", false}},
                {{"--all-versions", "Show every available version"}}, {}},
            {"use", "Switch tool version", {}, {{"target", "Tool name", true}, {"version", "Optional version", false}},
                {{"-a, --all", "Show every subos"}, {"--strict", "Require a coherent release"}}, {}},
            {"config", "Show or modify configuration", {}, {},
                {{"--lang <LANG>", "Set language"}, {"--mirror <MIRROR>", "Set mirror"}, {"--add-xpkg <FILE>", "Add package recipe"}, {"--index-repo <NS:URL>", "Add index repository"}}, {}},
            {"subos", "Manage SubOS environments", {}, {}, {}, {
                {"new", "Create a SubOS", {}, {{"name", "SubOS name", true}}, {{"--storage <MODE>", "shared, tmpfs or image"}, {"--image-size <SIZE>", "Image size"}, {"--from <SOURCE>", "Fork source"}, {"--runtime <SPEC>", "Runtime binding, e.g. glibc@2.39"}}, {}},
                {"use", "Enter a SubOS", {}, {{"name", "SubOS name", true}}, {{"--global", "Persist the active SubOS"}, {"--shell [KIND]", "Emit shell activation code"}, {"--sandbox [BACKEND]", "Enable sandbox (bwrap or proot on Linux)"}, {"--cmd <COMMAND>", "Run one command"}, {"--keep", "Keep the namespace keeper"}, {"--no-keep", "Disable the namespace keeper"}, {"--ttl <SECONDS>", "Keeper idle timeout"}, {"--gpu", "Expose GPU devices (bwrap only)"}}, {}},
                {"list", "List SubOS environments", {"ls"}, {}, {}, {}},
                {"remove", "Remove a SubOS", {"rm"}, {{"name", "SubOS name", true}}, {}, {}},
                {"info", "Show SubOS details", {"i"}, {{"name", "Optional SubOS", false}}, {}, {}},
                {"stop", "Stop a SubOS keeper", {}, {{"name", "SubOS name", true}}, {}, {}},
            }},
            {"self", "Manage xlings itself", {}, {}, {}, {
                {"install", "Install xlings", {}, {}, {}, {}},
                {"uninstall", "Uninstall xlings", {}, {}, {{"-y, --yes", "Skip confirmation"}, {"--keep-data", "Keep data"}, {"--dry-run", "Preview"}}, {}},
                {"init", "Initialize directories", {}, {}, {}, {}},
                {"update", "Update xlings", {}, {}, {}, {}},
                {"config", "Show configuration", {}, {}, {}, {}},
                {"clean", "Clean cache", {}, {}, {{"--dry-run", "Preview"}}, {}},
                {"migrate", "Migrate old layout", {}, {}, {}, {}},
                {"doctor", "Verify installation", {}, {}, {{"--fix", "Repair"}, {"--dry-run", "Preview"}, {"--all", "Show all findings"}, {"--reset-metadata", "Discard unreadable metadata"}}, {}},
            }},
            {"script", "Run an xlings script", {}, {{"script-file", "Script path", true}, {"args", "Script arguments", false, true}}, {}, {}},
            {"interface", "Use the NDJSON interface", {}, {{"capability", "Capability name", false}}, {{"--args <JSON>", "Capability arguments"}, {"--args-file <PATH>", "Read capability arguments from a file"}, {"--list", "List capabilities"}, {"--version", "Show protocol version"}}, {}},
            {"index", "Inspect and select package index snapshots", {}, {}, {}, {
                {"list", "List published index snapshots", {"ls"}, {{"name", "Optional index source", false}},
                    {{"--json", "Machine-readable output"}}, {}},
                {"use", "Pin an index source to a snapshot", {}, {{"name", "Index source", true}, {"version", "Snapshot version, or 'latest'", true}}, {}, {}},
            }},
            {"agent", "Agent integration", {}, {}, {}, {
                {"skills", "List or show built-in skills", {}, {{"name", "Optional skill name", false}}, {}, {}},
            }},
            {"profile", "Manage profile configuration", {}, {}, {}, {
                {"list", "List recorded generations", {}, {}, {}, {}},
                {"commit", "Record the active generation", {}, {{"reason", "Optional reason", false}}, {}, {}},
                {"rollback", "Restore a recorded generation", {}, {{"generation", "Generation number", true}}, {}, {}},
            }},
        },
    };
    return value;
}

const CommandSpec* find(std::span<const std::string_view> path) {
    const CommandSpec* current = &root();
    for (const auto part : path) {
        const auto found = std::ranges::find_if(current->children,
            [&](const CommandSpec& child) {
                return child.name == part
                    || std::ranges::find(child.aliases, part) != child.aliases.end();
            });
        if (found == current->children.end()) return nullptr;
        current = &*found;
    }
    return current;
}

struct ParsedManualArgs {
    std::vector<std::string> positional;
    std::set<std::string> options;
};

struct CliError {
    std::string message;
};

// `-g, --global` / `--ttl <SECONDS>` -> {"-g", "--global"} / {"--ttl"}.
std::vector<std::string_view> option_aliases(const OptionSpec& option) {
    std::vector<std::string_view> aliases;
    for (const auto piece : std::views::split(option.syntax, ',')) {
        auto alias = std::string_view{piece.begin(), piece.end()};
        while (!alias.empty() && alias.front() == ' ') alias.remove_prefix(1);
        alias = alias.substr(0, alias.find_first_of(" <[="));
        if (!alias.empty()) aliases.push_back(alias);
    }
    return aliases;
}

// The subset of root's options every command accepts. Kept as a view over
// root() rather than a second list so `--help`, the generated reference and
// this predicate cannot disagree about what "global" means.
const std::vector<const OptionSpec*>& global_options() {
    static const std::vector<const OptionSpec*> value = [] {
        std::vector<const OptionSpec*> options;
        for (const auto& option : root().options) {
            if (option.global) options.push_back(&option);
        }
        return options;
    }();
    return value;
}

bool is_global_option(std::string_view token) {
    const auto name = token.substr(0, token.find('='));
    for (const auto* option : global_options()) {
        for (const auto alias : option_aliases(*option)) {
            if (alias == name) return true;
        }
    }
    return false;
}

namespace detail_ {

// Non-flag tokens from `from` onward. Used to decide whether an optional
// value may eat the next token or whether a required positional still needs it.
std::size_t free_tokens_(std::span<const std::string_view> argv,
                         std::size_t from) {
    std::size_t count = 0;
    for (std::size_t i = from; i < argv.size(); ++i) {
        if (!argv[i].starts_with('-')) ++count;
    }
    return count;
}

std::expected<ParsedManualArgs, CliError> validate_(
    const CommandSpec& command,
    std::span<const std::string_view> argv,
    // Full invocation path, so a diagnostic names the command the user typed.
    // `command.name` alone turns `xlings subos use` into `xlings use` -- a
    // different, existing command -- and `xlings self doctor` into
    // `xlings doctor`, which does not exist at all.
    const std::string& path) {
    if (!command.children.empty() && !argv.empty()
        && !argv.front().starts_with('-')) {
        if (const auto* child = find(std::array<std::string_view, 2>{
                command.name, argv.front()})) {
            return validate_(*child, argv.subspan(1),
                             path + " " + std::string(argv.front()));
        }
        return std::unexpected(CliError{std::format(
            "unknown subcommand for `{}`: {}", path, argv.front())});
    }

    const auto required = static_cast<std::size_t>(
        std::ranges::count_if(command.arguments,
            [](const auto& argument) { return argument.required; }));

    ParsedManualArgs parsed;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const auto token = argv[i];
        if (!token.starts_with('-')) {
            parsed.positional.emplace_back(token);
            continue;
        }

        const auto equals = token.find('=');
        const auto optionName = token.substr(0, equals);
        const OptionSpec* matched = nullptr;
        for (const auto& option : command.options) {
            for (const auto alias : option_aliases(option)) {
                if (alias == optionName) { matched = &option; break; }
            }
            if (matched) break;
        }
        if (!matched) {
            for (const auto* option : global_options()) {
                for (const auto alias : option_aliases(*option)) {
                    if (alias == optionName) { matched = option; break; }
                }
                if (matched) break;
            }
        }
        if (!matched) {
            return std::unexpected(CliError{std::format(
                "unknown option for `{}`: {}", path, token)});
        }
        parsed.options.insert(std::string(optionName));

        const bool requiresValue = matched->syntax.contains('<');
        const bool optionalValue = matched->syntax.contains('[');
        if (equals != std::string_view::npos) {
            if (token.substr(equals + 1).empty()) {
                return std::unexpected(CliError{std::format(
                    "missing value for option: {}", optionName)});
            }
            continue;
        }
        if (requiresValue) {
            if (i + 1 >= argv.size() || argv[i + 1].starts_with('-')) {
                return std::unexpected(CliError{std::format(
                    "missing value for option: {}", optionName)});
            }
            ++i;
            continue;
        }
        if (optionalValue && i + 1 < argv.size()
            && !argv[i + 1].starts_with('-')) {
            // Take the next token as this option's value unless a required
            // positional still needs it. Deciding by a whitelist of known
            // values instead would mean every new shell kind, sandbox backend
            // or storage mode the parser learns silently reappears here as a
            // "surplus positional argument" -- which is exactly how
            // `--shell powershell` became an exit-2 error while the parser
            // that runs it accepted the word.
            if (parsed.positional.size()
                    + detail_::free_tokens_(argv, i + 2) >= required) {
                ++i;
            }
        }
    }

    const bool variadic = !command.arguments.empty()
        && command.arguments.back().variadic;
    if (parsed.positional.size() < required) {
        return std::unexpected(CliError{std::format(
            "missing argument for `{}`", path)});
    }
    if (!variadic && parsed.positional.size() > command.arguments.size()) {
        return std::unexpected(CliError{std::format(
            "surplus positional argument for `{}`: {}", path,
            parsed.positional[command.arguments.size()])});
    }
    return parsed;
}

}  // namespace detail_

std::expected<ParsedManualArgs, CliError> validate_manual_argv(
    const CommandSpec& command,
    std::span<const std::string_view> argv) {
    return detail_::validate_(command, argv, "xlings " + command.name);
}

nlohmann::json help_json(const CommandSpec& command) {
    nlohmann::json value;
    value["name"] = command.name;
    value["description"] = command.description;
    value["arguments"] = nlohmann::json::array();
    value["options"] = nlohmann::json::array();
    value["subcommands"] = nlohmann::json::array();
    for (const auto& argument : command.arguments) {
        value["arguments"].push_back({{"name", argument.name},
            {"description", argument.description}, {"required", argument.required},
            {"variadic", argument.variadic}});
    }
    for (const auto& option : command.options) {
        value["options"].push_back({{"syntax", option.syntax},
                                     {"description", option.description},
                                     {"global", option.global}});
    }
    for (const auto& child : command.children) {
        value["subcommands"].push_back(help_json(child));
    }
    return value;
}

nlohmann::json reference_json() { return help_json(root()); }

std::string agent_reference() {
    // The global options come first and on their own. They used to be spelled
    // out in every hand-written example (`xlings install <pkg> --yes --agent`);
    // a per-command listing that skips the root node drops them entirely, and
    // an agent reading only this section would never learn the two flags the
    // rules above tell it to always pass.
    std::string output = "GLOBAL OPTIONS — valid on every command\n";
    for (const auto* option : global_options()) {
        output += "  " + option->syntax + "\n    " + option->description + "\n";
    }
    output += "\n";

    const auto append = [&](this const auto& self, const CommandSpec& command,
                            std::string path) -> void {
        if (command.name != "xlings") {
            if (!path.empty()) path += " ";
            path += command.name;
            output += "  xlings " + path;
            for (const auto& argument : command.arguments) {
                output += argument.required ? " <" : " [";
                output += argument.name;
                output += argument.required ? ">" : "]";
                if (argument.variadic) output += "...";
            }
            output += "\n    " + command.description + "\n";
            if (!command.options.empty()) {
                output += "    options: ";
                for (std::size_t i = 0; i < command.options.size(); ++i) {
                    if (i != 0) output += "; ";
                    output += command.options[i].syntax;
                }
                output += "\n";
            }
        }
        for (const auto& child : command.children) self(child, path);
    };
    append(root(), {});
    return output;
}

}  // namespace xlings::cli::spec
