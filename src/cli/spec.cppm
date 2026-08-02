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
            {"-y, --yes", "Skip confirmation prompts"},
            {"--agent", "Use stable plain-text output"},
            {"-v, --verbose", "Enable verbose output"},
            {"-q, --quiet", "Suppress non-essential output"},
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
                {"new", "Create a SubOS", {}, {{"name", "SubOS name", true}}, {{"--storage <MODE>", "shared, tmpfs or image"}, {"--image-size <SIZE>", "Image size"}, {"--from <SOURCE>", "Fork source"}}, {}},
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

std::expected<ParsedManualArgs, CliError> validate_manual_argv(
    const CommandSpec& command,
    std::span<const std::string_view> argv) {
    if (!command.children.empty() && !argv.empty()
        && !argv.front().starts_with('-')) {
        if (const auto* child = find(std::array<std::string_view, 2>{
                command.name, argv.front()})) {
            return validate_manual_argv(*child, argv.subspan(1));
        }
        return std::unexpected(CliError{std::format(
            "unknown subcommand for `xlings {}`: {}", command.name,
            argv.front())});
    }

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
            for (const auto piece : std::views::split(option.syntax, ',')) {
                auto alias = std::string_view{piece.begin(), piece.end()};
                while (!alias.empty() && alias.front() == ' ') alias.remove_prefix(1);
                const auto value = alias.find_first_of(" <[");
                alias = alias.substr(0, value);
                if (alias == optionName) {
                    matched = &option;
                    break;
                }
            }
            if (matched) break;
        }
        if (!matched) {
            return std::unexpected(CliError{std::format(
                "unknown option for `xlings {}`: {}", command.name, token)});
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
            const auto value = argv[i + 1];
            const bool recognizedOptionalValue =
                (optionName == "--sandbox" && (value == "bwrap" || value == "proot"))
                || (optionName == "--shell" && (value == "sh" || value == "bash"
                    || value == "zsh" || value == "fish" || value == "pwsh"));
            if (recognizedOptionalValue) ++i;
        }
    }

    const auto required = std::ranges::count_if(command.arguments,
        [](const auto& argument) { return argument.required; });
    const bool variadic = !command.arguments.empty()
        && command.arguments.back().variadic;
    if (parsed.positional.size() < static_cast<std::size_t>(required)) {
        return std::unexpected(CliError{std::format(
            "missing argument for `xlings {}`", command.name)});
    }
    if (!variadic && parsed.positional.size() > command.arguments.size()) {
        return std::unexpected(CliError{std::format(
            "surplus positional argument for `xlings {}`: {}", command.name,
            parsed.positional[command.arguments.size()])});
    }
    return parsed;
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
                                     {"description", option.description}});
    }
    for (const auto& child : command.children) {
        value["subcommands"].push_back(help_json(child));
    }
    return value;
}

nlohmann::json reference_json() { return help_json(root()); }

std::string agent_reference() {
    std::string output;
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
