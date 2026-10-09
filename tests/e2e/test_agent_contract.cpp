// The agent contract (C13 + T4, design §13.3): with the audience declared as
// an agent, no command waits for input -- on a pseudo-terminal where nobody
// types, every command finishes. A command that needs an answer it was not
// given refuses with exit 2 and says which flag gives it.
//
// The scan walks the CLI's own spec (`--command-reference-json`), so a
// command added later is scanned without anyone remembering to add it here.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

namespace {

// Commands that reach the network (the index, a download) are out of a
// hermetic scan; they get the same treatment on the network lane.
const std::set<std::string> kNetwork{
    "install", "update", "search", "info", "why", "self install", "self update", "self init",
    "index list", "index use", "self doctor", "subos runtime", "use"};

std::vector<std::pair<std::string, std::vector<std::string>>> leaf_commands(const tk::Home& home) {
    auto r = home.xlings({"--command-reference-json"});
    auto root = nlohmann::json::parse(r.out, nullptr, false);
    std::vector<std::pair<std::string, std::vector<std::string>>> out;
    std::function<void(const nlohmann::json&, std::vector<std::string>)> walk =
        [&](const nlohmann::json& n, std::vector<std::string> path) {
            if (n["name"] != "xlings") path.push_back(n["name"].get<std::string>());
            const auto kids = n.value("subcommands", nlohmann::json::array());
            if (kids.empty()) {
                std::vector<std::string> args = path;
                for (auto& a : n["arguments"]) {
                    if (!a.value("required", false)) continue;
                    // Instance names get the instance that exists; the rest a placeholder.
                    args.push_back(path.front() == "subos" && a["name"] == "name" ? "box" : "x");
                }
                std::string joined;
                for (auto& p : path) joined += (joined.empty() ? "" : " ") + p;
                out.emplace_back(joined, args);
            }
            for (auto& k : kids) walk(k, path);
        };
    if (!root.is_discarded()) walk(root, {});
    return out;
}

tk::RunResult agent_on_pty(const tk::Home& home, std::vector<std::string> args,
                           std::map<std::string, std::string> extra = {}) {
    tk::RunOptions o;
    o.argv = std::move(args);
    o.env = std::move(extra);
    o.env["XLINGS_AGENT_MODE"] = "1";
    o.pty = true;                       // a terminal, and nobody at it
    o.timeout = std::chrono::seconds(30);
    return home.xlings(o);
}

}  // namespace

XTEST(AgentContract, NoCommandWaitsForInput,
      .area = "cli", .cost = tk::Cost::Slow, .covers = {"AGENT-NO-PROMPT"},
      .requires_ = {"pty", "xlings-bin"}) {
    auto home = tk::Home::isolated("agent-scan");
    fs::create_directories(home.dir() / "subos" / "default");
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    // Without a backend, `subos start` would go and fetch one -- the network,
    // not a question. Seeded, it runs here; not seedable, it is skipped.
    const bool backend = home.seed_sandbox_backend() && !tk::probe("sandbox");
    auto commands = leaf_commands(home);
    ASSERT_GT(commands.size(), 20u) << "the spec walk found too little";
    std::size_t scanned = 0;
    for (auto& [path, args] : commands) {
        if (kNetwork.contains(path)) continue;
        if (path == "subos remove" || path == "self uninstall") continue;   // below, on purpose
        if (path == "subos start" && !backend) continue;
        auto r = agent_on_pty(home, args);
        if (path == "subos start") (void)home.xlings({"subos", "stop", "box"});
        EXPECT_FALSE(r.timed_out) << "`xlings " << path << "` waited for input in agent mode\n"
                                  << r.transcript();
        ++scanned;
    }
    EXPECT_GT(scanned, 15u);
}

XTEST(AgentContract, AMissingConfirmationRefusesWithExit2AndDeletesNothing,
      .area = "cli", .cost = tk::Cost::Medium, .covers = {"EXIT-2-CONFIRM", "AGENT-NO-PROMPT"},
      .requires_ = {"pty", "xlings-bin"}) {
    auto home = tk::Home::isolated("agent-confirm");
    fs::create_directories(home.dir() / "subos" / "default");
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    auto r = agent_on_pty(home, {"subos", "remove", "box"});
    EXPECT_FALSE(r.timed_out) << r.transcript();
    EXPECT_EQ(r.exit_code, 2) << r.transcript();
    EXPECT_TRUE(fs::exists(home.dir() / "subos" / "box"));
    EXPECT_EQ(agent_on_pty(home, {"subos", "remove", "box", "-y"}).exit_code, 0);
    EXPECT_FALSE(fs::exists(home.dir() / "subos" / "box"));
}

XTEST(AgentContract, TheAudienceIsDeclaredByTheEnvironmentToo,
      .area = "cli", .cost = tk::Cost::Medium, .covers = {"AGENT-DECLARED"},
      .requires_ = {"pty", "xlings-bin"}) {
    auto home = tk::Home::isolated("agent-declared");
    fs::create_directories(home.dir() / "subos" / "default");
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    // An interactive shell is the one entry that waits by design: an agent is
    // told what to run instead, and nothing starts.
    auto r = agent_on_pty(home, {"subos", "use", "box"});
    EXPECT_FALSE(r.timed_out);
    EXPECT_EQ(r.exit_code, 2) << r.transcript();
    EXPECT_NE(r.out.find("subos exec box"), std::string::npos) << r.transcript();
    // XLINGS_AGENT_MODE=0 declares a human: the same command is not refused
    // for being interactive (it fails later, here, for having no terminal
    // shell -- what matters is the refusal above is gone).
    tk::RunOptions human;
    human.argv = {"subos", "use", "box", "--cmd", "true"};
    human.env = {{"XLINGS_AGENT_MODE", "0"}};
    EXPECT_EQ(home.xlings(human).exit_code, 0);
}

XTEST(AgentContract, AChoiceWithoutADefaultIsAnErrorWithCandidates,
      .area = "cli", .cost = tk::Cost::Medium, .covers = {"AGENT-CANDIDATES"},
      .requires_ = {"pty", "xlings-bin"}) {
    auto home = tk::Home::isolated("agent-candidates");
    fs::create_directories(home.dir() / "subos" / "default");
    ASSERT_EQ(home.xlings({"subos", "new", "box-a"}).exit_code, 0);
    ASSERT_EQ(home.xlings({"subos", "new", "box-b"}).exit_code, 0);
    auto r = agent_on_pty(home, {"subos", "exec", "box", "--", "true"});
    EXPECT_FALSE(r.timed_out);
    EXPECT_EQ(r.exit_code, 125) << r.transcript();
    EXPECT_NE(r.out.find("box-a"), std::string::npos) << r.transcript();
    EXPECT_NE(r.out.find("box-b"), std::string::npos) << r.transcript();
}

XTEST(AgentContract, TheDeclarationReachesTheXlingsInsideASandbox,
      .area = "cli", .cost = tk::Cost::Medium, .covers = {"AGENT-ENV-PROPAGATES"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"}) {
    auto home = tk::Home::isolated("agent-inside");
    home.seed_sandbox_backend();
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    auto r = home.xlings({"--agent", "subos", "exec", "box", "--sandbox", "--",
                          "/bin/sh", "-c", "echo mode=$XLINGS_AGENT_MODE"});
    EXPECT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_NE(r.out.find("mode=1"), std::string::npos) << r.transcript();
}
