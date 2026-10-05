#include <gtest/gtest.h>

import std;
import xlings.cli.completion;

using namespace xlings::cli::completion;

namespace {

bool has(const std::vector<Candidate>& candidates, std::string_view value) {
    return std::ranges::any_of(candidates, [&](const Candidate& candidate) {
        return candidate.value == value;
    });
}

Provider no_values = [](const Request&) { return std::vector<Candidate>{}; };

std::vector<Candidate> complete_words(std::vector<std::string> words,
                                      const Provider& provider = no_values) {
    return complete(std::span<const std::string>(words), provider);
}

} // namespace

TEST(Completion, OffersRootCommandsAndGlobalOptions) {
    auto candidates = complete_words({""});
    EXPECT_TRUE(has(candidates, "install"));
    EXPECT_TRUE(has(candidates, "subos"));
    EXPECT_TRUE(has(candidates, "self"));
    EXPECT_TRUE(has(candidates, "--agent"));
}

TEST(Completion, FiltersByPrefix) {
    auto candidates = complete_words({"su"});
    EXPECT_TRUE(has(candidates, "subos"));
    EXPECT_FALSE(has(candidates, "install"));
}

TEST(Completion, ResolvesNestedSubcommands) {
    auto candidates = complete_words({"subos", ""});
    EXPECT_TRUE(has(candidates, "use"));
    EXPECT_TRUE(has(candidates, "new"));
    EXPECT_TRUE(has(candidates, "remove"));
    EXPECT_TRUE(has(candidates, "--yes"));  // global, valid on every command
}

TEST(Completion, OffersOnlyOptionsOnceDashTyped) {
    auto candidates = complete_words({"subos", "use", "--"});
    EXPECT_TRUE(has(candidates, "--global"));
    EXPECT_TRUE(has(candidates, "--sandbox"));
    EXPECT_FALSE(has(candidates, "new"));
}

// The cursor on a positional asks the provider, and the request names the
// command path and the argument so the provider can decide what to answer.
TEST(Completion, AsksProviderForPositionalWithContext) {
    Request seen;
    Provider provider = [&](const Request& request) {
        seen = request;
        return std::vector<Candidate>{{"probe", "subos"}};
    };
    auto candidates = complete_words({"subos", "use", ""}, provider);
    EXPECT_EQ(seen.command, "subos use");
    EXPECT_EQ(seen.argument, "name");
    EXPECT_TRUE(seen.option.empty());
    EXPECT_TRUE(has(candidates, "probe"));
}

// A value-taking option consumes the following token, so it must not be
// mistaken for a positional.
TEST(Completion, OptionValueConsumesItsToken) {
    Request seen;
    Provider provider = [&](const Request& request) {
        seen = request;
        return std::vector<Candidate>{};
    };
    complete_words({"subos", "use", "--shell", "bash", ""}, provider);
    EXPECT_EQ(seen.argument, "name");
}

// Completing an option's value routes to the provider by option name.
TEST(Completion, AsksProviderForOptionValue) {
    Request seen;
    Provider provider = [&](const Request& request) {
        seen = request;
        return std::vector<Candidate>{{"en", ""}, {"zh", ""}};
    };
    auto candidates = complete_words({"config", "--lang", "e"}, provider);
    EXPECT_EQ(seen.option, "--lang");
    EXPECT_TRUE(seen.argument.empty());
    EXPECT_TRUE(has(candidates, "en"));
    EXPECT_FALSE(has(candidates, "zh"));  // filtered by the "e" prefix
}

// The last argument is variadic for `install`, so the provider keeps being
// asked after the declared slots are used up.
TEST(Completion, RepeatsVariadicArgument) {
    Request seen;
    Provider provider = [&](const Request& request) {
        seen = request;
        return std::vector<Candidate>{{"node", ""}};
    };
    complete_words({"install", "gcc", ""}, provider);
    EXPECT_EQ(seen.argument, "packages");
}

// `--opt=value` carries its own value: the word after it is the next
// positional, not the option's value.
TEST(Completion, InlineOptionValueDoesNotConsumeTheNextWord) {
    Request seen;
    Provider provider = [&](const Request& request) {
        seen = request;
        return std::vector<Candidate>{};
    };
    complete_words({"subos", "use", "--shell=bash", ""}, provider);
    EXPECT_EQ(seen.argument, "name");
    EXPECT_TRUE(seen.option.empty());
}

// The cursor inside `--opt=` completes the value and answers with the whole
// word, which is what a shell replacing the full token needs.
TEST(Completion, CompletesTheValueOfAnInlineOption) {
    Provider provider = [](const Request& request) {
        if (request.option == "--lang") {
            return std::vector<Candidate>{{"en", ""}, {"zh", ""}};
        }
        return std::vector<Candidate>{};
    };
    auto candidates = complete_words({"config", "--lang=e"}, provider);
    EXPECT_TRUE(has(candidates, "--lang=en"));
    EXPECT_FALSE(has(candidates, "--lang=zh"));
}

// After `--` nothing is an option or a subcommand any more.
TEST(Completion, OffersNoOptionsAfterTheTerminator) {
    auto candidates = complete_words({"subos", "--", "-"});
    EXPECT_TRUE(candidates.empty());
    candidates = complete_words({"subos", "--", ""});
    EXPECT_FALSE(has(candidates, "use"));
    EXPECT_FALSE(has(candidates, "--yes"));
}
