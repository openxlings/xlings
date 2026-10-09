// xlings.guard and xlings.observe (modules/runtime, C1): the confirmation token, the Asker
// port, the journal and redaction. Both were lifted out of src/core with
// their behaviour unchanged; these pin that behaviour at the new seam.
#include <gtest/gtest.h>

import std;
import xlings.guard;
import xlings.observe;
import xlings.libs.json;

namespace g = xlings::guard;
namespace o = xlings::observe;
namespace fs = std::filesystem;

namespace {

struct ScriptedAsker : g::Asker {
    g::Reply reply;
    int asked = 0;
    g::Reply ask(const g::Question&) override { ++asked; return reply; }
};

fs::path temp_dir(std::string_view name) {
    auto d = fs::temp_directory_path() / std::format("xlings-guard-observe-{}-{}", name,
        std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(d);
    return d;
}

}  // namespace

TEST(Guard, AutoYesConfirmsWithoutAskingAndRecordsTheSpelling) {
    ScriptedAsker asker;
    auto a = g::ask(asker, {.id = "x", .question = "?"}, true, "-y");
    EXPECT_EQ(a.outcome, g::Outcome::Confirmed);
    ASSERT_TRUE(a.token);
    EXPECT_EQ(a.token->how(), "-y");
    EXPECT_EQ(asker.asked, 0);
}

TEST(Guard, OnlyAnExplicitYesConfirms) {
    ScriptedAsker yes;
    yes.reply = {.kind = g::Reply::Kind::Chosen, .value = "y"};
    auto a = g::ask(yes, {.id = "x", .question = "?"}, false, "-y");
    EXPECT_EQ(a.outcome, g::Outcome::Confirmed);
    EXPECT_EQ(a.token->how(), "terminal");

    ScriptedAsker no;
    no.reply = {.kind = g::Reply::Kind::Chosen, .value = "n"};
    EXPECT_EQ(g::ask(no, {.id = "x"}, false, "-y").outcome, g::Outcome::Declined);

    ScriptedAsker cancelled;
    cancelled.reply = {.kind = g::Reply::Kind::Cancelled};
    EXPECT_EQ(g::ask(cancelled, {.id = "x"}, false, "-y").outcome, g::Outcome::Declined);

    ScriptedAsker nobody;
    auto n = g::ask(nobody, {.id = "x"}, false, "-y");
    EXPECT_EQ(n.outcome, g::Outcome::NobodyToAsk);
    EXPECT_FALSE(n.token);
}

TEST(Guard, IsWithinIsLexical) {
    EXPECT_TRUE(g::is_within("/h/subos/a/home", "/h/subos/a"));
    EXPECT_TRUE(g::is_within("/h/subos/a", "/h/subos/a/"));
    EXPECT_FALSE(g::is_within("/h/subos/ab", "/h/subos/a"));
    EXPECT_FALSE(g::is_within("/h/subos/a/../b", "/h/subos/a"));
}

TEST(Observe, JournalAppendsReadsAndRotates) {
    auto dir = temp_dir("journal");
    auto file = dir / "logs" / "events.ndjson";
    o::JournalLimits limits{.max_bytes = 200, .keep = 2};
    for (int i = 0; i < 20; ++i)
        ASSERT_TRUE(o::append_checked(file, {.kind = o::Kind::Lifecycle, .fields = {{"n", i}}}, limits));
    EXPECT_TRUE(fs::exists(file));
    EXPECT_TRUE(fs::exists(fs::path(file.string() + ".1")));
    EXPECT_FALSE(fs::exists(fs::path(file.string() + ".3")));
    auto lines = o::read(file);
    ASSERT_FALSE(lines.empty());
    EXPECT_EQ(lines.back()["n"], 19);
    EXPECT_EQ(lines.back()["kind"], "lifecycle");
    EXPECT_TRUE(lines.back().contains("ts"));
    fs::remove_all(dir);
}

TEST(Observe, AppendNeverThrowsOnAnUnwritablePath) {
    // A path whose parent is a regular file cannot be created.
    auto dir = temp_dir("unwritable");
    std::ofstream(dir / "file") << "x";
    const auto file = dir / "file" / "sub" / "events.ndjson";
    EXPECT_FALSE(o::append_checked(file, {.kind = o::Kind::Ops}));
    EXPECT_FALSE(o::append_json_checked(file, {{"probe", true}}));
    EXPECT_NO_THROW(o::append(file, {.kind = o::Kind::Ops}));
    fs::remove_all(dir);
}

TEST(Observe, RedactionKeepsNamesOnly) {
    auto names = o::redact_env({{"GITHUB_TOKEN", "secret-value"}, {"PATH", "/bin"}});
    EXPECT_EQ(names, (std::vector<std::string>{"GITHUB_TOKEN", "PATH"}));
    EXPECT_TRUE(o::looks_secret("GITHUB_TOKEN"));
    EXPECT_TRUE(o::looks_secret("aws_secret_access_key"));
    EXPECT_FALSE(o::looks_secret("PATH"));
    EXPECT_FALSE(o::looks_secret("LANG"));
}

TEST(Observe, DestructiveRecordIsNeverRotatedAndCarriesIdentity) {
    auto home = temp_dir("destructive");
    o::destructive::set_identity("9.9.9", "xlings subos remove x -y");
    o::destructive::record(home, {.op = "subos-remove", .path = home / "subos" / "x",
                                  .bytes = 10, .files = 1, .confirmedBy = "-y"});
    auto lines = o::read(o::destructive::log_path(home));
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0]["op"], "subos-remove");
    EXPECT_EQ(lines[0]["xlings"], "9.9.9");
    EXPECT_EQ(lines[0]["confirmedBy"], "-y");
    EXPECT_EQ(o::destructive::human_bytes(2048), "2.0 KB");
    fs::remove_all(home);
}

TEST(Observe, FailedRotationDoesNotPretendToStoreAnEvent) {
    auto dir = temp_dir("rotation-error");
    auto file = dir / "events.ndjson";
    ASSERT_TRUE(o::append_checked(file, {.fields = {{"original", true}}}));
    fs::create_directories(dir / "events.ndjson.1");
    std::ofstream(dir / "events.ndjson.1" / "keep") << "existing evidence";
    EXPECT_FALSE(o::append_checked(file, {.fields = {{"next", true}}},
                                    {.max_bytes = 1, .keep = 1}));
    const auto lines = o::read(file, false);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_TRUE(lines.front().contains("original"));
    fs::remove_all(dir);
}

TEST(Observe, ADeviceThatOpensButCannotFlushReportsFailure) {
    if (!fs::exists("/dev/full")) GTEST_SKIP() << "requires a full-device fixture";
    EXPECT_FALSE(o::append_json_checked("/dev/full", {{"probe", true}}, {.max_bytes = 0}));
    EXPECT_NO_THROW(o::append_json("/dev/full", {{"probe", true}}, {.max_bytes = 0}));
}
