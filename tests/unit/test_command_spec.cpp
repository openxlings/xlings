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
    EXPECT_FALSE(validate_manual_argv(*subos,
        std::array<std::string_view, 3>{"remove", "probe", "extra"}));
}
