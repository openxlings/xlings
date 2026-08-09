#include <gtest/gtest.h>

import std;
import xlings.cli.spec;

using namespace xlings::cli::spec;

TEST(CommandSpec, ResolvesNestedCommandsAndAliases) {
    EXPECT_NE(find(std::array<std::string_view, 2>{"subos", "use"}), nullptr);
    const auto* alias = find(std::array<std::string_view, 2>{"subos", "ls"});
    ASSERT_NE(alias, nullptr);
    EXPECT_EQ(alias->name, "list");
}

TEST(CommandSpec, PublishesImplementedFlags) {
    const auto* info = find(std::array<std::string_view, 1>{"info"});
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(std::ranges::any_of(info->options, [](const auto& option) {
        return option.syntax.contains("--all-versions");
    }));
    EXPECT_TRUE(reference_json().contains("subcommands"));
}

TEST(CommandSpec, ValidatesManualNestedArguments) {
    const auto* self = find(std::array<std::string_view, 1>{"self"});
    ASSERT_NE(self, nullptr);
    EXPECT_TRUE(validate_manual_argv(*self,
        std::array<std::string_view, 2>{"doctor", "--fix"}));
    auto bad = validate_manual_argv(*self,
        std::array<std::string_view, 2>{"doctor", "--bogus"});
    ASSERT_FALSE(bad);
    EXPECT_TRUE(bad.error().message.contains("unknown option"));

    const auto* subos = find(std::array<std::string_view, 1>{"subos"});
    ASSERT_NE(subos, nullptr);
    EXPECT_TRUE(validate_manual_argv(*subos,
        std::array<std::string_view, 5>{
            "use", "probe", "--sandbox", "bwrap", "--gpu"}));
    EXPECT_TRUE(validate_manual_argv(*subos,
        std::array<std::string_view, 1>{"use"}));
    EXPECT_FALSE(validate_manual_argv(*subos,
        std::array<std::string_view, 3>{"remove", "probe", "extra"}));
}

// A diagnostic that names only the leaf sends the user to a different command:
// `xlings subos use` reported as `xlings use` is a real, unrelated command,
// and `xlings self doctor` reported as `xlings doctor` does not exist at all.
TEST(CommandSpec, DiagnosticsNameTheInvokedPath) {
    const auto* subos = find(std::array<std::string_view, 1>{"subos"});
    ASSERT_NE(subos, nullptr);
    auto surplus = validate_manual_argv(*subos,
        std::array<std::string_view, 3>{"remove", "probe", "extra"});
    ASSERT_FALSE(surplus);
    EXPECT_TRUE(surplus.error().message.contains("xlings subos remove"))
        << surplus.error().message;

    const auto* self = find(std::array<std::string_view, 1>{"self"});
    ASSERT_NE(self, nullptr);
    auto unknown = validate_manual_argv(*self,
        std::array<std::string_view, 2>{"doctor", "--bogus"});
    ASSERT_FALSE(unknown);
    EXPECT_TRUE(unknown.error().message.contains("xlings self doctor"))
        << unknown.error().message;
}

// The parser accepts `powershell`, `ps1` and `ps` as shell kinds. A validator
// that recognises only its own five-name whitelist turns a working spelling
// into "surplus positional argument" -- and every future shell kind, sandbox
// backend or storage mode repeats it.
TEST(CommandSpec, OptionalValuesAreNotWhitelisted) {
    const auto* subos = find(std::array<std::string_view, 1>{"subos"});
    ASSERT_NE(subos, nullptr);
    for (const auto* kind : {"sh", "bash", "zsh", "fish", "pwsh",
                             "powershell", "ps1", "ps", "nu"}) {
        std::array<std::string_view, 4> argv{"use", "probe", "--shell", kind};
        EXPECT_TRUE(validate_manual_argv(*subos, argv))
            << "rejected --shell " << kind;
    }
    // With the SubOS name optional, `--shell probe` is the option's optional
    // value and leaves discovery mode name-less. A named environment remains
    // unambiguous when it precedes the option.
    auto ambiguous = validate_manual_argv(*subos,
        std::array<std::string_view, 3>{"use", "--shell", "probe"});
    ASSERT_TRUE(ambiguous);
    EXPECT_TRUE(ambiguous->positional.empty());
    auto named = validate_manual_argv(*subos,
        std::array<std::string_view, 3>{"use", "probe", "--shell"});
    ASSERT_TRUE(named);
    EXPECT_EQ(named->positional, (std::vector<std::string>{"probe"}));
}

// `xlings --help` documents `-y/--yes`, `--agent`, `-v` and `-q` without
// qualification. Refusing them on a subcommand that cannot act on them makes
// the documentation a trap -- and the agent skill's first rule is to always
// pass `--yes`.
TEST(CommandSpec, GlobalOptionsAreAcceptedEverywhere) {
    EXPECT_TRUE(is_global_option("--yes"));
    EXPECT_TRUE(is_global_option("-y"));
    EXPECT_TRUE(is_global_option("--agent"));
    EXPECT_TRUE(is_global_option("-q"));
    // Not modifiers: they are commands, and accepting them mid-argv would let
    // `xlings subos new foo --version` silently create a subos.
    EXPECT_FALSE(is_global_option("--version"));
    EXPECT_FALSE(is_global_option("-h"));
    EXPECT_FALSE(is_global_option("--fix"));

    for (const auto* family : {"subos", "self", "profile"}) {
        std::array<std::string_view, 1> path{family};
        const auto* command = find(path);
        ASSERT_NE(command, nullptr) << family;
        for (const auto& child : command->children) {
            std::vector<std::string_view> argv{child.name};
            for (const auto& argument : child.arguments) {
                if (argument.required) argv.push_back("probe");
            }
            argv.push_back("--yes");
            EXPECT_TRUE(validate_manual_argv(*command, argv))
                << family << " " << child.name << " rejected --yes";
        }
    }
}

// The agent reference replaced hand-written examples that spelled out
// `--yes --agent` on every line. A per-command walk that skips the root node
// drops both flags entirely, leaving the RULES section referring to options
// the reference below it never shows.
TEST(CommandSpec, AgentReferenceKeepsGlobalOptions) {
    const auto reference = agent_reference();
    EXPECT_TRUE(reference.contains("--yes"));
    EXPECT_TRUE(reference.contains("--agent"));
    EXPECT_TRUE(reference.contains("xlings install"));
}
