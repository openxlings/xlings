// A confirmation nobody can answer must fail, not guess.
//
// THE BUG THIS LOCKS DOWN
//
// `--agent` used to register an auto-responder that replied with each
// prompt's `defaultValue`. The two confirmations in this codebase default in
// OPPOSITE directions -- `confirm_install` = "y", `confirm_remove` = "n" -- so
// the same "helpful" fallback meant:
//
//     xlings install foo --agent   → installs without asking
//     xlings remove  foo --agent   → prints "cancelled", exits 0, removes nothing
//
// An agent reading the exit code was told the removal succeeded. That is the
// project's recurring shape: didn't-happen and succeeded produce identical
// output.
//
// The documented contract was already "pass --yes for non-interactive use"
// (the agent skill says ALWAYS). Refusing to guess is what turns that from
// advice into something the tool enforces.
#include <gtest/gtest.h>

import std;
import xlings.runtime;

using xlings::EventStream;
using xlings::PromptEvent;

namespace {

PromptEvent confirm_(std::string id, std::string def) {
    PromptEvent p;
    p.id = std::move(id);
    p.question = "Proceed?";
    p.options = {"y", "n"};
    p.defaultValue = std::move(def);
    return p;
}

}  // namespace

TEST(PromptRefusal, NonInteractiveReturnsCannotAskNotTheDefault) {
    EventStream stream;
    stream.set_interactive(false);

    // Both directions, because the old bug was invisible in one of them: an
    // install that auto-answers "y" looks like it works.
    EXPECT_EQ(stream.prompt(confirm_("confirm_install", "y")),
              EventStream::kCannotAsk);
    EXPECT_EQ(stream.prompt(confirm_("confirm_remove", "n")),
              EventStream::kCannotAsk);
}

TEST(PromptRefusal, CannotAskIsDistinctFromCancelled) {
    // "" already means cancelled/timed out -- the user's answer. The absence
    // of anyone to ask has to be a different value or callers cannot react
    // differently, which is precisely how "cancelled" ended up standing in for
    // "nobody was there".
    EXPECT_NE(EventStream::kCannotAsk, "");
}

TEST(PromptRefusal, NonInteractiveDoesNotEmitAQuestionNobodyCanAnswer) {
    EventStream stream;
    int prompts = 0;
    stream.on_event([&](const xlings::Event& e) {
        if (std::get_if<PromptEvent>(&e)) ++prompts;
    });
    stream.set_interactive(false);

    (void)stream.prompt(confirm_("confirm_remove", "n"));
    // Emitting it would either deadlock (nothing responds) or train consumers
    // to answer on the user's behalf, which is the bug.
    EXPECT_EQ(prompts, 0);
}

TEST(PromptRefusal, RegisteredResponderOutranksNonInteractivity) {
    EventStream stream;
    stream.set_interactive(false);
    // An explicit auto-responder IS somebody answering -- just not a human.
    // `xlings interface` relies on this to drive confirmations over stdin.
    stream.register_auto_responder("confirm_",
        [](const PromptEvent&) { return std::string("y"); });

    EXPECT_EQ(stream.prompt(confirm_("confirm_remove", "n")), "y");
}

TEST(PromptRefusal, InteractiveStreamsStillPromptNormally) {
    EventStream stream;
    // Default is interactive: a terminal session must be unaffected.
    EXPECT_TRUE(stream.interactive());

    std::string asked;
    stream.on_event([&](const xlings::Event& e) {
        if (auto* p = std::get_if<PromptEvent>(&e)) {
            asked = p->id;
            stream.respond(p->id, "y");
        }
    });
    EXPECT_EQ(stream.prompt(confirm_("confirm_install", "y")), "y");
    EXPECT_EQ(asked, "confirm_install");
}
