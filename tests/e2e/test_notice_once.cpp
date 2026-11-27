// An upgrade is announced once per home, not once per command (E2E-100 of
// 2026.9.12.1, once notice_once_test.sh; #592). `print_migration_hint_once`'s
// `static bool` was per process, so "show this the first time ever" printed on
// every invocation; `notice::notice_once` keeps the decision in the home's
// `.xlings.json`, so the second process sees the first one said it. The
// notice is for a terminal only, so the commands run on a pseudo-terminal --
// without one the notice's own gate would pass this whether or not the
// once-only logic works.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.core.config;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
using Json = nlohmann::json;

#include "home_contract_support.hpp"

namespace hc = home_contract;

namespace {

tk::RunResult on_terminal(const tk::Home& home, std::vector<std::string> args) {
    tk::RunOptions o;
    o.argv = std::move(args);
    o.env["TERM"] = "xterm-256color";
    o.pty = true;
    o.pty_columns = 80;
    o.timeout = std::chrono::seconds(30);
    return home.xlings(o);
}

std::size_t count(std::string_view text, std::string_view needle) {
    std::size_t n = 0;
    for (auto at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) ++n;
    return n;
}

}  // namespace

XTEST(NoticeOnce, AnUpgradeIsAnnouncedOnceAndAConvergedHomeIsQuiet, .area = "cli",
      .covers = {"NOTICE-ONCE"}, .requires_ = {"pty", "xlings-bin"}) {
    auto home = hc::seeded("notice-once", {});
    const auto config = home.dir() / ".xlings.json";

    // I0: `self init` alone records no client version: nothing to compare, so
    // nothing is announced, on either run.
    ASSERT_FALSE(hc::read_json(config).contains("version")) << tk::read_file(config);
    for (int run = 0; run < 2; ++run) {
        const auto r = on_terminal(home, {"list"});
        EXPECT_EQ(count(r.out, "xlings is now"), 0u) << "run " << run << "\n" << r.out;
    }

    // A client far older than this one set the home up, and nothing has
    // verified it since.
    auto json = hc::read_json(config);
    json["version"] = "v0.4.40";
    hc::write_json(config, json);

    // I1: the mismatch is said on the first command, not on the next.
    const auto first = on_terminal(home, {"list"});
    EXPECT_LE(count(first.out, "xlings is now"), 1u) << first.out;
    const auto second = on_terminal(home, {"list"});
    EXPECT_EQ(count(second.out, "xlings is now"), 0u) << second.out;

    // I2: a converging `--fix` stamps verifiedBy, which silences the notice.
    const auto fix = home.xlings({"self", "doctor", "--fix", "-y"});
    ASSERT_EQ(fix.exit_code, 0) << "nothing is installed: no findings\n" << fix.transcript();
    EXPECT_EQ(hc::verified_by(home), std::string(xlings::Info::VERSION));
    const auto quiet = on_terminal(home, {"list"});
    EXPECT_EQ(count(quiet.out, "xlings is now"), 0u) << quiet.out;
    EXPECT_EQ(count(quiet.out, "self doctor --fix"), 0u) << "the old hint's command line\n" << quiet.out;
}

// I3: the per-process hint is deleted, not merely unreached.
XTEST(NoticeOnce, TheOldPerProcessHintIsNotInTheBinary, .area = "cli",
      .covers = {"NOTICE-ONCE"}, .requires_ = {"xlings-bin"}) {
    const auto binary = tk::read_file(tk::xlings_binary());
    ASSERT_FALSE(binary.empty());
    EXPECT_EQ(binary.find("packages installed by the previous client"), std::string::npos);
}
