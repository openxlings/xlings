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

// The three outcomes, as predicates. Spelled once so the tests below read as
// statements about behaviour rather than about std::variant.
bool nobody_to_ask(const EventStream::Outcome& o) {
    return std::holds_alternative<EventStream::NobodyToAsk>(o);
}
bool cancelled(const EventStream::Outcome& o) {
    return std::holds_alternative<EventStream::Cancelled>(o);
}
std::string chosen_or_empty(const EventStream::Outcome& o) {
    if (auto* c = std::get_if<EventStream::Chosen>(&o)) return c->value;
    return {};
}

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
    EXPECT_TRUE(nobody_to_ask(stream.prompt(confirm_("confirm_install", "y"))));
    EXPECT_TRUE(nobody_to_ask(stream.prompt(confirm_("confirm_remove", "n"))));
}

TEST(PromptRefusal, CannotAskIsDistinctFromCancelled) {
    // A cancel is the user's answer; nobody-to-ask is the absence of one, and
    // callers must react differently. These were both encoded in one string
    // ("" vs a sentinel), which is precisely how "cancelled" ended up standing
    // in for "nobody was there" in `select_package`.
    //
    // Now they are distinct types, so the confusion is not expressible -- and
    // `std::visit` over an incomplete overload set does not compile, which is
    // the guarantee the sentinel could not give.
    EventStream interactive;
    std::string asked;
    interactive.on_event([&](const xlings::Event& e) {
        if (auto* p = std::get_if<PromptEvent>(&e)) interactive.respond(p->id, "");
    });
    EXPECT_TRUE(cancelled(interactive.prompt(confirm_("confirm_remove", "n"))));

    EventStream silent;
    silent.set_interactive(false);
    EXPECT_TRUE(nobody_to_ask(silent.prompt(confirm_("confirm_remove", "n"))));
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

    EXPECT_EQ(chosen_or_empty(stream.prompt(confirm_("confirm_remove", "n"))), "y");
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
    EXPECT_EQ(chosen_or_empty(stream.prompt(confirm_("confirm_install", "y"))), "y");
    EXPECT_EQ(asked, "confirm_install");
}
