// The shared command-line model (modules/cli): a tree declared once, help
// shown by level, suggestions for a mistyped word.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.cli.model;

namespace spec = xlings::cli::spec;
using spec::Level;

namespace {

const spec::CommandSpec& tree() {
    static const spec::CommandSpec value{
        .name = "tool",
        .description = "a tool",
        .options = {{"--json", "machine-readable output", true},
                    {"--expert-flag", "only for experts", true, Level::Expert}},
        .children = {
            {.name = "new", .description = "make one", .arguments = {{"name", "its name", true}, {"kind", "which kind"}}},
            {.name = "enter", .description = "go in", .arguments = {{"name", "its name", true}}},
            {.name = "export", .description = "make an image", .arguments = {{"name", "", true}, {"file", "", true}},
             .level = Level::More},
            {.name = "install", .description = "install to a disk", .arguments = {{"device", "", true}},
             .level = Level::Expert},
        },
    };
    return value;
}

}  // namespace

XTEST(CliModel, HelpShowsTheCommonCommandsAndSaysHowToSeeTheRest,
      .area = "cli", .covers = {"LUBAN-CLI-LEVELS"}) {
    const auto common = spec::render_help(tree(), tree(), "tool", Level::Common);
    EXPECT_NE(common.find("new <name> [kind]"), std::string::npos) << common;
    EXPECT_NE(common.find("enter <name>"), std::string::npos) << common;
    EXPECT_EQ(common.find("export"), std::string::npos) << common;
    EXPECT_EQ(common.find("install"), std::string::npos) << common;
    EXPECT_EQ(common.find("--expert-flag"), std::string::npos) << common;
    EXPECT_NE(common.find("more: tool help --all"), std::string::npos) << common;

    const auto more = spec::render_help(tree(), tree(), "tool", Level::More);
    EXPECT_NE(more.find("export <name> <file>"), std::string::npos) << more;
    EXPECT_EQ(more.find("install"), std::string::npos) << more;
    EXPECT_NE(more.find("everything: tool help --all --expert"), std::string::npos) << more;

    const auto all = spec::render_help(tree(), tree(), "tool", Level::Expert);
    EXPECT_NE(all.find("install <device>"), std::string::npos) << all;
    EXPECT_EQ(all.find("help --all"), std::string::npos) << "nothing held back, nothing to point at\n" << all;
}

XTEST(CliModel, AMistypedCommandOrOptionGetsTheNearestNames,
      .area = "cli", .covers = {"LUBAN-CLI-SUGGEST"}) {
    EXPECT_EQ(spec::suggest(tree(), tree(), "entr"), std::vector<std::string>{"enter"});
    EXPECT_EQ(spec::suggest(tree(), tree(), "exprot").front(), "export");
    EXPECT_EQ(spec::suggest(tree(), tree(), "--jsn"), std::vector<std::string>{"--json"});
    EXPECT_TRUE(spec::suggest(tree(), tree(), "zzzzzz").empty());
}

XTEST(CliModel, ValidationWalksTheTreeItIsGiven,
      .area = "cli", .covers = {"LUBAN-CLI-LEVELS"}) {
    const std::array<std::string_view, 3> ok{"new", "box", "--json"};
    EXPECT_TRUE(spec::validate_in(tree(), tree(), ok, "tool"));
    const std::array<std::string_view, 1> missing{"enter"};
    auto r = spec::validate_in(tree(), tree(), missing, "tool");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().message, "missing argument for `tool enter`");
    const std::array<std::string_view, 1> unknown{"nope"};
    EXPECT_FALSE(spec::validate_in(tree(), tree(), unknown, "tool"));
}
