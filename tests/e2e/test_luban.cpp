// The luban command against a real xlings (Luban design §B3): where it is,
// the environments, a new one from a template, and what a person and an
// agent are each told.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
import std;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

// luban beside the xlings under test: in a release `bin/luban`; in a
// development build `bin/luban-tool/luban`.
std::optional<fs::path> luban_binary() {
    if (const char* forced = std::getenv("LUBAN_BIN"); forced && *forced) return fs::path(forced);
    const auto xlings = tk::xlings_binary();
    if (xlings.empty()) return std::nullopt;
    for (const auto& candidate : {xlings.parent_path() / "luban", xlings.parent_path() / "luban-tool" / "luban"})
        if (fs::is_regular_file(candidate)) return candidate;
    return std::nullopt;
}

std::string template_recipe(std::string_view name, const Json& manifest) {
    return std::format(R"LUA(
package = {{ spec = '1', name = '{}', type = 'subos', archs = {{'x86_64', 'aarch64'}},
            xpm = {{ linux = {{ ['1.0.0'] = {{}} }} }} }}
import('xim.libxpkg.pkginfo')
function install()
    local dir = pkginfo.install_dir()
    os.mkdir(dir)
    io.writefile(path.join(dir, '.xlings.json'), {})
    return true
end
)LUA", name, Json(manifest.dump()).dump());
}

// A home with a fixture index (one template that declares a root and one
// package), xlings and luban in its bin/, and a bwrap of its own that says yes.
tk::Home luban_home(std::string_view name, const fs::path& luban) {
    auto home = tk::Home::isolated(name);
    const auto primary = home.root() / "primary";
    const auto repo = home.root() / "index";
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    fs::create_directories(primary / "pkgs");
    tk::write_file(repo / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    tk::write_file(repo / "pkgs/h/hello.lua", R"LUA(
package = { spec = '1', name = 'hello', archs = {'x86_64', 'aarch64'}, xpm = { linux = { ['1.0.0'] = {} } } }
import('xim.libxpkg.pkginfo')
function install()
    local dir = pkginfo.install_dir()
    os.mkdir(path.join(dir, 'bin'))
    io.writefile(path.join(dir, 'bin', 'hello'), '#!/bin/sh\necho hello\n')
    return true
end
)LUA");
    tk::write_file(repo / "pkgs/m/my-os.lua", template_recipe("my-os",
        Json{{"subos_kind", "rootfs"}, {"packages", {"fixture:hello@1.0.0"}}, {"workspace", Json::object()}}));
    // An agent workspace in miniature: a template that declares its policy.
    tk::write_file(repo / "pkgs/a/agent-ws.lua", template_recipe("agent-ws",
        Json{{"subos_kind", "rootfs"}, {"packages", {"fixture:hello@1.0.0"}}, {"policy", "fixture:agent-test@1.0.0"},
             {"workspace", Json::object()}}));
    tk::write_file(repo / "pkgs/a/agent-test.lua", R"LUA(
package = { spec = '1', name = 'agent-test', type = 'subos-policy', archs = {'x86_64', 'aarch64'},
            xpm = { linux = { ['1.0.0'] = {} }, macosx = { ['1.0.0'] = {} }, windows = { ['1.0.0'] = {} } } }
import('xim.libxpkg.pkginfo')
function install()
    local dir = pkginfo.install_dir()
    os.mkdir(dir)
    io.writefile(path.join(dir, 'policy.json'),
        '{"extends":"private","isolation":{"net":"none"},"permissions":{"fetch":"ask"}}')
    return true
end
)LUA");
    Json config{{"mirror", "GLOBAL"}, {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", Json::array({{{"name", "xim"}, {"url", primary.generic_string()}, {"source", "git"}},
                                    {{"name", "fixture"}, {"url", repo.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin/xlings");
    fs::copy_file(luban, home.dir() / "bin/luban");
    const auto bwrap = home.dir() / "data/xpkgs/xim-x-bwrap/0.11.2/bin/bwrap";
    tk::write_file(bwrap, "#!/bin/sh\nexit 0\n");
    fs::permissions(bwrap, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec);
    auto initialized = home.xlings({"self", "init"});
    EXPECT_EQ(initialized.exit_code, 0) << initialized.transcript();
    return home;
}

tk::RunResult luban(const tk::Home& home, std::vector<std::string> args, bool pty = false) {
    tk::RunOptions o;
    o.argv = {(home.dir() / "bin/luban").string()};
    o.argv.insert(o.argv.end(), args.begin(), args.end());
    o.env = home.env();
    o.env["XLINGS_TOOLS_SEARCH"] = "home";
    o.env.erase("XLINGS_SUBOS_MODE");
    o.env.erase("XLINGS_ACTIVE_SUBOS");
    o.pty = pty;
    o.timeout = std::chrono::seconds(90);
    return tk::run(o);
}

}  // namespace

// What this computer can do: one line per way of running a system, ready
// or why not and the next step -- on every platform the test runs on.
XTEST(Luban, TheHostStatusSaysWhatThisComputerCanRun, .area = "luban",
      .covers = {"PLATFORM-CAPS"}, .requires_ = {"xlings-bin"}) {
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test (mcpp build -p luban-tool, or LUBAN_BIN)";
    auto home = luban_home("luban-caps", *bin);
    auto st = luban(home, {"status", "--json"});
    ASSERT_FALSE(st.timed_out) << st.transcript();
    const auto j = Json::parse(st.out, nullptr, false);
    ASSERT_TRUE(j.is_object()) << st.transcript();
    ASSERT_EQ(j["modes"].size(), 3u) << st.out;
    bool any = false;
    for (const auto& m : j["modes"]) {
        ASSERT_TRUE(m["ready"].is_boolean()) << m.dump();
        if (m["ready"].get<bool>()) EXPECT_FALSE(m.value("how", std::string{}).empty()) << m.dump();
        else {
            EXPECT_FALSE(m.value("reason", std::string{}).empty()) << m.dump();
            EXPECT_FALSE(m.value("route", std::string{}).empty()) << m.dump();
        }
        if (m["name"] != "desktop" && m["ready"].get<bool>()) any = true;
    }
    EXPECT_EQ(st.exit_code, any ? 0 : 1) << st.transcript();
    for (const auto* kind : {"gpu", "display", "audio-output", "audio-input", "input"})
        EXPECT_TRUE(j["devices"].contains(kind)) << kind;
    auto text = luban(home, {"status"});
    for (const auto* word : {"sandbox", "machine", "desktop", "devices"})
        EXPECT_NE(text.out.find(word), std::string::npos) << text.transcript();
}

// A program's way in (part 4 §7): the verbs described, one run from JSON
// arguments reported as openas lines ending with its result, and the journal
// naming who ran it -- inherited by every process it started -- and which
// step started which.
XTEST(Luban, AProgramDescribesTheVerbsRunsOneAndTheJournalSaysWhoAndWhy, .area = "luban",
      .covers = {"CONTROL-DESCRIBE", "CONTROL-PRINCIPAL"}, .requires_ = {"xlings-bin"}) {
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test (mcpp build -p luban-tool, or LUBAN_BIN)";
    auto home = luban_home("luban-interface", *bin);
    auto d = luban(home, {"interface", "--describe"});
    ASSERT_EQ(d.exit_code, 0) << d.transcript();
    const auto desc = Json::parse(d.out, nullptr, false);
    ASSERT_TRUE(desc.is_object()) << d.out;
    EXPECT_EQ(desc["client"], "luban");
    bool rm_asks = false;
    for (const auto& v : desc["verbs"])
        if (v["name"] == "rm") rm_asks = v["effect"] == "irreversible" && v["answers"].size() > 0;
    EXPECT_TRUE(rm_asks) << d.out;

    // Who: the agent that runs it says so through XLINGS_ACTOR.
    tk::RunOptions o;
    o.argv = {(home.dir() / "bin/luban").string(), "interface", "status", "--args", "{}"};
    o.env = home.env();
    o.env["XLINGS_TOOLS_SEARCH"] = "home";
    o.env["XLINGS_ACTOR"] = "agent:tester;via=person:dev";
    o.env.erase("XLINGS_TRACE_ID");
    o.timeout = std::chrono::seconds(90);
    auto r = tk::run(o);
    std::vector<Json> lines;
    std::istringstream in(r.out);
    for (std::string l; std::getline(in, l);)
        if (!l.empty()) lines.push_back(Json::parse(l, nullptr, false));
    ASSERT_FALSE(lines.empty()) << r.transcript();
    for (const auto& l : lines) ASSERT_TRUE(l.is_object() && l.contains("kind")) << r.out;
    const auto& result = lines.back();
    EXPECT_EQ(result["kind"], "result") << r.out;
    EXPECT_EQ(result["exitCode"].get<int>(), r.exit_code);
    EXPECT_EQ(result["actor"], "agent:tester;via=person:dev");
    EXPECT_TRUE(std::ranges::any_of(lines, [](const Json& l) { return l["kind"] == "data" && l["data"].contains("modes"); })) << r.out;
    // A verb that is not luban's, or arguments it does not take: refused, as a result.
    auto bad = luban(home, {"interface", "ls", "--args", R"({"bogus":1})"});
    EXPECT_EQ(bad.exit_code, 1) << bad.transcript();
    EXPECT_NE(bad.out.find("E_INVALID_ARGS"), std::string::npos) << bad.out;

    // The journal of that run: every process names the actor, and each
    // names the step that started it.
    const auto trace = result["trace"].get<std::string>();
    ASSERT_FALSE(trace.empty());
    std::vector<Json> journal;
    for (const auto& day : fs::directory_iterator(home.dir() / "logs/ops"))
        if (fs::exists(day.path() / (trace + ".ndjson"))) {
            std::istringstream f(tk::read_file(day.path() / (trace + ".ndjson")));
            for (std::string l; std::getline(f, l);) journal.push_back(Json::parse(l, nullptr, false));
        }
    std::map<std::string, std::string> parent_of;
    std::string first;
    for (const auto& j : journal) {
        if (j.value("line", "") != "op" && j.value("line", "") != "child") continue;
        EXPECT_EQ(j.value("principal", ""), "agent:tester;via=person:dev") << j.dump();
        parent_of[j["proc"]] = j.value("parent", "");
        if (j["line"] == "op") first = j["proc"];
    }
    ASSERT_FALSE(first.empty()) << trace;
    EXPECT_GE(parent_of.size(), 2u) << "the verb ran as a process of its own";
    for (const auto& [proc, parent] : parent_of)
        if (proc != first) EXPECT_TRUE(parent_of.contains(parent)) << proc << " names " << parent;
}

XTEST(Luban, ANewEnvironmentFromATemplateIsListedAndDescribed,
      .area = "luban", .cost = tk::Cost::Medium,
      .covers = {"LUBAN-CLI-E2E", "LUBAN-CLI-MAP"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test (mcpp build -p luban-tool, or LUBAN_BIN)";
    auto home = luban_home("luban-new", *bin);

    auto empty = luban(home, {});
    ASSERT_EQ(empty.exit_code, 0) << empty.transcript();
    EXPECT_NE(empty.out.find("on this host"), std::string::npos) << empty.out;
    EXPECT_NE(empty.out.find("no environments yet"), std::string::npos) << empty.out;

    auto made = luban(home, {"new", "box", "--edition", "fixture:my-os@1.0.0"}, /*pty=*/true);
    ASSERT_FALSE(made.timed_out) << made.transcript();
    ASSERT_EQ(made.exit_code, 0) << made.transcript();
    EXPECT_TRUE(fs::exists(home.dir() / "data/xpkgs/fixture-x-hello/1.0.0/bin/hello"));

    auto overview = luban(home, {"--json"});
    ASSERT_EQ(overview.exit_code, 0) << overview.transcript();
    const auto j = Json::parse(overview.out);
    EXPECT_EQ(j["place"], "host");
    ASSERT_EQ(j["environments"].size(), 1u) << overview.out;
    EXPECT_EQ(j["environments"][0]["name"], "box");

    auto listed = luban(home, {"ls"});
    ASSERT_EQ(listed.exit_code, 0) << listed.transcript();
    EXPECT_NE(listed.out.find("box"), std::string::npos) << listed.out;

    auto status = luban(home, {"status", "box", "--json"});
    ASSERT_EQ(status.exit_code, 0) << status.transcript();
    EXPECT_TRUE(Json::parse(status.out, nullptr, false).is_object()) << status.out;
}

XTEST(Luban, AnAgentIsNeverAskedAndAPersonIsToldTheNearestCommand,
      .area = "luban", .covers = {"LUBAN-CLI-E2E", "UX-AGENT-NO-PROMPT", "LUBAN-CLI-SUGGEST"},
      .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test (mcpp build -p luban-tool, or LUBAN_BIN)";
    auto home = luban_home("luban-agent", *bin);
    auto made = luban(home, {"new", "box", "--edition", "fixture:my-os@1.0.0"});
    ASSERT_EQ(made.exit_code, 0) << made.transcript();

    // Removing asks first. An agent, with nobody to ask and no --yes: exit 2,
    // nothing removed, nothing waited for.
    auto refused = luban(home, {"--agent", "rm", "box"});
    ASSERT_FALSE(refused.timed_out) << "an agent waited on a question\n" << refused.transcript();
    EXPECT_EQ(refused.exit_code, 2) << refused.transcript();
    EXPECT_TRUE(fs::exists(home.dir() / "subos/box"));

    auto typo = luban(home, {"entr", "box"});
    EXPECT_EQ(typo.exit_code, 2);
    EXPECT_NE(typo.err.find("did you mean `luban enter`"), std::string::npos) << typo.transcript();
    auto typoJson = luban(home, {"--json", "entr"});
    EXPECT_EQ(Json::parse(typoJson.out)["suggestions"][0], "enter") << typoJson.out;

    auto removed = luban(home, {"rm", "box", "-y"});
    EXPECT_EQ(removed.exit_code, 0) << removed.transcript();
    EXPECT_FALSE(fs::exists(home.dir() / "subos/box"));
}

XTEST(Luban, AWorkspaceIsPrivateWhenItIsMadeNotAfter,
      .area = "luban", .cost = tk::Cost::Medium,
      .covers = {"LUBAN-WS-POLICY-AT-NEW"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test (mcpp build -p luban-tool, or LUBAN_BIN)";
    auto home = luban_home("luban-ws", *bin);
    auto made = luban(home, {"new", "agent", "--edition", "fixture:agent-ws@1.0.0", "--proxy", "socks5h://127.0.0.1:1080"});
    ASSERT_EQ(made.exit_code, 0) << made.transcript();
    // Before anything has entered it: the template's policy, selected and
    // locked, with the proxy as its only network.
    const auto file = home.dir() / "config/subos/agent/policy.json";
    ASSERT_TRUE(fs::exists(file)) << made.transcript();
    const auto doc = Json::parse(tk::read_file(file));
    EXPECT_EQ(doc["extends"], "fixture:agent-test@1.0.0") << doc.dump();
    EXPECT_EQ(doc["resolved"]["from"], "fixture:agent-test@1.0.0") << doc.dump();
    EXPECT_EQ(doc["isolation"]["net"], "proxy") << doc.dump();
    EXPECT_EQ(doc["isolation"]["proxy"], "socks5h://127.0.0.1:1080") << doc.dump();
}

// The default output is the conclusion and the next step (design §5.2): one
// line for what was made, how to go in, and none of the steps' own lines --
// those are -v's.
XTEST(Luban, NewSaysWhatItMadeAndHowToGoInAndNotItsSteps,
      .area = "luban", .cost = tk::Cost::Medium, .covers = {"LUBAN-OUTPUT"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    auto home = luban_home("luban-output", *bin);
    auto made = luban(home, {"new", "box", "--edition", "fixture:my-os@1.0.0"});
    ASSERT_EQ(made.exit_code, 0) << made.transcript();
    EXPECT_NE(made.out.find("making box"), std::string::npos) << made.out;
    // The fixture's root has no shell (as nano): the next step is a command
    // run in it, and going in is refused with that.
    EXPECT_NE(made.out.find("it has no shell"), std::string::npos) << made.out;
    EXPECT_NE(made.out.find("luban run box --"), std::string::npos) << made.out;
    auto entered = luban(home, {"enter", "box"});
    EXPECT_EQ(entered.exit_code, 1) << entered.transcript();
    EXPECT_NE(entered.err.find("box has no shell to go into"), std::string::npos) << entered.transcript();
    EXPECT_NE(entered.err.find("luban run box -- <command>"), std::string::npos) << entered.transcript();
    EXPECT_EQ(made.transcript().find("[xlings]"), std::string::npos) << "an xlings step's own line\n" << made.transcript();
    EXPECT_EQ(made.transcript().find("xlings subos use"), std::string::npos) << "an xlings command as the next step\n"
                                                                            << made.transcript();
    auto again = luban(home, {"new", "box2", "--edition", "fixture:my-os@1.0.0", "-v"});
    ASSERT_EQ(again.exit_code, 0) << again.transcript();
    EXPECT_GT(again.out.size(), made.out.size()) << "-v shows the steps\n" << again.out;
    auto json = luban(home, {"--json", "new", "box3", "--edition", "fixture:my-os@1.0.0"});
    ASSERT_EQ(json.exit_code, 0) << json.transcript();
    bool result = false;
    std::istringstream in(json.out);
    for (std::string line; std::getline(in, line);) {
        auto j = Json::parse(line, nullptr, false);
        ASSERT_TRUE(j.is_object()) << "not an event: " << line;
        result = result || j.contains("result");
    }
    EXPECT_TRUE(result) << json.out;
}

XTEST(Luban, AnEnvironmentsSettingsAreItsAndItsShellIsItsPasswds,
      .area = "luban", .cost = tk::Cost::Medium, .covers = {"LUBAN-SETTINGS", "LUBAN-SHELL"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    auto home = luban_home("luban-config", *bin);
    ASSERT_EQ(luban(home, {"new", "box", "--edition", "fixture:my-os@1.0.0"}).exit_code, 0);
    auto set = luban(home, {"config", "box", "--mode", "machine", "--persist", "home", "--on-close", "background"});
    ASSERT_EQ(set.exit_code, 0) << set.transcript();
    const auto doc = Json::parse(tk::read_file(home.dir() / "config/subos/box/machine.json"));
    EXPECT_EQ(doc["mode"], "machine");
    EXPECT_EQ(doc["persist"], "home");
    EXPECT_EQ(doc["on_close"], "background");
    auto bad = luban(home, {"config", "box", "--persist", "sometimes"});
    EXPECT_EQ(bad.exit_code, 2) << bad.transcript();
    auto shown = luban(home, {"--json", "config", "box"});
    ASSERT_EQ(shown.exit_code, 0) << shown.transcript();
    EXPECT_NE(shown.out.find("\"persist\":\"home\""), std::string::npos) << shown.out;
    // A shell the root does not have is refused with the install that brings it.
    auto noFish = luban(home, {"config", "box", "--shell", "fish"});
    EXPECT_NE(noFish.exit_code, 0);
    EXPECT_NE(noFish.err.find("xlings install fish --subos box"), std::string::npos) << noFish.transcript();
}

XTEST(Luban, AnExportedEnvironmentIsImportedWithItsStateAndANewPersona,
      .area = "luban", .cost = tk::Cost::Medium, .covers = {"LUBAN-ARCHIVE"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    auto home = luban_home("luban-archive-e2e", *bin);
    ASSERT_EQ(luban(home, {"new", "box", "--edition", "fixture:my-os@1.0.0"}).exit_code, 0);
    const auto tree = home.dir() / "subos/box/rootfs";
    ASSERT_TRUE(fs::is_directory(tree / "etc")) << "the fixture made no root";
    tk::write_file(tree / "root/notes.txt", "mine\n");
    // A link a package's configuration made into the home's store (as
    // ca-certificates' /etc/ssl one is): named by where it is, not where it points.
    // What a package's configuration makes (ca-certificates): an absolute link
    // to the environment's own etc on this host, which links into the store.
    const auto stored = home.dir() / "data/xpkgs/xim-x-ca-certificates/1/cacert.pem";
    tk::write_file(stored, "a certificate\n");
    fs::create_directories(home.dir() / "subos/box/etc/ssl");
    fs::create_symlink(stored, home.dir() / "subos/box/etc/ssl/cert.pem");
    fs::create_directories(tree / "etc/ssl");
    fs::create_symlink(home.dir() / "subos/box/etc/ssl/cert.pem", tree / "etc/ssl/cert.pem");
    // And one that means something inside the root.
    fs::create_symlink("/usr/share/zoneinfo/UTC", tree / "etc/localtime");
    ASSERT_EQ(luban(home, {"config", "box", "--persist", "home"}).exit_code, 0);
    const auto file = home.root() / "box.luban";
    auto exported = luban(home, {"export", "box", file.string()});
    ASSERT_EQ(exported.exit_code, 0) << exported.transcript();
    EXPECT_NE(exported.out.find("luban import"), std::string::npos) << exported.out;
    auto taken = luban(home, {"import", file.string()});
    EXPECT_EQ(taken.exit_code, 2) << "the name is taken\n" << taken.transcript();
    auto imported = luban(home, {"import", file.string(), "--name", "copy"});
    ASSERT_EQ(imported.exit_code, 0) << imported.transcript();
    EXPECT_EQ(tk::read_file(home.dir() / "subos/copy/rootfs/root/notes.txt"), "mine\n");
    // This host's link is not carried (the copy makes its own); the root's is.
    EXPECT_FALSE(fs::is_symlink(home.dir() / "subos/copy/rootfs/etc/ssl/cert.pem")
                 && fs::read_symlink(home.dir() / "subos/copy/rootfs/etc/ssl/cert.pem").string().find("/subos/box/")
                        != std::string::npos) << "the copy points at the original's files";
    EXPECT_EQ(fs::read_symlink(home.dir() / "subos/copy/rootfs/etc/localtime"), "/usr/share/zoneinfo/UTC");
    EXPECT_EQ(Json::parse(tk::read_file(home.dir() / "config/subos/copy/machine.json"))["persist"], "home");
}

XTEST(Luban, BootSaysWhatThisBootWas,
      .area = "luban", .cost = tk::Cost::Fast, .covers = {"LUBAN-BOOT-ENTRIES"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "boot entries are a Linux machine's";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    auto home = luban_home("luban-boot", *bin);
    // What stage-0 records of the boot it made (boot.json's `last`).
    tk::write_file(home.dir() / "boot.json",
                   R"({"default":"default","fallback":"default","once":null,"tries":{},)"
                   R"("last":{"subos":"default","via":"default","good":true}})");
    auto shown = luban(home, {"boot"});
    ASSERT_EQ(shown.exit_code, 0) << shown.transcript();
    EXPECT_NE(shown.out.find("default   default"), std::string::npos) << shown.out;
    EXPECT_NE(shown.out.find("this boot default via default, marked good"), std::string::npos) << shown.out;
}

XTEST(Luban, AnUpgradesPlanIsShownBeforeAnythingMovesAndTheOldNewOrderIsAnswered,
      .area = "luban", .cost = tk::Cost::Medium, .covers = {"LUBAN-UPGRADE-PLAN", "LUBAN-CLI-SUGGEST"},
      .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    auto home = luban_home("luban-upgrade-plan", *bin);
    // An edition with two versions: the newer one adds a package.
    const auto repo = home.root() / "index";
    tk::write_file(repo / "pkgs/w/world.lua", R"LUA(
package = { spec = '1', name = 'world', archs = {'x86_64', 'aarch64'}, xpm = { linux = { ['1.0.0'] = {} } } }
import('xim.libxpkg.pkginfo')
function install()
    os.mkdir(path.join(pkginfo.install_dir(), 'bin'))
    io.writefile(path.join(pkginfo.install_dir(), 'bin', 'world'), '#!/bin/sh\necho world\n')
    return true
end
)LUA");
    tk::write_file(repo / "pkgs/e/evolving.lua", R"LUA(
package = { spec = '1', name = 'evolving', type = 'subos', archs = {'x86_64', 'aarch64'},
            xpm = { linux = { ['latest'] = { ref = '1.1.0' }, ['1.0.0'] = {}, ['1.1.0'] = {} } } }
import('xim.libxpkg.pkginfo')
function install()
    local packages = '"fixture:hello@1.0.0"'
    if pkginfo.version() == '1.1.0' then packages = packages .. ', "fixture:world@1.0.0"' end
    os.mkdir(pkginfo.install_dir())
    io.writefile(path.join(pkginfo.install_dir(), '.xlings.json'),
        '{"subos_kind":"rootfs","packages":[' .. packages .. '],"workspace":{}}')
    return true
end
)LUA");
    auto old = luban(home, {"new", "box", "--edition", "fixture:evolving@1.0.0"});
    ASSERT_EQ(old.exit_code, 0) << old.transcript();
    auto plan = luban(home, {"upgrade", "box", "--dry-run"});
    ASSERT_EQ(plan.exit_code, 0) << plan.transcript();
    EXPECT_NE(plan.out.find("planning box's upgrade"), std::string::npos) << plan.out;
    EXPECT_EQ(plan.out.find("moving box"), std::string::npos) << "a dry run moves nothing\n" << plan.out;
    EXPECT_NE(plan.out.find("fixture:evolving@1.0.0 -> fixture:evolving@1.1.0"), std::string::npos) << plan.out;
    EXPECT_NE(plan.out.find("add      fixture:world@1.0.0"), std::string::npos) << plan.out;
    EXPECT_NE(plan.out.find("luban upgrade box"), std::string::npos) << "the command that applies it\n" << plan.out;
    auto as_json = luban(home, {"--json", "upgrade", "box", "--dry-run"});
    ASSERT_EQ(as_json.exit_code, 0) << as_json.transcript();
    EXPECT_NE(as_json.out.find(R"("up_to_date":false)"), std::string::npos) << as_json.out;

    auto fresh = luban(home, {"new", "box2", "--edition", "fixture:evolving"});
    ASSERT_EQ(fresh.exit_code, 0) << fresh.transcript();
    auto none = luban(home, {"upgrade", "box2", "--dry-run"});
    ASSERT_EQ(none.exit_code, 0) << none.transcript();
    EXPECT_NE(none.out.find("box2 is up to date"), std::string::npos) << none.out;

    // `luban new <name> <edition>`, the order before 2026.10.13.1.
    auto order = luban(home, {"new", "box3", "fixture:evolving"});
    EXPECT_EQ(order.exit_code, 2) << order.transcript();
    EXPECT_NE(order.err.find("luban new box3 --edition fixture:evolving"), std::string::npos) << order.err;
    EXPECT_FALSE(fs::exists(home.dir() / "subos/box3")) << "nothing is made from a guess";
}

XTEST(Luban, AnExportWithoutStageZeroSaysAMachineCannotBootIt,
      .area = "luban", .cost = tk::Cost::Medium, .covers = {"LUBAN-EXPORT-STAGE0"}, .requires_ = {"linux", "xlings-bin", "static-xlings"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    // This home's bin/ has xlings and luban, and no luban-init.
    auto home = luban_home("luban-export-stage0", *bin);
    ASSERT_FALSE(fs::exists(home.dir() / "bin/luban-init"));
    auto made = luban(home, {"new", "box", "--edition", "fixture:my-os@1.0.0"});
    ASSERT_EQ(made.exit_code, 0) << made.transcript();
    const auto dir = home.root() / "out" / "rootfs";
    auto exported = luban(home, {"export", "box", dir.string() + "/"});
    ASSERT_EQ(exported.exit_code, 0) << exported.transcript();
    EXPECT_NE(exported.transcript().find("a machine cannot boot it"), std::string::npos) << exported.transcript();
    EXPECT_NE(exported.transcript().find("xlings self update"), std::string::npos) << exported.transcript();
}

XTEST(Luban, SetupIsOneStepOnAHostWithNoBwrap,
      .area = "luban", .cost = tk::Cost::Medium, .covers = {"LUBAN-SETUP-ONE-STEP"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "the isolation setup is Linux's";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    auto home = luban_home("luban-setup", *bin);
    // A fresh machine: no bwrap anywhere this home looks (XLINGS_TOOLS_SEARCH=home),
    // and the index has one.
    fs::remove_all(home.dir() / "data/xpkgs/xim-x-bwrap");
    tk::write_file(home.root() / "primary/pkgs/b/bwrap.lua", R"LUA(
package = { spec = '1', name = 'bwrap', archs = {'x86_64', 'aarch64'}, xpm = { linux = { ['0.11.2'] = {} } } }
import('xim.libxpkg.pkginfo')
function install()
    local bin = path.join(pkginfo.install_dir(), 'bin')
    os.mkdir(bin)
    io.writefile(path.join(bin, 'bwrap'), '#!/bin/sh\nexit 0\n')
    os.execute('chmod 755 "' .. path.join(bin, 'bwrap') .. '"')
    return true
end
)LUA");
    auto setup = luban(home, {"setup"});
    EXPECT_NE(setup.transcript().find("installing xim:bwrap first"), std::string::npos) << setup.transcript();
    EXPECT_TRUE(fs::exists(home.dir() / "data/xpkgs/xim-x-bwrap/0.11.2/bin/bwrap")) << setup.transcript();
    EXPECT_EQ(setup.transcript().find("then run this again"), std::string::npos) << setup.transcript();
    EXPECT_EQ(setup.exit_code, 0) << setup.transcript();
}

XTEST(Luban, ARollbackTakesThePackagesBackSoTheNextEntryKeepsIt,
      .area = "luban", .cost = tk::Cost::Medium, .covers = {"LUBAN-ROLLBACK-PACKAGES"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    auto home = luban_home("luban-rollback", *bin);
    // A package that registers a program, as a real one does (xvm).
    tk::write_file(home.root() / "index/pkgs/w/world.lua", R"LUA(
package = { spec = '1', name = 'world', archs = {'x86_64', 'aarch64'}, xvm_enable = true,
            xpm = { linux = { ['1.0.0'] = {} } } }
import('xim.libxpkg.pkginfo')
import('xim.libxpkg.xvm')
function install()
    io.writefile(path.join(pkginfo.install_dir(), 'world'), '#!/bin/sh\necho world\n')
    return true
end
function config()
    xvm.add('world', { bindir = pkginfo.install_dir() })
    return true
end
)LUA");
    ASSERT_EQ(luban(home, {"new", "box", "--edition", "fixture:my-os@1.0.0"}).exit_code, 0);
    const auto workspace = [&] {
        auto j = Json::parse(tk::read_file(home.dir() / "subos/box/.xlings.json"), nullptr, false);
        return j.is_object() ? j.value("workspace", Json::object()) : Json::object();
    };
    ASSERT_FALSE(workspace().contains("world"));
    auto added = home.xlings({"install", "-y", "--subos", "box", "fixture:world@1.0.0"});
    ASSERT_EQ(added.exit_code, 0) << added.transcript();
    ASSERT_TRUE(workspace().contains("world")) << workspace().dump();
    auto back = luban(home, {"rollback", "box", "-y"});
    ASSERT_EQ(back.exit_code, 0) << back.transcript();
    // The packages are the earlier generation's: the next entry rebuilds that root.
    EXPECT_FALSE(workspace().contains("world")) << workspace().dump();
    EXPECT_EQ(back.transcript().find("made before generations recorded"), std::string::npos) << back.transcript();
}

XTEST(Luban, TheHostStatusIsWhatThisComputerCanMake,
      .area = "luban", .cost = tk::Cost::Fast, .covers = {"LUBAN-HOST-STATUS"}, .requires_ = {"xlings-bin"}) {
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    auto home = tk::Home::isolated("luban-host-status");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    tk::RunOptions o;
    o.argv = {bin->string(), "status"};
    o.env = home.env();
    o.env["LUBAN_XLINGS"] = tk::xlings_binary().string();
    o.timeout = std::chrono::seconds(60);
    auto r = tk::run(o);
    EXPECT_TRUE(r.exit_code == 0 || r.exit_code == 1) << r.transcript();
    EXPECT_NE(r.out.find("ready"), std::string::npos) << r.transcript();
    if constexpr (!tk::is_linux) {
        // Elsewhere a Luban system runs in a carrier: the status is its answer,
        // never "isolated here" (which no carrier made true).
        EXPECT_NE(r.out.find("Luban systems run in"), std::string::npos) << r.transcript();
        EXPECT_EQ(r.out.find("isolated here"), std::string::npos) << r.transcript();
    }
}
