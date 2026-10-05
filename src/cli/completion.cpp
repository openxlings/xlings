// See completion.cppm for why this exists.
module xlings.cli.completion;

import std;
import xlings.cli.spec;

namespace xlings::cli::completion {

namespace {

const spec::CommandSpec* find_child_(const spec::CommandSpec& command,
                                     std::string_view token) {
    for (const auto& child : command.children) {
        if (child.name == token) return &child;
        if (std::ranges::find(child.aliases, token) != child.aliases.end()) {
            return &child;
        }
    }
    return nullptr;
}

std::string_view option_name_(std::string_view token) {
    return token.substr(0, token.find('='));
}

const spec::OptionSpec* find_option_(const spec::CommandSpec& command,
                                     std::string_view token) {
    const auto name = option_name_(token);
    for (const auto& option : command.options) {
        for (auto alias : spec::option_aliases(option)) {
            if (alias == name) return &option;
        }
    }
    // Global options are accepted everywhere; see spec::is_global_option.
    for (const auto* option : spec::global_options()) {
        for (auto alias : spec::option_aliases(*option)) {
            if (alias == name) return option;
        }
    }
    return nullptr;
}

void add_(std::vector<Candidate>& out, std::string value,
          std::string description) {
    if (value.empty()) return;
    const bool seen = std::ranges::any_of(out, [&](const Candidate& c) {
        return c.value == value;
    });
    if (!seen) out.push_back({std::move(value), std::move(description)});
}

void add_options_(std::vector<Candidate>& out, const spec::CommandSpec& command,
                  std::string_view prefix) {
    const auto add_option = [&](const spec::OptionSpec& option) {
        for (auto alias : spec::option_aliases(option)) {
            if (alias.starts_with(prefix)) {
                add_(out, std::string{alias}, option.description);
            }
        }
    };
    for (const auto& option : command.options) add_option(option);
    for (const auto* option : spec::global_options()) add_option(*option);
}

} // namespace

std::vector<Candidate> complete(std::span<const std::string> words,
                                const Provider& provider) {
    std::string prefix;
    std::vector<std::string_view> context;
    if (!words.empty()) {
        prefix = words.back();
        context.assign(words.begin(), words.end() - 1);
    }

    const spec::CommandSpec* current = &spec::root();
    std::string commandPath;
    std::size_t positional = 0;
    // A value-taking option in the last context slot with nothing after it:
    // the cursor is on its value, not on a command word.
    std::string pendingOption;
    // After `--` every word is an argument, never an option or a command.
    bool endOfOptions = false;

    for (std::size_t i = 0; i < context.size(); ++i) {
        const std::string_view token = context[i];
        if (endOfOptions) {
            ++positional;
            continue;
        }
        if (token == "--") {
            endOfOptions = true;
            continue;
        }
        if (token.starts_with('-')) {
            const auto* option = find_option_(*current, token);
            // `--opt=value` carries its value; the next word is its own.
            if (option && !token.contains('=')) {
                const bool needs = option->syntax.contains('<');
                const bool optional = option->syntax.contains('[');
                if (needs || optional) {
                    const bool hasValue = i + 1 < context.size()
                        && !context[i + 1].starts_with('-');
                    if (hasValue) {
                        ++i;  // consume the value
                    } else if (i + 1 == context.size()) {
                        pendingOption = std::string{option_name_(token)};
                    }
                }
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
        for (auto& c : provider({commandPath, "", pendingOption})) {
            if (c.value.starts_with(prefix)) {
                add_(out, std::move(c.value), std::move(c.description));
            }
        }
        return out;
    }

    // The cursor is on `--opt=<value>`: complete the value, and answer with
    // the whole token so a shell that replaces the full word gets it intact.
    if (!endOfOptions && prefix.starts_with('-') && prefix.contains('=')) {
        const auto eq = prefix.find('=');
        const std::string name = prefix.substr(0, eq);
        const std::string valuePrefix = prefix.substr(eq + 1);
        const auto* option = find_option_(*current, name);
        if (option && (option->syntax.contains('<') || option->syntax.contains('['))) {
            for (auto& c : provider({commandPath, "", name})) {
                if (c.value.starts_with(valuePrefix)) {
                    add_(out, name + "=" + c.value, std::move(c.description));
                }
            }
        }
        return out;
    }

    // The cursor is on an option name. Never mix in command names: a token
    // starting with '-' is a flag until it is closed.
    if (!endOfOptions && prefix.starts_with('-')) {
        add_options_(out, *current, prefix);
        return out;
    }

    // Subcommands only where a command word is still expected. Once a
    // positional has been consumed the parser would reject a subcommand, so
    // offering one would be a lie.
    if (positional == 0 && !endOfOptions) {
        for (const auto& child : current->children) {
            if (child.name.starts_with(prefix)) {
                add_(out, child.name, child.description);
            }
        }
    }

    if (!endOfOptions) add_options_(out, *current, prefix);

    // Positional argument values. The last argument may be variadic, so keep
    // asking the provider for it after the declared slots are used up.
    const auto* argument = static_cast<const spec::ArgSpec*>(nullptr);
    if (positional < current->arguments.size()) {
        argument = &current->arguments[positional];
    } else if (!current->arguments.empty()
               && current->arguments.back().variadic) {
        argument = &current->arguments.back();
    }
    if (argument) {
        for (auto& c : provider({commandPath, argument->name, ""})) {
            if (c.value.starts_with(prefix)) {
                add_(out, std::move(c.value), std::move(c.description));
            }
        }
    }

    return out;
}

} // namespace xlings::cli::completion
