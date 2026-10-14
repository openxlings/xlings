// openas: actors and their delegation, effect classes, and the conformance
// of a description of verbs -- with a description that breaks each clause,
// so a check that holds nothing cannot pass. luban's own verbs conform, and
// their JSON arguments become the command line they stand for.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import openas;
import openas.conformance;
import luban.cli;

using Json = nlohmann::json;

XTEST(OpenAs, AnActorTravelsAsTextAndDelegationOnlyAddsToItsChain, .area = "luban", .covers = {"CONTROL-PRINCIPAL"}) {
    const auto alice = openas::parse("person:alice");
    ASSERT_TRUE(alice);
    EXPECT_EQ(alice->kind, openas::Kind::Person);
    const auto planner = openas::delegate(*alice, openas::Kind::Agent, "planner");
    EXPECT_EQ(openas::format(planner), "agent:planner;via=person:alice");
    const auto reader = openas::delegate(planner, openas::Kind::Agent, "reader");
    EXPECT_EQ(openas::format(reader), "agent:reader;via=person:alice,agent:planner");
    EXPECT_EQ(openas::parse(openas::format(reader)), reader);
    EXPECT_TRUE(openas::acts_for(reader, *alice));
    EXPECT_TRUE(openas::acts_for(reader, planner));
    EXPECT_FALSE(openas::acts_for(planner, reader));
    EXPECT_FALSE(openas::acts_for(*alice, planner));
    const auto bob = *openas::parse("person:bob");
    EXPECT_FALSE(openas::acts_for(reader, bob));
    // A kind alone; and what is not an actor.
    EXPECT_EQ(openas::format(*openas::parse("agent")), "agent");
    for (const auto* bad : {"", "robot:x", "agent:", "agent:a b", "agent:x;via=", "agent:x;via=person:a,", "agent:x;who=person:y", "agent:x;via=bogus"})
        EXPECT_FALSE(openas::parse(bad)) << bad;
}

XTEST(OpenAs, EffectsNameThemselvesAndTheUndoableOnesAskFirst, .area = "luban", .covers = {"CONTROL-DESCRIBE"}) {
    for (auto e : {openas::Effect::Read, openas::Effect::Reversible, openas::Effect::Irreversible, openas::Effect::External})
        EXPECT_EQ(openas::effect_from(openas::name_of(e)), e);
    EXPECT_FALSE(openas::effect_from("dangerous"));
    EXPECT_FALSE(openas::asks_first(openas::Effect::Read));
    EXPECT_FALSE(openas::asks_first(openas::Effect::Reversible));
    EXPECT_TRUE(openas::asks_first(openas::Effect::Irreversible));
    EXPECT_TRUE(openas::asks_first(openas::Effect::External));
}

XTEST(OpenAs, TheCheckRefusesEachBrokenClause, .area = "luban", .covers = {"CONTROL-DESCRIBE"}) {
    using V = openas::Verb;
    const auto yes = openas::Param{.name = "yes", .flag = true};
    const auto failed = [](const std::vector<V>& vs, std::string_view clause) {
        const auto r = openas::conformance::check(vs);
        return std::ranges::any_of(r.failed, [&](const std::string& f) { return f.starts_with(clause); });
    };
    const V good{.name = "rm", .summary = "Remove", .effect = openas::Effect::Irreversible, .options = {yes}, .answers = {"yes"}};
    EXPECT_TRUE(openas::conformance::check(std::vector<V>{good}).ok());
    EXPECT_TRUE(failed({V{.name = "Remove", .summary = "x"}}, "6.1"));
    EXPECT_TRUE(failed({good, good}, "6.2"));
    EXPECT_TRUE(failed({V{.name = "ls"}}, "6.3"));
    EXPECT_TRUE(failed({V{.name = "ls", .summary = "x", .options = {yes, yes}}}, "6.4"));
    EXPECT_TRUE(failed({V{.name = "ls", .summary = "x", .answers = {"yes"}}}, "6.5"));
    EXPECT_TRUE(failed({V{.name = "rm", .summary = "x", .effect = openas::Effect::Irreversible, .options = {yes}}}, "6.6"));
}

XTEST(OpenAs, LubansVerbsConformAndTheirArgumentsBecomeItsCommandLine, .area = "luban",
      .covers = {"CONTROL-DESCRIBE"}) {
    const auto verbs = luban::cli::verbs();
    const auto report = openas::conformance::check(verbs);
    for (const auto& f : report.failed) ADD_FAILURE() << f;
    EXPECT_GT(verbs.size(), 10u);
    // Every command of the tree is a verb or says why it is not.
    const auto has = [&](std::string_view n) { return std::ranges::any_of(verbs, [&](const auto& v) { return v.name == n; }); };
    for (const auto* n : {"new", "enter", "run", "status", "config", "rm", "export", "import", "write", "setup"}) EXPECT_TRUE(has(n)) << n;
    EXPECT_FALSE(has("interface"));
    const auto d = luban::cli::describe_json();
    EXPECT_EQ(d["openas"], std::string(openas::kVersion));
    EXPECT_EQ(d["client"], "luban");
    const auto verb = [&](std::string_view n) { return *std::ranges::find_if(verbs, [&](const auto& v) { return v.name == n; }); };
    auto argv = luban::cli::argv_of(verb("new"), Json{{"name", "box"}, {"edition", "tiny"}, {"yes", true}, {"json", false}});
    ASSERT_TRUE(argv) << argv.error();
    EXPECT_EQ(*argv, (std::vector<std::string>{"new", "box", "--edition", "tiny", "--yes"}));
    argv = luban::cli::argv_of(verb("run"), Json{{"name", "box"}, {"command", {"uname", "-a"}}});
    ASSERT_TRUE(argv) << argv.error();
    EXPECT_EQ(*argv, (std::vector<std::string>{"run", "box", "--", "uname", "-a"}));
    argv = luban::cli::argv_of(verb("config"), Json{{"name", "box"}, {"allow", {"clipboard", "gpu"}}});
    ASSERT_TRUE(argv) << argv.error();
    EXPECT_EQ(*argv, (std::vector<std::string>{"config", "box", "--allow", "clipboard", "--allow", "gpu"}));
    EXPECT_FALSE(luban::cli::argv_of(verb("new"), Json{{"edition", "tiny"}}));          // name is required
    EXPECT_FALSE(luban::cli::argv_of(verb("ls"), Json{{"bogus", 1}}));                  // no such option
    EXPECT_FALSE(luban::cli::argv_of(verb("rm"), Json{{"name", "box"}, {"yes", "y"}})); // a flag is true or false
    EXPECT_FALSE(luban::cli::argv_of(verb("ls"), Json::array()));
}
