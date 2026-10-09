module xlings.cli.model;

import std;
import xlings.libs.json;

namespace xlings::cli::spec {

namespace {

const CommandSpec* child_of_(const CommandSpec& command, std::string_view token) {
    for (const auto& child : command.children)
        if (child.name == token || std::ranges::find(child.aliases, token) != child.aliases.end())
            return &child;
    return nullptr;
}

std::size_t free_tokens_(std::span<const std::string_view> argv, std::size_t from) {
    std::size_t count = 0;
    for (std::size_t i = from; i < argv.size(); ++i)
        if (!argv[i].starts_with('-')) ++count;
    return count;
}

std::size_t distance_(std::string_view a, std::string_view b) {
    std::vector<std::size_t> row(b.size() + 1);
    std::iota(row.begin(), row.end(), std::size_t{0});
    for (std::size_t i = 1; i <= a.size(); ++i) {
        std::size_t diagonal = row[0];
        row[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const auto above = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0u : 1u)});
            diagonal = above;
        }
    }
    return row[b.size()];
}

std::string usage_of_(const CommandSpec& command) {
    std::string out;
    for (const auto& a : command.arguments) {
        out += a.required ? " <" : " [";
        out += a.name;
        out += a.required ? ">" : "]";
        if (a.variadic) out += "...";
    }
    if (!command.children.empty()) out += " <command>";
    return out;
}

}  // namespace

const CommandSpec* find_in(const CommandSpec& root, std::span<const std::string_view> path) {
    const CommandSpec* current = &root;
    for (const auto part : path) {
        current = child_of_(*current, part);
        if (!current) return nullptr;
    }
    return current;
}

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

std::vector<const OptionSpec*> global_options_of(const CommandSpec& root) {
    std::vector<const OptionSpec*> options;
    for (const auto& option : root.options)
        if (option.global) options.push_back(&option);
    return options;
}

std::expected<ParsedManualArgs, CliError> validate_in(const CommandSpec& root, const CommandSpec& command,
                                                      std::span<const std::string_view> argv,
                                                      const std::string& path) {
    if (!command.children.empty() && !argv.empty() && !argv.front().starts_with('-')) {
        if (const auto* child = child_of_(command, argv.front()))
            return validate_in(root, *child, argv.subspan(1), path + " " + std::string(argv.front()));
        return std::unexpected(CliError{std::format("unknown subcommand for `{}`: {}", path, argv.front())});
    }
    const auto globals = global_options_of(root);
    const auto required = static_cast<std::size_t>(
        std::ranges::count_if(command.arguments, [](const auto& a) { return a.required; }));
    ParsedManualArgs parsed;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const auto token = argv[i];
        // `--` ends the options: what follows is a command line of its own
        // (`subos exec <s> -- sh -c ...`), taken verbatim.
        if (token == "--") {
            for (++i; i < argv.size(); ++i) parsed.positional.emplace_back(argv[i]);
            break;
        }
        if (!token.starts_with('-')) {
            parsed.positional.emplace_back(token);
            continue;
        }
        const auto equals = token.find('=');
        const auto optionName = token.substr(0, equals);
        const OptionSpec* matched = nullptr;
        for (const auto& option : command.options)
            for (const auto alias : option_aliases(option))
                if (!matched && alias == optionName) matched = &option;
        for (const auto* option : globals)
            for (const auto alias : option_aliases(*option))
                if (!matched && alias == optionName) matched = option;
        if (!matched)
            return std::unexpected(CliError{std::format("unknown option for `{}`: {}", path, token)});
        parsed.options.insert(std::string(optionName));
        const bool requiresValue = matched->syntax.contains('<');
        const bool optionalValue = matched->syntax.contains('[');
        if (equals != std::string_view::npos) {
            if (token.substr(equals + 1).empty())
                return std::unexpected(CliError{std::format("missing value for option: {}", optionName)});
            continue;
        }
        if (requiresValue) {
            if (i + 1 >= argv.size() || argv[i + 1].starts_with('-'))
                return std::unexpected(CliError{std::format("missing value for option: {}", optionName)});
            ++i;
            continue;
        }
        // An optional value takes the next word unless a required positional
        // still needs it -- never a whitelist of known values, which every new
        // value the parser learns would silently fall out of.
        if (optionalValue && i + 1 < argv.size() && !argv[i + 1].starts_with('-')
            && parsed.positional.size() + free_tokens_(argv, i + 2) >= required)
            ++i;
    }
    const bool variadic = !command.arguments.empty() && command.arguments.back().variadic;
    if (parsed.positional.size() < required)
        return std::unexpected(CliError{std::format("missing argument for `{}`", path)});
    if (!variadic && parsed.positional.size() > command.arguments.size())
        return std::unexpected(CliError{std::format("surplus positional argument for `{}`: {}", path,
                                                    parsed.positional[command.arguments.size()])});
    return parsed;
}

nlohmann::json help_json(const CommandSpec& command) {
    nlohmann::json value;
    value["name"] = command.name;
    value["description"] = command.description;
    value["arguments"] = nlohmann::json::array();
    value["options"] = nlohmann::json::array();
    value["subcommands"] = nlohmann::json::array();
    for (const auto& a : command.arguments)
        value["arguments"].push_back({{"name", a.name}, {"description", a.description},
                                      {"required", a.required}, {"variadic", a.variadic}});
    for (const auto& o : command.options)
        value["options"].push_back({{"syntax", o.syntax}, {"description", o.description}, {"global", o.global}});
    for (const auto& child : command.children) value["subcommands"].push_back(help_json(child));
    return value;
}

std::string render_help(const CommandSpec& root, const CommandSpec& command, std::string_view program,
                        Level upto) {
    std::string out = std::format("{}{}\n", program, usage_of_(command));
    if (!command.description.empty()) out += "  " + command.description + "\n";
    bool held = false;
    const auto shown = [&](Level l) { if (l > upto) { held = true; return false; } return true; };

    std::vector<const CommandSpec*> children;
    for (const auto& c : command.children) if (shown(c.level)) children.push_back(&c);
    if (!children.empty()) {
        std::size_t width = 0;
        for (const auto* c : children) width = std::max(width, c->name.size() + usage_of_(*c).size());
        out += "\n";
        for (const auto level : {Level::Common, Level::More, Level::Expert}) {
            bool header = false;
            for (const auto* c : children) {
                if (c->level != level) continue;
                if (!header && upto != Level::Common) {
                    out += level == Level::Common ? "common\n" : level == Level::More ? "more\n" : "expert\n";
                    header = true;
                }
                const auto left = c->name + usage_of_(*c);
                out += std::format("  {:<{}}  {}\n", left, width, c->description);
            }
        }
    }
    std::vector<const OptionSpec*> options;
    for (const auto& o : command.options) if (shown(o.level)) options.push_back(&o);
    if (&command == &root)
        for (const auto* o : global_options_of(root)) if (shown(o->level) && std::ranges::find(options, o) == options.end()) options.push_back(o);
    if (!options.empty()) {
        std::size_t width = 0;
        for (const auto* o : options) width = std::max(width, o->syntax.size());
        out += "\noptions\n";
        for (const auto* o : options) out += std::format("  {:<{}}  {}\n", o->syntax, width, o->description);
    }
    if (held) {
        // The way to the rest, with the program's own first word.
        const auto first = program.substr(0, program.find(' '));
        out += upto == Level::Common
            ? std::format("\nmore: {} help --all\n", first)
            : std::format("\neverything: {} help --all --expert\n", first);
    }
    return out;
}

std::vector<std::string> suggest(const CommandSpec& root, const CommandSpec& command, std::string_view token) {
    std::vector<std::pair<std::size_t, std::string>> scored;
    const auto consider = [&](std::string_view name) {
        const auto d = distance_(token, name);
        // Close: at most a third of the word wrong, and never more than 2.
        if (d <= std::min<std::size_t>(2, std::max<std::size_t>(1, name.size() / 3))
            || (token.size() >= 3 && name.starts_with(token)))
            scored.emplace_back(d, std::string(name));
    };
    if (token.starts_with('-')) {
        for (const auto& o : command.options) for (auto a : option_aliases(o)) consider(a);
        for (const auto* o : global_options_of(root)) for (auto a : option_aliases(*o)) consider(a);
    } else {
        for (const auto& c : command.children) {
            consider(c.name);
            for (const auto& a : c.aliases) consider(a);
        }
    }
    std::ranges::sort(scored);
    std::vector<std::string> out;
    for (auto& [d, name] : scored)
        if (std::ranges::find(out, name) == out.end()) out.push_back(std::move(name));
    return out;
}

}  // namespace xlings::cli::spec

namespace xlings::cli::completion {

namespace {

using spec::CommandSpec;
using spec::OptionSpec;

const CommandSpec* find_child_(const CommandSpec& command, std::string_view token) {
    for (const auto& child : command.children)
        if (child.name == token || std::ranges::find(child.aliases, token) != child.aliases.end()) return &child;
    return nullptr;
}

std::string_view option_name_(std::string_view token) { return token.substr(0, token.find('=')); }

const OptionSpec* find_option_(const CommandSpec& root, const CommandSpec& command, std::string_view token) {
    const auto name = option_name_(token);
    for (const auto& option : command.options)
        for (auto alias : spec::option_aliases(option))
            if (alias == name) return &option;
    for (const auto* option : spec::global_options_of(root))
        for (auto alias : spec::option_aliases(*option))
            if (alias == name) return option;
    return nullptr;
}

void add_(std::vector<Candidate>& out, std::string value, std::string description) {
    if (value.empty()) return;
    if (std::ranges::none_of(out, [&](const Candidate& c) { return c.value == value; }))
        out.push_back({std::move(value), std::move(description)});
}

void add_options_(std::vector<Candidate>& out, const CommandSpec& root, const CommandSpec& command,
                  std::string_view prefix) {
    const auto add_option = [&](const OptionSpec& option) {
        for (auto alias : spec::option_aliases(option))
            if (alias.starts_with(prefix)) add_(out, std::string{alias}, option.description);
    };
    for (const auto& option : command.options) add_option(option);
    for (const auto* option : spec::global_options_of(root)) add_option(*option);
}

}  // namespace

std::vector<Candidate> complete_in(const spec::CommandSpec& root, std::span<const std::string> words,
                                   const Provider& provider) {
    std::string prefix;
    std::vector<std::string_view> context;
    if (!words.empty()) {
        prefix = words.back();
        context.assign(words.begin(), words.end() - 1);
    }
    const CommandSpec* current = &root;
    std::string commandPath;
    std::size_t positional = 0;
    // A value-taking option in the last context slot with nothing after it:
    // the cursor is on its value, not on a command word.
    std::string pendingOption;
    // After `--` every word is an argument, never an option or a command.
    bool endOfOptions = false;
    for (std::size_t i = 0; i < context.size(); ++i) {
        const std::string_view token = context[i];
        if (endOfOptions) { ++positional; continue; }
        if (token == "--") { endOfOptions = true; continue; }
        if (token.starts_with('-')) {
            const auto* option = find_option_(root, *current, token);
            // `--opt=value` carries its value; the next word is its own.
            if (option && !token.contains('=')
                && (option->syntax.contains('<') || option->syntax.contains('['))) {
                const bool hasValue = i + 1 < context.size() && !context[i + 1].starts_with('-');
                if (hasValue) ++i;
                else if (i + 1 == context.size()) pendingOption = std::string{option_name_(token)};
            }
            continue;
        }
        if (const auto* child = find_child_(*current, token)) {
            current = child;
            if (!commandPath.empty()) commandPath += ' ';
            commandPath += child->name;
            positional = 0;
            continue;
        }
        ++positional;
    }

    std::vector<Candidate> out;
    // The cursor is on an option's value.
    if (!pendingOption.empty()) {
        for (auto& c : provider({commandPath, "", pendingOption}))
            if (c.value.starts_with(prefix)) add_(out, std::move(c.value), std::move(c.description));
        return out;
    }
    // `--opt=<value>`: complete the value, and answer with the whole token so
    // a shell that replaces the full word gets it intact.
    if (!endOfOptions && prefix.starts_with('-') && prefix.contains('=')) {
        const auto eq = prefix.find('=');
        const std::string name = prefix.substr(0, eq);
        const std::string valuePrefix = prefix.substr(eq + 1);
        const auto* option = find_option_(root, *current, name);
        if (option && (option->syntax.contains('<') || option->syntax.contains('[')))
            for (auto& c : provider({commandPath, "", name}))
                if (c.value.starts_with(valuePrefix)) add_(out, name + "=" + c.value, std::move(c.description));
        return out;
    }
    // An option name. Never mix in command names: a token starting with '-'
    // is a flag until it is closed.
    if (!endOfOptions && prefix.starts_with('-')) {
        add_options_(out, root, *current, prefix);
        return out;
    }
    // Subcommands only where a command word is still expected.
    if (positional == 0 && !endOfOptions)
        for (const auto& child : current->children)
            if (child.name.starts_with(prefix)) add_(out, child.name, child.description);
    if (!endOfOptions) add_options_(out, root, *current, prefix);
    // Positional values. The last argument may be variadic, so keep asking
    // for it after the declared slots are used up.
    const spec::ArgSpec* argument = nullptr;
    if (positional < current->arguments.size()) argument = &current->arguments[positional];
    else if (!current->arguments.empty() && current->arguments.back().variadic) argument = &current->arguments.back();
    if (argument)
        for (auto& c : provider({commandPath, argument->name, ""}))
            if (c.value.starts_with(prefix)) add_(out, std::move(c.value), std::move(c.description));
    return out;
}

}  // namespace xlings::cli::completion
