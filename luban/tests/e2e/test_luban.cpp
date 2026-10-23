// The luban command against a real xlings (Luban design §B3): where it is,
// the environments, a new one from a template, and what a person and an
// agent are each told.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
import std;
import xlings.libs.json;
import xlings.platform;
import openhal.audio;

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

// A fixture system, built on the fixture's official edition (luban:base) unless it says otherwise.
std::string template_recipe(std::string_view name, Json manifest) {
    if (!manifest.contains("from")) manifest["from"] = "luban:base@1.0.0";
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
tk::Home luban_home(std::string_view name, const fs::path& luban,
                    const std::function<void(const fs::path& primary, const fs::path& repo)>& more = {}) {
    auto home = tk::Home::isolated(name);
    const auto primary = home.root() / "primary";
    const auto repo = home.root() / "index";
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    fs::create_directories(primary / "pkgs");
    // The official index's edition every fixture system is built on (design
    // 2026-10-18 §4.4: a Luban system's chain reaches an official edition).
    tk::write_file(primary / "pkgs/luban/base.lua", R"LUA(
package = { spec = '1', name = 'base', namespace = 'luban', type = 'subos', categories = {'subos', 'distribution'},
            archs = {'x86_64', 'aarch64'}, xpm = { linux = { ['1.0.0'] = {} } } }
import('xim.libxpkg.pkginfo')
function install()
    local dir = pkginfo.install_dir()
    os.mkdir(dir)
    io.writefile(path.join(dir, '.xlings.json'), '{"subos_kind":"rootfs","packages":[],"workspace":{}}')
    return true
end
)LUA");
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
    // Shells a root gets while it is made (LUBAN-SHELL): fish and bash, from a
    // package the template installs -- after the root's first layout.
    tk::write_file(repo / "pkgs/f/fish.lua", R"LUA(
package = { spec = '1', name = 'fish', archs = {'x86_64', 'aarch64'}, xpm = { linux = { ['1.0.0'] = {} } } }
import('xim.libxpkg.pkginfo')
import('xim.libxpkg.xvm')
function install()
    local dir = pkginfo.install_dir()
    os.mkdir(path.join(dir, 'bin'))
    io.writefile(path.join(dir, 'bin', 'fish'), '#!/bin/sh\necho fish\n')
    io.writefile(path.join(dir, 'bin', 'bash'), '#!/bin/sh\necho bash\n')
    return true
end
function config()
    local bindir = path.join(pkginfo.install_dir(), 'bin')
    xvm.add('fish', { bindir = bindir })
    xvm.add('bash', { bindir = bindir, binding = 'fish@' .. pkginfo.version() })
    return true
end
)LUA");
    tk::write_file(repo / "pkgs/s/shell-os.lua", template_recipe("shell-os",
        Json{{"subos_kind", "rootfs"}, {"packages", {"fixture:hello@1.0.0", "fixture:fish@1.0.0"}}, {"workspace", Json::object()}}));
    // The same, its edition naming bash (usr/share/luban/settings.json).
    tk::write_file(repo / "pkgs/b/bash-os.lua", R"LUA(
package = { spec = '1', name = 'bash-os', type = 'subos', archs = {'x86_64', 'aarch64'},
            xpm = { linux = { ['1.0.0'] = {} } } }
import('xim.libxpkg.pkginfo')
function install()
    local dir = pkginfo.install_dir()
    os.mkdir(path.join(dir, 'usr', 'share', 'luban'))
    io.writefile(path.join(dir, '.xlings.json'), '{"subos_kind":"rootfs","from":"luban:base@1.0.0","packages":["fixture:hello@1.0.0","fixture:fish@1.0.0"],"workspace":{}}')
    io.writefile(path.join(dir, 'usr', 'share', 'luban', 'settings.json'), '{"login_shell":"/usr/bin/bash"}')
    return true
end
)LUA");
    // Two versions of a system, the newest needing a client from the future.
    tk::write_file(repo / "pkgs/n/next-os.lua", R"LUA(
package = { spec = '1', name = 'next-os', type = 'subos', archs = {'x86_64', 'aarch64'},
            xpm = { linux = { ['latest'] = { ref = '2.0.0' }, ['1.0.0'] = {}, ['2.0.0'] = { min_client = '9999.1.1.1' } } } }
import('xim.libxpkg.pkginfo')
function install()
    local dir = pkginfo.install_dir()
    os.mkdir(dir)
    io.writefile(path.join(dir, '.xlings.json'), '{"subos_kind":"rootfs","from":"luban:base@1.0.0","packages":["fixture:hello@1.0.0"],"workspace":{}}')
    return true
end
)LUA");
    // A system from the future: its manifest needs what no luban has yet.
    tk::write_file(repo / "pkgs/f/future-os.lua", template_recipe("future-os",
        Json{{"subos_kind", "rootfs"}, {"packages", {"fixture:hello@1.0.0"}}, {"workspace", Json::object()},
             {"luban", {{"requires", {"root-layout>=2", "warp-drive>=1"}}}}}));
    // A community system that names itself, with settings and a policy of its own.
    tk::write_file(repo / "pkgs/m/myos.lua", template_recipe("myos",
        Json{{"subos_kind", "rootfs"}, {"packages", {"fixture:hello@1.0.0"}}, {"workspace", Json::object()},
             {"luban", {{"os", {{"name", "MyOS"}}}, {"settings", {{"persist", "home"}}}}}}));
    // One built on nothing of Luban's.
    tk::write_file(repo / "pkgs/l/loner.lua", template_recipe("loner",
        Json{{"subos_kind", "rootfs"}, {"from", ""}, {"packages", {"fixture:hello@1.0.0"}}, {"workspace", Json::object()}}));
    // A desktop, by its category and its session (design 2026-10-18 §4.1): a community one ...
    auto desktop = [](std::string_view name, std::string_view ns) {
        return std::format(R"LUA(
package = {{ spec = '1', name = '{0}', namespace = '{1}', type = 'package', categories = {{'luban-desktop'}},
            archs = {{'x86_64', 'aarch64'}}, xpm = {{ linux = {{ ['1.0.0'] = {{}} }} }} }}
import('xim.libxpkg.pkginfo')
import('xim.libxpkg.xvm')
function install()
    local d = pkginfo.install_dir()
    os.mkdir(path.join(d, 'share', 'wayland-sessions'))
    io.writefile(path.join(d, 'share', 'wayland-sessions', '{0}.desktop'), '[Desktop Entry]\nName={0}\nExec={0}\n')
    os.mkdir(path.join(d, 'bin'))
    io.writefile(path.join(d, 'bin', '{0}'), '#!/bin/sh\n')
    return true
end
function config()
    xvm.add('{0}', {{ bindir = path.join(pkginfo.install_dir(), 'bin') }})
    return true
end
)LUA", name, ns);
    };
    tk::write_file(repo / "pkgs/h/hyper.lua", desktop("hyper", "fixture"));
    // ... and Luban's own, which `--desktop fancy` brings.
    tk::write_file(primary / "pkgs/luban/fancy.lua", desktop("fancy", "luban"));
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
    if (more) more(primary, repo);
    Json config{{"mirror", "GLOBAL"}, {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", Json::array({{{"name", "xim"}, {"url", primary.generic_string()}, {"source", "git"}},
                                    {{"name", "fixture"}, {"url", repo.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    fs::create_directories(home.dir() / "bin");
    // Linked where the file system allows (a sanitizer build is hundreds of
    // megabytes, and every test here makes a home), else copied.
    const auto place = [](const fs::path& from, const fs::path& to) {
        std::error_code ec;
        fs::create_hard_link(from, to, ec);
        if (ec) fs::copy_file(from, to);
    };
    place(tk::xlings_binary(), home.dir() / "bin/xlings");
    place(luban, home.dir() / "bin/luban");
    const auto bwrap = home.dir() / "data/xpkgs/xim-x-bwrap/0.11.2/bin/bwrap";
    tk::write_file(bwrap, "#!/bin/sh\nexit 0\n");
    fs::permissions(bwrap, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec);
    auto initialized = home.xlings({"self", "init"});
    EXPECT_EQ(initialized.exit_code, 0) << initialized.transcript();
    return home;
}

// The desktop a system's settings resolve to, as luban config says it.
std::string svc_settings_desktop(const tk::Home& home, std::string_view name);

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

std::string svc_settings_desktop(const tk::Home& home, std::string_view name) {
    auto shown = luban(home, {"--json", "config", std::string(name)});
    std::istringstream in(shown.out);
    for (std::string line; std::getline(in, line);)
        if (auto d = Json::parse(line, nullptr, false); d.is_object() && d.contains("result"))
            return d["result"].value("desktop", std::string{});
    return {};
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

// Who ran it, and how the journal knows (openas §2.5): given on the command
// line (--as), inherited from what started it (XLINGS_ACTOR), or observed.
XTEST(Luban, TheJournalSaysWhoRanItAndHowItKnows, .area = "luban",
      .covers = {"CONTROL-PRINCIPAL"}, .requires_ = {"xlings-bin"}) {
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test (mcpp build -p luban-tool, or LUBAN_BIN)";
    auto home = luban_home("luban-as", *bin);
    const auto ops_of = [&](const std::string& trace) {
        std::vector<Json> lines;
        for (const auto& day : fs::directory_iterator(home.dir() / "logs/ops"))
            if (fs::exists(day.path() / (trace + ".ndjson"))) {
                std::istringstream f(tk::read_file(day.path() / (trace + ".ndjson")));
                for (std::string l; std::getline(f, l);) lines.push_back(Json::parse(l, nullptr, false));
            }
        return lines;
    };
    int runs = 0;
    const auto first_line = [&](std::vector<std::string> args, std::map<std::string, std::string> env) {
        tk::RunOptions o;
        o.argv = {(home.dir() / "bin/luban").string()};
        o.argv.insert(o.argv.end(), args.begin(), args.end());
        o.env = home.env();
        o.env["XLINGS_TOOLS_SEARCH"] = "home";
        o.env["XLINGS_TRACE_ID"] = std::format("as-test-{}", ++runs);
        for (auto& [k, v] : env) o.env[k] = v;
        o.timeout = std::chrono::seconds(60);
        (void)tk::run(o);
        for (const auto& l : ops_of(o.env["XLINGS_TRACE_ID"]))
            if (l.value("client", "") == "luban") return l;
        return Json();
    };
    auto given = first_line({"--as", "agent:tester;via=person:dev", "config", "nobox"}, {});
    EXPECT_EQ(given.value("principal", ""), "agent:tester;via=person:dev") << given.dump();
    EXPECT_EQ(given.value("principal_source", ""), "given") << given.dump();
    auto inherited = first_line({"config", "nobox"}, {{"XLINGS_ACTOR", "agent:other"}});
    EXPECT_EQ(inherited.value("principal", ""), "agent:other") << inherited.dump();
    EXPECT_EQ(inherited.value("principal_source", ""), "inherited") << inherited.dump();
    // What is not an actor is not believed.
    auto bad = first_line({"--as", "root:me", "config", "nobox"}, {});
    EXPECT_NE(bad.value("principal", ""), "root:me") << bad.dump();
    EXPECT_EQ(bad.value("principal_source", ""), "observed") << bad.dump();
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

    auto made = luban(home, {"new", "box", "--xpkg", "fixture:my-os@1.0.0", "-y"}, /*pty=*/true);
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
      .area = "luban", .covers = {"LUBAN-CLI-E2E", "UX-AGENT-NO-PROMPT", "LUBAN-CLI-SUGGEST", "LUBAN-APP-WINDOW"},
      .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test (mcpp build -p luban-tool, or LUBAN_BIN)";
    auto home = luban_home("luban-agent", *bin);
    auto made = luban(home, {"new", "box", "--xpkg", "fixture:my-os@1.0.0", "-y"});
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
    // A program runs here or alone in a window; nothing else, and as whom is the terminal's to say.
    auto display = luban(home, {"run", "box", "--display", "screen", "--", "true"});
    EXPECT_EQ(display.exit_code, 2) << display.transcript();
    EXPECT_NE(display.err.find("--display terminal|window"), std::string::npos) << display.transcript();
    auto as = luban(home, {"run", "box", "--display", "window", "--user", "root", "--", "true"});
    EXPECT_EQ(as.exit_code, 2) << as.transcript();

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
    auto made = luban(home, {"new", "agent", "--xpkg", "fixture:agent-ws@1.0.0", "--proxy", "socks5h://127.0.0.1:1080", "-y"});
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
      .area = "luban", .cost = tk::Cost::Medium, .covers = {"LUBAN-OUTPUT", "LUBAN-PROGRESS"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    auto home = luban_home("luban-output", *bin);
    auto made = luban(home, {"new", "box", "--xpkg", "fixture:my-os@1.0.0", "-y"});
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
    auto again = luban(home, {"new", "box2", "--xpkg", "fixture:my-os@1.0.0", "-v", "-y"});
    ASSERT_EQ(again.exit_code, 0) << again.transcript();
    EXPECT_GT(again.out.size(), made.out.size()) << "-v shows the steps\n" << again.out;
    auto json = luban(home, {"--json", "new", "box3", "--xpkg", "fixture:my-os@1.0.0", "-y"});
    ASSERT_EQ(json.exit_code, 0) << json.transcript();
    bool result = false;
    double overall = 0;
    int measured = 0;
    std::istringstream in(json.out);
    for (std::string line; std::getline(in, line);) {
        auto j = Json::parse(line, nullptr, false);
        ASSERT_TRUE(j.is_object()) << "not an event: " << line;
        result = result || j.contains("result");
        // One progress for the whole of it, never going back (design 2026-10-20 G22).
        if (j.contains("overall")) {
            EXPECT_GE(j["overall"].get<double>(), overall) << line;
            overall = j["overall"].get<double>();
            ++measured;
        }
    }
    EXPECT_TRUE(result) << json.out;
    EXPECT_GT(measured, 0) << "no event carried the whole progress\n" << json.out;
}

// A name while it is made, and what an unfinished making left (design
// 2026-10-20 WuXian §11, H1): a record alone is said as that, with `luban
// rm`, which clears it; a name held by a making that runs is said, one held
// by a process that is gone is taken over.
XTEST(Luban, ANameIsHeldWhileItIsMadeAndAHalfMadeOneIsSaidAndCleared,
      .area = "luban", .cost = tk::Cost::Medium, .covers = {"LUBAN-NEW-NAME"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    auto home = luban_home("luban-new-names", *bin);
    // What a `new` that stopped half way leaves: its record, no root.
    const auto record = home.dir() / "config" / "subos" / "half";
    tk::write_file(record / "instance.json", R"({"kind":"rootfs"})");
    auto refused = luban(home, {"new", "half", "--xpkg", "fixture:my-os@1.0.0", "-y"});
    EXPECT_EQ(refused.exit_code, 2) << refused.transcript();
    EXPECT_NE(refused.err.find("an unfinished 'half' left its record"), std::string::npos) << refused.transcript();
    EXPECT_NE(refused.err.find("luban rm half"), std::string::npos) << refused.transcript();
    EXPECT_EQ(refused.transcript().find("usage: xlings"), std::string::npos) << refused.transcript();
    auto cleared = luban(home, {"rm", "half"});
    EXPECT_EQ(cleared.exit_code, 0) << "a record only is removed without asking\n" << cleared.transcript();
    EXPECT_FALSE(fs::exists(record)) << cleared.transcript();
    auto made = luban(home, {"new", "half", "--xpkg", "fixture:my-os@1.0.0", "-y"});
    EXPECT_EQ(made.exit_code, 0) << made.transcript();
    // Held by a making that runs (this test, as far as luban can tell): said.
    const auto names = home.dir() / "run" / "luban" / "names";
    tk::write_file(names / "held", std::format("{} 0\n", static_cast<int>(xlings::platform::get_pid())));
    auto busy = luban(home, {"new", "held", "--xpkg", "fixture:my-os@1.0.0", "-y"});
    EXPECT_EQ(busy.exit_code, 2) << busy.transcript();
    EXPECT_NE(busy.err.find("'held' is being made"), std::string::npos) << busy.transcript();
    // Held by one that is gone: taken over, and let go when it is made.
    tk::write_file(names / "stale", "2147483646 0\n");
    auto stale = luban(home, {"new", "stale", "--xpkg", "fixture:my-os@1.0.0", "-y"});
    EXPECT_EQ(stale.exit_code, 0) << stale.transcript();
    EXPECT_FALSE(fs::exists(names / "stale")) << "the making let it go";
    EXPECT_FALSE(fs::exists(names / "half"));
}

// The running phase's line is rewritten in place with `\r`; a line wider
// than the terminal wraps, `\r` returns only to the wrapped part, and every
// refresh added a line (2026.10.19.2, an 80-column terminal).
XTEST(Luban, TheRunningPhaseLineFitsTheTerminal,
      .area = "luban", .cost = tk::Cost::Medium, .covers = {"LUBAN-PROGRESS-WIDTH", "LUBAN-PROGRESS"},
      .requires_ = {"linux", "xlings-bin", "pty"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    auto home = luban_home("luban-progress-width", *bin);
    tk::RunOptions o;
    o.argv = {(home.dir() / "bin/luban").string(), "new", "box-with-a-rather-long-name", "--xpkg",
              "fixture:my-os@1.0.0", "-y"};
    o.env = home.env();
    o.env["XLINGS_TOOLS_SEARCH"] = "home";
    o.env.erase("XLINGS_SUBOS_MODE");
    o.env.erase("XLINGS_ACTIVE_SUBOS");
    o.pty = true;
    o.pty_columns = 30;
    o.timeout = std::chrono::seconds(90);
    const auto made = tk::run(o);
    ASSERT_EQ(made.exit_code, 0) << made.transcript();
    // Each piece the terminal draws between two `\r` / `\n`, its escapes
    // removed; one holding `▸` is the running phase's line.
    int running = 0, staged = 0;
    std::string piece;
    auto check = [&] {
        std::string plain;
        for (std::size_t i = 0; i < piece.size(); ++i) {
            if (piece[i] == '\033') {
                while (i < piece.size() && !std::isalpha(static_cast<unsigned char>(piece[i]))) ++i;
                continue;
            }
            plain += piece[i];
        }
        piece.clear();
        // The running phase's line, or the stage's under it (WuXian design §10).
        const bool stage_line = plain.starts_with("    ") && plain.find(" · ") != std::string::npos && plain.ends_with(" s");
        staged += stage_line;
        if (plain.find("▸") == std::string::npos && !stage_line) return;
        ++running;
        int cols = 0;
        for (unsigned char c : plain) cols += (c & 0xC0) != 0x80;
        EXPECT_LE(cols, 29) << "wider than the terminal less one: " << plain;
    };
    for (char c : made.out) {
        if (c == '\r' || c == '\n') check();
        else piece += c;
    }
    check();
    EXPECT_GT(running, 0) << made.out;
    EXPECT_GT(staged, 0) << "the stage's own line under the phase's\n" << made.out;
    EXPECT_NE(made.out.find("…"), std::string::npos) << "the cut is marked\n" << made.out;
}

XTEST(Luban, AnEnvironmentsSettingsAreItsAndItsShellIsItsPasswds,
      .area = "luban", .cost = tk::Cost::Medium, .covers = {"LUBAN-SETTINGS", "LUBAN-SHELL"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    auto home = luban_home("luban-config", *bin);
    ASSERT_EQ(luban(home, {"new", "box", "--xpkg", "fixture:my-os@1.0.0", "-y"}).exit_code, 0);
    auto set = luban(home, {"config", "box", "--mode", "machine", "--persist", "home", "--on-close", "background", "--files", "9p"});
    ASSERT_EQ(set.exit_code, 0) << set.transcript();
    const auto doc = Json::parse(tk::read_file(home.dir() / "config/subos/box/machine.json"));
    EXPECT_EQ(doc["mode"], "machine");
    EXPECT_EQ(doc["persist"], "home");
    EXPECT_EQ(doc["on_close"], "background");
    EXPECT_EQ(doc["files"], "9p");
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

// The login shell is the best the finished root has, and its edition's when
// it names one (design 2026-10-18 §11, E1, E2): the root's first layout runs
// before the packages, when only /bin/sh is there. Its user copies root's.
XTEST(Luban, ARootsLoginShellIsTheBestItHasOnceMade,
      .area = "luban", .cost = tk::Cost::Medium, .covers = {"LUBAN-SHELL"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    auto home = luban_home("luban-shell", *bin);
    // A user's shell in a system's passwd (the last field of its line).
    const auto shell = [&](std::string_view name, std::string_view user) {
        std::istringstream in(tk::read_file(home.dir() / "subos" / name / "rootfs/etc/passwd"));
        for (std::string line; std::getline(in, line);)
            if (line.starts_with(std::string(user) + ":")) return line.substr(line.rfind(':') + 1);
        return std::string("(no user)");
    };
    auto made = luban(home, {"new", "fishy", "--xpkg", "fixture:shell-os@1.0.0", "--user", "alice", "--no-password", "-y"});
    ASSERT_EQ(made.exit_code, 0) << made.transcript();
    EXPECT_EQ(shell("fishy", "root"), "/usr/bin/fish");
    EXPECT_EQ(shell("fishy", "alice"), "/usr/bin/fish") << "its user copies root's shell";
    auto named = luban(home, {"new", "bashy", "--xpkg", "fixture:bash-os@1.0.0", "-y"});
    ASSERT_EQ(named.exit_code, 0) << named.transcript();
    EXPECT_EQ(shell("bashy", "root"), "/usr/bin/bash") << "the edition's own choice";
    // Nothing to choose from: sh stays.
    ASSERT_EQ(luban(home, {"new", "plain", "--xpkg", "fixture:my-os@1.0.0", "-y"}).exit_code, 0);
    EXPECT_EQ(shell("plain", "root"), "/bin/sh");
}

// A system's user and sudo (design 2026-10-20 §4.5): given no password, the
// default one, said, and said again by status until changed; --no-password is
// a choice -- no password, and a sudo that asks nothing. sudo's rules are the
// system's: the wheel group, 15 minutes, xlings on its path, the proxy kept.
XTEST(Luban, AUserGivenNoPasswordHasTheDefaultOneAndNoPasswordIsAChoice,
      .area = "luban", .cost = tk::Cost::Medium, .covers = {"LUBAN-SUDO"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    auto home = luban_home("luban-sudo", *bin);
    const auto etc = [&](std::string_view name) { return home.dir() / "subos" / name / "rootfs/etc"; };
    auto made = luban(home, {"new", "box", "--xpkg", "fixture:shell-os@1.0.0", "--user", "alice", "-y"});
    ASSERT_EQ(made.exit_code, 0) << made.transcript();
    EXPECT_NE(made.transcript().find("password lubanoser (the default) -- passwd changes it"), std::string::npos) << made.transcript();
    const auto rules = tk::read_file(etc("box") / "sudoers.d/luban");
    for (const auto* line : {"%wheel ALL=(ALL:ALL) ALL", "timestamp_timeout=15", "/xlings/bin", "HTTPS_PROXY"})
        EXPECT_NE(rules.find(line), std::string::npos) << line << "\n" << rules;
    EXPECT_NE(tk::read_file(etc("box") / "sudoers").find("@includedir /etc/sudoers.d"), std::string::npos);
    EXPECT_FALSE(fs::exists(etc("box") / "sudoers.d/luban-alice")) << "a password: sudo asks for it";
    auto status = luban(home, {"status", "box"});
    EXPECT_NE(status.out.find("alice's password is still the default"), std::string::npos) << status.transcript();
    EXPECT_NE(status.out.find("luban run box --user alice -- passwd"), std::string::npos) << status.transcript();
    // No password, chosen: none, and sudo asks nothing.
    auto open = luban(home, {"new", "open", "--xpkg", "fixture:shell-os@1.0.0", "--user", "bob", "--no-password", "-y"});
    ASSERT_EQ(open.exit_code, 0) << open.transcript();
    EXPECT_NE(open.transcript().find("without a password; its sudo asks nothing"), std::string::npos) << open.transcript();
    EXPECT_NE(tk::read_file(etc("open") / "sudoers.d/luban-bob").find("bob ALL=(ALL:ALL) NOPASSWD: ALL"), std::string::npos);
    EXPECT_NE(tk::read_file(etc("open") / "shadow").find("bob::"), std::string::npos) << tk::read_file(etc("open") / "shadow");
    EXPECT_EQ(luban(home, {"status", "open"}).out.find("still the default"), std::string::npos);
}

// A template named without a version is the index's; when the index has no
// such package and one is installed, that one is used and said -- never
// silently (design 2026-10-19 §9.2).
XTEST(Luban, ATemplateOnlyInstalledIsUsedAndSaid,
      .area = "luban", .cost = tk::Cost::Medium, .covers = {"SUBOS-FROM-NO-SILENT-INSTALLED"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    auto home = luban_home("luban-installed-only", *bin);
    ASSERT_EQ(home.xlings({"install", "fixture:my-os@1.0.0", "-y"}).exit_code, 0);
    fs::remove(home.root() / "index/pkgs/m/my-os.lua");
    ASSERT_EQ(home.xlings({"update"}).exit_code, 0);
    const auto made = home.xlings({"interface", "subos_command", "--args",
                                   R"({"argv":["new","box","--rootfs","--from","fixture:my-os"],"yes":true})"});
    ASSERT_EQ(made.exit_code, 0) << made.transcript();
    int said = 0;
    for (const auto& j : made.json_lines())
        if (j.value("kind", "") == "data" && j.value("dataKind", "") == "installed_only") {
            ++said;
            EXPECT_EQ(j["payload"].value("version", ""), "1.0.0") << j.dump();
        }
    EXPECT_EQ(said, 1) << "said once, though the base is resolved twice\n" << made.out;
}

// A home with no index fetched yet (a fresh one, a carrier's own) gets it
// before deciding which version to make: not "could not be read", not
// whichever is installed (design 2026-10-19 §9.2; the luban-wsl2 lane).
XTEST(Luban, AHomeWithoutItsIndexYetFetchesItBeforeChoosingAVersion,
      .area = "luban", .cost = tk::Cost::Medium, .covers = {"SUBOS-FROM-NO-SILENT-INSTALLED"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    auto home = luban_home("luban-index-fetched", *bin);
    std::error_code ec;
    for (const auto* gone : {"data/xim-pkgindex", "data/xim-index-repos", "data/fixture"}) fs::remove_all(home.dir() / gone, ec);
    const auto made = home.xlings({"interface", "subos_command", "--args",
                                   R"({"argv":["new","box","--rootfs","--from","fixture:my-os"],"yes":true})"});
    ASSERT_EQ(made.exit_code, 0) << made.transcript();
    EXPECT_EQ(made.out.find("could not be read"), std::string::npos) << made.out;
    EXPECT_TRUE(fs::is_directory(home.dir() / "subos/box"));
}

// A window for a system is opened only through an app that says what it can
// do (design 2026-10-19 §10.3): one too old to be asked is not taken as
// compatible -- refused with the update, before anything starts.
XTEST(Luban, AWindowIsOpenedOnlyThroughAnAppThatSaysWhatItCanDo,
      .area = "luban", .cost = tk::Cost::Medium, .covers = {"LUBAN-OS-APP-CAPABILITIES", "LUBAN-CAPS"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    auto home = luban_home("luban-window-app", *bin);
    ASSERT_EQ(luban(home, {"new", "box", "--xpkg", "fixture:my-os@1.0.0", "-y"}).exit_code, 0);
    // An app of 2026.10.18.1: no __capabilities; its --version answers.
    const auto app = home.dir() / "bin/luban-os";
    tk::write_file(app, "#!/bin/sh\nfor a in \"$@\"; do [ \"$a\" = --version ] && { echo 'luban-os 2026.10.18.1'; exit 0; }; done\n"
                        "touch \"$0.opened\"; sleep 30\n");
    fs::permissions(app, fs::perms::owner_all, fs::perm_options::add);
    const auto refused = luban(home, {"run", "box", "--display", "window", "--", "true"});
    EXPECT_EQ(refused.exit_code, 1) << refused.transcript();
    EXPECT_NE(refused.err.find("cannot say what it can do"), std::string::npos) << refused.transcript();
    EXPECT_NE(refused.err.find("xlings update luban-os"), std::string::npos) << refused.transcript();
    EXPECT_FALSE(fs::exists(home.dir() / "bin/luban-os.opened")) << "nothing was opened";
    // An app that answers, and provides what the system needs, is used.
    tk::write_file(app, "#!/bin/sh\n[ \"$1\" = __capabilities ] && { echo '{\"version\":\"9\",\"provides\":{\"root-layout\":9},\"needs\":[]}'; exit 0; }\n"
                        "touch \"$0.opened\"\n");
    const auto opened = luban(home, {"run", "box", "--display", "window", "--", "true"});
    EXPECT_EQ(opened.exit_code, 0) << opened.transcript();
    EXPECT_TRUE(fs::exists(home.dir() / "bin/luban-os.opened"));
}

// What is not an edition is said in luban's words before xlings is asked
// (E14); an older edition than the index's newest, chosen because the newest
// needs a newer xlings, is said with the update (E8, B22).
XTEST(Luban, NewSaysWhatItCannotMakeAndWhyItMadeAnOlderOne,
      .area = "luban", .cost = tk::Cost::Medium, .covers = {"LUBAN-EDITION-MIN-CLIENT"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    auto home = luban_home("luban-new-edition", *bin);
    auto unknown = luban(home, {"new", "box", "--edition", "labwc-desktop"});
    EXPECT_EQ(unknown.exit_code, 2) << unknown.transcript();
    EXPECT_NE(unknown.err.find("no edition 'labwc-desktop'"), std::string::npos) << unknown.transcript();
    EXPECT_EQ(unknown.transcript().find("subos new"), std::string::npos) << "xlings's usage\n" << unknown.transcript();
    EXPECT_FALSE(fs::exists(home.dir() / "subos/box")) << "nothing was made";
    auto older = luban(home, {"--json", "new", "box", "--xpkg", "fixture:next-os", "-y"});
    ASSERT_EQ(older.exit_code, 0) << older.transcript();
    EXPECT_NE(older.out.find("next-os@2.0.0 needs xlings >= 9999.1.1.1"), std::string::npos) << older.out;
    EXPECT_NE(older.out.find("xlings self update"), std::string::npos) << older.out;
    EXPECT_NE(older.out.find("\"passed_over\""), std::string::npos) << "in the result\n" << older.out;
    // Where it came from is recorded with it (design 2026-10-18 §4.4).
    const auto record = Json::parse(tk::read_file(home.dir() / "config/subos/box/instance.json"), nullptr, false);
    ASSERT_TRUE(record.is_object());
    EXPECT_EQ(record["edition"].value("index", std::string{}), "fixture") << record.dump();
    auto text = luban(home, {"new", "box2", "--xpkg", "fixture:next-os", "-y"});
    ASSERT_EQ(text.exit_code, 0) << text.transcript();
    EXPECT_NE(text.transcript().find("needs xlings >= 9999.1.1.1"), std::string::npos) << text.transcript();
}

// What a system needs of the luban that runs it (design 2026-10-18 §9,
// opencaps): its root's layout and its manifest's requires; what this luban
// does not have is said, with the update.
XTEST(Luban, WhatASystemNeedsThatThisLubanLacksIsSaid,
      .area = "luban", .cost = tk::Cost::Medium, .covers = {"LUBAN-CAPS", "LUBAN-NETWORK-PROXY"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    auto home = luban_home("luban-caps-needs", *bin);
    // Not made by this luban: it says why before anything is fetched.
    auto refused = luban(home, {"new", "box", "--xpkg", "fixture:future-os@1.0.0", "-y"});
    EXPECT_EQ(refused.exit_code, 1) << refused.transcript();
    EXPECT_NE(refused.err.find("warp-drive"), std::string::npos) << refused.transcript();
    EXPECT_NE(refused.err.find("xlings update luban"), std::string::npos) << refused.transcript();
    EXPECT_FALSE(fs::exists(home.dir() / "subos/box"));
    // One made another way (an older luban, xlings itself): said by status.
    ASSERT_EQ(home.xlings({"subos", "new", "box", "--from", "fixture:future-os@1.0.0", "--rootfs"}).exit_code, 0);
    auto st = luban(home, {"--json", "status", "box"});
    ASSERT_EQ(st.exit_code, 0) << st.transcript();
    Json j;
    std::istringstream in(st.out);
    for (std::string line; std::getline(in, line);)
        if (auto d = Json::parse(line, nullptr, false); d.is_object() && d.contains("result")) j = d["result"];
    ASSERT_TRUE(j.is_object()) << st.out;
    EXPECT_EQ(j["layout"], 2) << j.dump();
    EXPECT_EQ(j["needs"], Json::array({"root-layout>=2", "warp-drive>=1"}));
    ASSERT_EQ(j["unmet"].size(), 1u) << j.dump();
    EXPECT_NE(j["unmet"][0].get<std::string>().find("warp-drive"), std::string::npos) << j.dump();
    auto text = luban(home, {"status", "box"});
    EXPECT_NE(text.out.find("xlings update luban"), std::string::npos) << text.transcript();
    // One that needs nothing it lacks says nothing of it.
    ASSERT_EQ(luban(home, {"new", "plain", "--xpkg", "fixture:my-os@1.0.0", "-y"}).exit_code, 0);
    const auto plain = luban(home, {"status", "plain"});
    EXPECT_EQ(plain.out.find("xlings update luban"), std::string::npos);
    // Its network in one line (design 2026-10-19 §27.2): like the host; as a
    // machine, the host's proxy on its loopback is the NAT's gateway inside.
    EXPECT_NE(plain.out.find("network   like the host"), std::string::npos) << plain.out;
    ASSERT_EQ(luban(home, {"config", "plain", "--mode", "machine"}).exit_code, 0);
    tk::RunOptions o;
    o.argv = {(home.dir() / "bin/luban").string(), "status", "plain"};
    o.env = home.env();
    o.env["XLINGS_TOOLS_SEARCH"] = "home";
    o.env["HTTPS_PROXY"] = "http://127.0.0.1:7890";
    const auto proxied = tk::run(o);
    EXPECT_NE(proxied.out.find("proxy http://10.0.2.2:7890 (the host's http://127.0.0.1:7890)"), std::string::npos) << proxied.out;
}

// Any system and any components by --xpkg (design 2026-10-18 §5): what each
// is, read from the index; one system, built on Luban; a third party's part
// agreed to once and remembered; what luban config takes, given at once; the
// official desktop --desktop names, brought; its name and os-release its own.
XTEST(Luban, AnyIndexsSystemAndComponentsAreMadeByXpkg,
      .area = "luban", .cost = tk::Cost::Medium, .covers = {"LUBAN-DIST-XPKG", "LUBAN-DIST-LINEAGE", "LUBAN-NAMESPACE", "LUBAN-DESKTOP-GRANTS"},
      .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    auto home = luban_home("luban-xpkg", *bin);
    // What it would be, nothing made.
    auto dry = luban(home, {"--json", "new", "box", "--xpkg", "fixture:myos@1.0.0", "--xpkg", "fixture:hyper", "--dry-run"});
    ASSERT_EQ(dry.exit_code, 0) << dry.transcript();
    EXPECT_NE(dry.out.find("MyOS 1.0.0 (Luban base)"), std::string::npos) << dry.out;
    EXPECT_NE(dry.out.find(R"("needs_confirmation":true)"), std::string::npos) << dry.out;
    EXPECT_FALSE(fs::exists(home.dir() / "subos/box"));
    // Nobody to ask: refused, with the flag that answers it.
    auto unasked = luban(home, {"new", "box", "--xpkg", "fixture:myos@1.0.0"});
    EXPECT_EQ(unasked.exit_code, 2) << unasked.transcript();
    EXPECT_NE(unasked.err.find("--yes"), std::string::npos) << unasked.transcript();
    EXPECT_FALSE(fs::exists(home.dir() / "subos/box"));
    // Agreed: made, with its component, its settings and its names.
    auto made = luban(home, {"--json", "new", "box", "--xpkg", "fixture:myos@1.0.0", "--xpkg", "fixture:hyper",
                             "--mode", "machine", "-y"});
    ASSERT_EQ(made.exit_code, 0) << made.transcript();
    Json result;
    std::istringstream in(made.out);
    for (std::string line; std::getline(in, line);)
        if (auto d = Json::parse(line, nullptr, false); d.is_object() && d.contains("result")) result = d["result"];
    ASSERT_TRUE(result.is_object()) << made.out;
    EXPECT_EQ(result["distribution"], "MyOS 1.0.0 (Luban base)");
    EXPECT_EQ(result["luban_base"], "base 1.0.0");
    EXPECT_EQ(result["desktops"], Json::array({"hyper"})) << "the component's session";
    const auto settings = Json::parse(tk::read_file(home.dir() / "config/subos/box/machine.json"), nullptr, false);
    EXPECT_EQ(settings.value("mode", ""), "machine");
    const auto release = tk::read_file(home.dir() / "subos/box/rootfs/etc/os-release");
    EXPECT_NE(release.find("ID=myos"), std::string::npos) << release;
    EXPECT_NE(release.find("ID_LIKE=luban"), std::string::npos) << release;
    EXPECT_TRUE(fs::exists(home.dir() / "config/luban/confirmed.json")) << "the answer is kept";
    // What it is, on three lines: the system, the desktop, the way in (design §3).
    auto status = luban(home, {"status", "box"});
    EXPECT_NE(status.out.find("system    MyOS 1.0.0 (Luban base)"), std::string::npos) << status.out;
    EXPECT_NE(status.out.find("desktop   hyper"), std::string::npos) << status.out;
    EXPECT_NE(status.out.find("enter     desktop · machine"), std::string::npos) << status.out;
    // Kept: the same again is not asked (its user given: a desktop's system has one).
    auto again = luban(home, {"new", "box2", "--xpkg", "fixture:myos@1.0.0", "--xpkg", "fixture:hyper", "--user", "alice",
                              "--no-password"});
    EXPECT_EQ(again.exit_code, 0) << again.transcript();
    // A desktop's grants (design 2026-10-20 WuXian §7.4, §9): its sound and
    // this computer's GPU; the clipboard only for one made in the window by a
    // person at it -- not by a command, which an agent may be.
    const auto policy = Json::parse(tk::read_file(home.dir() / "config/subos/box2/policy.json"), nullptr, false);
    ASSERT_TRUE(policy.is_object()) << tk::read_file(home.dir() / "config/subos/box2/policy.json");
    const auto granted = policy.dump();
    EXPECT_NE(granted.find("\"audio\""), std::string::npos) << granted;
    EXPECT_NE(granted.find("\"gpu\""), std::string::npos) << granted;
    EXPECT_EQ(granted.find("\"clipboard\""), std::string::npos) << granted;
    // The official desktop --desktop names, brought and said.
    auto fancy = luban(home, {"new", "box3", "--xpkg", "fixture:my-os@1.0.0", "--desktop", "fancy", "-y"});
    ASSERT_EQ(fancy.exit_code, 0) << fancy.transcript();
    EXPECT_NE(fancy.transcript().find("luban:fancy"), std::string::npos) << fancy.transcript();
    auto st = luban(home, {"--json", "status", "box3"});
    EXPECT_NE(st.out.find("fancy"), std::string::npos) << st.out;
    // What is refused, and the right way.
    auto two = luban(home, {"new", "x", "--xpkg", "fixture:myos@1.0.0", "--xpkg", "fixture:my-os@1.0.0", "-y"});
    EXPECT_EQ(two.exit_code, 2) << two.transcript();
    EXPECT_NE(two.err.find("both systems"), std::string::npos) << two.transcript();
    auto coord = luban(home, {"new", "x", "--edition", "fixture:myos@1.0.0"});
    EXPECT_EQ(coord.exit_code, 2) << coord.transcript();
    EXPECT_NE(coord.err.find("--xpkg fixture:myos@1.0.0"), std::string::npos) << coord.transcript();
    auto loner = luban(home, {"new", "x", "--xpkg", "fixture:loner@1.0.0", "-y"});
    EXPECT_EQ(loner.exit_code, 1) << loner.transcript();
    EXPECT_NE(loner.err.find("not built on a Luban edition"), std::string::npos) << loner.transcript();
    auto desk = luban(home, {"new", "x", "--edition", "fancy"});
    EXPECT_EQ(desk.exit_code, 2) << desk.transcript();
    EXPECT_NE(desk.err.find("fancy is a desktop, not a system"), std::string::npos) << desk.transcript();
    EXPECT_NE(desk.err.find("--desktop fancy"), std::string::npos) << desk.transcript();
    EXPECT_FALSE(fs::exists(home.dir() / "subos/x"));
    // An official short name: a name in the luban: namespace.
    auto base = luban(home, {"new", "plain", "--edition", "base"});
    EXPECT_EQ(base.exit_code, 0) << base.transcript();
    // And its published package's name, as luban took it before (luban-base).
    auto named = luban(home, {"new", "plain2", "--edition", "luban-base", "--dry-run"});
    EXPECT_EQ(named.exit_code, 0) << named.transcript();
}

// A published name, frozen, moves to the official edition that replaces it
// (design 2026-10-18 §15.1), and what it showed stays what it shows (§15.2).
XTEST(Luban, AFrozenEditionMovesToItsSuccessorKeepingWhatItShows,
      .area = "luban", .cost = tk::Cost::Medium, .covers = {"LUBAN-NAMESPACE", "LUBAN-DESKTOP-SWITCH"},
      .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    auto edition = [](std::string_view ns, std::string_view name, const Json& manifest) {
        return std::format(R"LUA(
package = {{ spec = '1', name = '{}', namespace = '{}', type = 'subos', categories = {{'subos', 'distribution'}},
            archs = {{'x86_64', 'aarch64'}}, xpm = {{ linux = {{ ['1.0.0'] = {{}} }} }} }}
import('xim.libxpkg.pkginfo')
function install()
    local dir = pkginfo.install_dir()
    os.mkdir(dir)
    io.writefile(path.join(dir, '.xlings.json'), {})
    return true
end
)LUA", name, ns, Json(manifest.dump()).dump());
    };
    auto home = luban_home("luban-successor", *bin, [&](const fs::path& primary, const fs::path&) {
        tk::write_file(primary / "pkgs/luban/luban-canvas.lua", edition("subos", "luban-canvas",
            Json{{"subos_kind", "rootfs"}, {"from", "luban:base@1.0.0"}, {"packages", Json::array()}, {"workspace", Json::object()},
                 {"luban", {{"settings", {{"desktop", "xdesk-canvas"}}}}}}));
        tk::write_file(primary / "pkgs/luban/next.lua", edition("luban", "next",
            Json{{"subos_kind", "rootfs"}, {"from", "luban:base@1.0.0"}, {"packages", Json::array()}, {"workspace", Json::object()},
                 {"replaces", "subos:luban-canvas"}, {"luban", {{"settings", {{"desktop", "xdesk-classic"}}}}}}));
    });
    auto made = luban(home, {"new", "box", "--edition", "canvas"});
    ASSERT_EQ(made.exit_code, 0) << made.transcript();
    EXPECT_EQ(svc_settings_desktop(home, "box"), "xdesk-canvas");
    auto up = luban(home, {"upgrade", "box", "-y"});
    ASSERT_EQ(up.exit_code, 0) << up.transcript();
    EXPECT_NE(up.transcript().find("which replaces it"), std::string::npos) << up.transcript();
    EXPECT_NE(up.transcript().find("keeps desktop xdesk-canvas"), std::string::npos) << up.transcript();
    const auto record = Json::parse(tk::read_file(home.dir() / "config/subos/box/instance.json"), nullptr, false);
    EXPECT_EQ(record["edition"].value("ref", std::string{}), "luban:next@1.0.0") << record.dump();
    const auto settings = Json::parse(tk::read_file(home.dir() / "config/subos/box/machine.json"), nullptr, false);
    EXPECT_EQ(settings.value("desktop", std::string{}), "xdesk-canvas") << "what it showed, kept";
    // xlings honours a replacement within one index only.
    auto foreign = home.xlings({"subos", "upgrade", "box", "--to", "fixture:my-os@1.0.0", "-y"});
    EXPECT_NE(foreign.exit_code, 0) << foreign.transcript();
    EXPECT_NE(foreign.transcript().find("does not replace"), std::string::npos) << foreign.transcript();
}

XTEST(Luban, AnExportedEnvironmentIsImportedWithItsStateAndANewPersona,
      .area = "luban", .cost = tk::Cost::Medium, .covers = {"LUBAN-ARCHIVE"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    auto home = luban_home("luban-archive-e2e", *bin);
    ASSERT_EQ(luban(home, {"new", "box", "--xpkg", "fixture:my-os@1.0.0", "-y"}).exit_code, 0);
    const auto tree = home.dir() / "subos/box/rootfs";
    ASSERT_TRUE(fs::is_directory(tree / "etc")) << "the fixture made no root";
    tk::write_file(tree / "root/notes.txt", "mine\n");
    // A package beyond its edition's: it comes along (an archive's workspace
    // entries are {active, installed} since 2026.10.20 -- read as versions,
    // they were all skipped).
    auto extra = home.xlings({"install", "-y", "--subos", "box", "fixture:fish@1.0.0"});
    ASSERT_EQ(extra.exit_code, 0) << extra.transcript();
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
    // And one that means something inside the root: a zone set inside, in
    // place of this computer's (part 2 J43) -- the system's own, kept.
    fs::remove(tree / "etc/localtime");
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
    EXPECT_TRUE(Json::parse(tk::read_file(home.dir() / "subos/copy/.xlings.json"))["workspace"].contains("fish"))
        << "the package beyond its edition's did not come along\n" << imported.transcript();
    // This host's link is not carried (the copy makes its own); the root's is.
    EXPECT_FALSE(fs::is_symlink(home.dir() / "subos/copy/rootfs/etc/ssl/cert.pem")
                 && fs::read_symlink(home.dir() / "subos/copy/rootfs/etc/ssl/cert.pem").string().find("/subos/box/")
                        != std::string::npos) << "the copy points at the original's files";
    EXPECT_EQ(fs::read_symlink(home.dir() / "subos/copy/rootfs/etc/localtime"), "/usr/share/zoneinfo/UTC");
    EXPECT_EQ(Json::parse(tk::read_file(home.dir() / "config/subos/copy/machine.json"))["persist"], "home");
}

// An archive carries what its system runs on -- its payloads and the index
// naming them -- and is imported with no network, no index and none of its
// payloads at hand (design 2026-10-19 §9.3: a .luban shared to another
// machine, offline).
XTEST(Luban, AnArchiveIsImportedWithoutTheNetworkFromWhatItCarries,
      .area = "luban", .cost = tk::Cost::Medium, .covers = {"SUBOS-INDEX-SNAPSHOT", "LUBAN-ARCHIVE"},
      .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    if (std::system("unshare -rn true >/dev/null 2>&1") != 0) GTEST_SKIP() << "no unprivileged network namespace here";
    auto home = luban_home("luban-archive-offline", *bin);
    ASSERT_EQ(luban(home, {"new", "box", "--xpkg", "fixture:my-os@1.0.0", "-y"}).exit_code, 0);
    tk::write_file(home.dir() / "subos/box/rootfs/root/notes.txt", "mine\n");
    const auto file = home.root() / "box.luban";
    const auto exported = luban(home, {"export", "box", file.string()});
    ASSERT_EQ(exported.exit_code, 0) << exported.transcript();
    // What it says it carries.
    std::string carried;
    {
        std::ifstream in(file, std::ios::binary);
        carried.assign(std::istreambuf_iterator<char>(in), {});
    }
    EXPECT_NE(carried.find("payloads/fixture-x-hello/1.0.0"), std::string::npos);
    EXPECT_NE(carried.find("payloads/fixture-x-my-os/1.0.0"), std::string::npos) << "the edition it was made from";
    EXPECT_NE(carried.find("index/"), std::string::npos);
    // Elsewhere: none of it here, and no network.
    ASSERT_EQ(luban(home, {"rm", "box", "-y"}).exit_code, 0);
    std::error_code ec;
    for (const auto* gone : {"data/xpkgs/fixture-x-hello", "data/xpkgs/fixture-x-my-os", "data/xim-pkgindex", "data/xim-index-repos",
                             "data/fixture"})
        fs::remove_all(home.dir() / gone, ec);
    for (const auto* gone : {"index", "primary"}) fs::remove_all(home.root() / gone, ec);
    for (const auto& d : fs::directory_iterator(home.dir() / "domains", ec)) fs::remove_all(d.path() / "private/data/xpkgs", ec);
    tk::RunOptions o;
    o.argv = {"unshare", "-rn", (home.dir() / "bin/luban").string(), "import", file.string()};
    o.env = home.env();
    o.env["XLINGS_TOOLS_SEARCH"] = "home";
    o.env.erase("XLINGS_SUBOS_MODE");
    o.env.erase("XLINGS_ACTIVE_SUBOS");
    o.timeout = std::chrono::seconds(120);
    const auto imported = tk::run(o);
    ASSERT_EQ(imported.exit_code, 0) << imported.transcript();
    EXPECT_EQ(tk::read_file(home.dir() / "subos/box/rootfs/root/notes.txt"), "mine\n");
    EXPECT_TRUE(fs::is_directory(home.dir() / "data/xpkgs/fixture-x-hello/1.0.0")
                || !fs::is_empty(home.dir() / "domains", ec))
        << "its package came from the archive";
    // Still no network: another made from what is here now (design 2026-10-19
    // §9.3 -- the no-network cases run in a network namespace; entering one,
    // which needs the release client, is the fresh suite's).
    tk::RunOptions n = o;
    n.argv = {"unshare", "-rn", (home.dir() / "bin/luban").string(), "new", "box2", "--xpkg", "fixture:my-os@1.0.0", "-y"};
    const auto made = tk::run(n);
    EXPECT_EQ(made.exit_code, 0) << made.transcript();
}

// What is here and what the index offers (design 2026-10-19 §6): `luban ls`
// with a name pattern (without one, as it always was), and `luban search`,
// grouped and ordered as the quick start shows them -- desktops by tier, the
// unrated last, a community one said so.
XTEST(Luban, LsTakesANamePatternAndSearchListsWhatTheIndexOffers,
      .area = "luban", .cost = tk::Cost::Medium, .covers = {"LUBAN-LS-SEARCH"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    auto home = luban_home("luban-ls-search", *bin, [](const fs::path& primary, const fs::path& repo) {
        const auto desktop = [](std::string_view name, std::string_view ns, std::string_view ext) {
            return std::format(R"LUA(
package = {{ spec = '1', name = '{}', namespace = '{}', description = 'a desktop', type = 'package', status = 'stable',
            categories = {{'luban-desktop'}}, archs = {{'x86_64', 'aarch64'}}, {}
            xpm = {{ linux = {{ ['1.0.0'] = {{}} }} }} }}
)LUA", name, ns, ext);
        };
        tk::write_file(primary / "pkgs/luban/heavy.lua", desktop("heavy", "luban", "extensions = { luban = { tier = 4, gpu = 'better', title = { en = 'Heavy' } } },"));
        tk::write_file(primary / "pkgs/luban/light.lua", desktop("light", "luban", "extensions = { luban = { tier = 1, title = { en = 'Light' } } },"));
        tk::write_file(repo / "pkgs/w/wild.lua", desktop("wild", "fixture", "extensions = { luban = { tier = 9 } },"));
    });
    ASSERT_EQ(luban(home, {"new", "box", "--xpkg", "fixture:my-os@1.0.0", "-y"}).exit_code, 0);
    ASSERT_EQ(luban(home, {"new", "work", "--xpkg", "fixture:my-os@1.0.0", "-y"}).exit_code, 0);
    const auto all = luban(home, {"--json", "ls"});
    ASSERT_EQ(all.exit_code, 0) << all.transcript();
    const auto listed = Json::parse(all.out, nullptr, false);
    ASSERT_TRUE(listed.is_object() && listed.contains("place") && listed["environments"].size() == 2) << all.out;
    const auto some = luban(home, {"--json", "ls", "wk"});
    ASSERT_EQ(some.exit_code, 0) << some.transcript();
    const auto picked = Json::parse(some.out, nullptr, false);
    ASSERT_EQ(picked["environments"].size(), 1u) << some.out;
    EXPECT_EQ(picked["environments"][0]["name"], "work") << "w..k, in order";

    const auto found = luban(home, {"--json", "search", "--desktops"});
    ASSERT_EQ(found.exit_code, 0) << found.transcript();
    const auto groups = Json::parse(found.out, nullptr, false);
    ASSERT_TRUE(groups.is_object() && groups["groups"].size() == 1) << found.out;
    // By tier, the unrated after (the fixture's own desktops are unrated too).
    std::vector<std::string> order;
    std::map<std::string, Json> by_ref;
    for (const auto& d : groups["groups"][0]["items"]) {
        order.push_back(d.value("ref", ""));
        by_ref[d.value("ref", "")] = d;
    }
    const auto at = [&](std::string_view ref) { return std::ranges::find(order, ref) - order.begin(); };
    ASSERT_EQ(at("luban:light"), 0) << found.out;
    ASSERT_EQ(at("luban:heavy"), 1) << found.out;
    EXPECT_LT(at("fixture:wild"), static_cast<long>(order.size())) << found.out;
    EXPECT_EQ(by_ref["luban:heavy"].value("gpu", ""), "better");
    EXPECT_EQ(by_ref["fixture:wild"].value("source", ""), "community");
    EXPECT_FALSE(by_ref["fixture:wild"].contains("tier")) << "a tier out of range is not taken";
    EXPECT_EQ(by_ref["fixture:wild"]["ignored"][0], "tier 9");
    const auto text = luban(home, {"search", "heavy"});
    EXPECT_NE(text.out.find("T4"), std::string::npos) << text.out;
    EXPECT_NE(text.out.find("GPU better"), std::string::npos) << text.out;
}

// The parts' versions in one place (design 2026-10-19 §11), and what to do
// where they disagree: an app too old to say what it can do.
XTEST(Luban, StatusSaysThePartsVersionsAndWhatToDoWhenTheyDisagree,
      .area = "luban", .cost = tk::Cost::Medium, .covers = {"LUBAN-VERSIONS"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    auto home = luban_home("luban-versions", *bin);
    const auto plain = luban(home, {"status"});
    EXPECT_NE(plain.out.find("versions"), std::string::npos) << plain.out;
    EXPECT_NE(plain.out.find("xlings "), std::string::npos) << plain.out;
    const auto app = home.dir() / "bin/luban-os";
    tk::write_file(app, "#!/bin/sh\necho 'luban-os 2026.10.17.1'\n");
    fs::permissions(app, fs::perms::owner_all, fs::perm_options::add);
    const auto status = luban(home, {"--json", "status"});
    const auto j = Json::parse(status.out, nullptr, false);
    ASSERT_TRUE(j.is_object() && j.contains("versions")) << status.out;
    const auto& v = j["versions"];
    EXPECT_FALSE(v.value("xlings", "").empty()) << v.dump();
    EXPECT_FALSE(v.value("luban", "").empty()) << v.dump();
    bool told = false;
    for (const auto& m : v["mismatches"])
        told = told || (m.value("what", "") == "luban-os" && m.value("next", "") == "xlings update luban-os");
    EXPECT_TRUE(told) << v.dump();
    const auto text = luban(home, {"status"});
    EXPECT_NE(text.out.find("xlings update luban-os"), std::string::npos) << text.out;
}

// What is shown before a system is made is what is made (design 2026-10-19
// §9.1): the plan's version is named to xlings, not resolved again.
XTEST(Luban, TheVersionShownIsTheVersionMade,
      .area = "luban", .cost = tk::Cost::Medium, .covers = {"LUBAN-EDITION-PINNED"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    auto home = luban_home("luban-pinned", *bin);
    const auto dry = luban(home, {"--json", "new", "box", "--xpkg", "fixture:next-os", "--dry-run"});
    ASSERT_EQ(dry.exit_code, 0) << dry.transcript();
    std::string shown;
    for (const auto& j : dry.json_lines())
        if (j.contains("result") && j["result"].is_object()) shown = j["result"].value("system", "");
    ASSERT_FALSE(shown.empty()) << dry.out;
    ASSERT_EQ(luban(home, {"new", "box", "--xpkg", "fixture:next-os", "-y"}).exit_code, 0);
    const auto record = Json::parse(tk::read_file(home.dir() / "config/subos/box/instance.json"), nullptr, false);
    EXPECT_EQ(record["edition"].value("ref", std::string{}), shown) << record.dump();
    EXPECT_NE(shown.find('@'), std::string::npos) << "a version, not a name to resolve again: " << shown;
}

// A move into its own userland keeps what the system was before it, and the
// way back (design 2026-10-19 §12.4): history names it first, rollback
// --to relayout asks for -y, then makes the system again from it -- its files
// as they were -- and the archive goes only with a confirmed removal.
XTEST(Luban, WhatASystemWasBeforeItsMoveIsKeptAndItsWayBack,
      .area = "luban", .cost = tk::Cost::Medium, .covers = {"LUBAN-UPGRADE-REVERSIBLE"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    auto home = luban_home("luban-relayout-back", *bin);
    ASSERT_EQ(luban(home, {"new", "box", "--xpkg", "fixture:my-os@1.0.0", "-y"}).exit_code, 0);
    const auto root = home.dir() / "subos/box/rootfs";
    tk::write_file(root / "root/before.txt", "before\n");
    // What a move keeps, where it keeps it.
    const auto kept = home.dir() / "state/luban/relayout/box.luban";
    fs::create_directories(kept.parent_path());
    ASSERT_EQ(luban(home, {"export", "box", kept.string()}).exit_code, 0);
    tk::write_file(root / "root/before.txt", "changed since\n");
    const auto history = luban(home, {"history", "box"});
    EXPECT_NE(history.out.find("before its move"), std::string::npos) << history.out;
    EXPECT_NE(history.out.find("luban rollback box --to relayout"), std::string::npos) << history.out;
    const auto status = luban(home, {"--json", "status", "box"});
    EXPECT_NE(status.out.find("luban rollback box --to relayout"), std::string::npos) << "in its notes\n" << status.out;
    const auto asked = luban(home, {"rollback", "box", "--to", "relayout"});
    EXPECT_EQ(asked.exit_code, 2) << asked.transcript();
    EXPECT_NE(asked.err.find("luban rollback box --to relayout -y"), std::string::npos) << asked.transcript();
    const auto back = luban(home, {"rollback", "box", "--to", "relayout", "-y"});
    ASSERT_EQ(back.exit_code, 0) << back.transcript();
    EXPECT_EQ(tk::read_file(home.dir() / "subos/box/rootfs/root/before.txt"), "before\n");
    EXPECT_TRUE(fs::exists(kept)) << "still the way back";
    ASSERT_EQ(luban(home, {"rm", "box", "-y"}).exit_code, 0);
    EXPECT_FALSE(fs::exists(kept)) << "it goes with the system";
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
        '{"subos_kind":"rootfs","from":"luban:base@1.0.0","packages":[' .. packages .. '],"workspace":{}}')
    return true
end
)LUA");
    auto old = luban(home, {"new", "box", "--xpkg", "fixture:evolving@1.0.0", "-y"});
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

    auto fresh = luban(home, {"new", "box2", "--xpkg", "fixture:evolving", "-y"});
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
    auto made = luban(home, {"new", "box", "--xpkg", "fixture:my-os@1.0.0", "-y"});
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
    ASSERT_EQ(luban(home, {"new", "box", "--xpkg", "fixture:my-os@1.0.0", "-y"}).exit_code, 0);
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
        // Elsewhere a Luban system runs in the Luban host machine: the status
        // is its carrier's answer, never "isolated here" (which no carrier
        // made true) -- ready in it, or why not and what to do.
        EXPECT_NE(r.out.find("sandbox"), std::string::npos) << r.transcript();
        EXPECT_EQ(r.out.find("isolated here"), std::string::npos) << r.transcript();
        EXPECT_TRUE(r.out.find("Luban host machine") != std::string::npos || r.out.find("do it:") != std::string::npos)
            << r.transcript();
    }
}

// A system's sound to the window (design 2026-10-20 §20.1): what its sound
// server writes into the `luban` sink's named pipe comes out as an openhal
// audio stream -- 48 kHz stereo, whole frames, the same samples.
XTEST(Luban, ASystemsSoundLeavesItAsAnOpenhalStream, .area = "luban", .covers = {"LUBAN-SOUND"}, .requires_ = {"linux", "xlings-bin"}) {
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test";
    auto home = tk::Home::isolated("luban-sound");
    const auto fifo = home.root() / "sink.pcm";
    ASSERT_TRUE(xlings::platform::make_fifo(fifo, 0600));
    const int fd = xlings::platform::open_device(fifo);
    ASSERT_GE(fd, 0);
    // A known wave: 1200 frames of a ramp, 4800 bytes.
    std::string wave;
    for (int k = 0; k < 1200; ++k)
        for (int ch = 0; ch < 2; ++ch) {
            const auto v = static_cast<std::uint16_t>(k * 37 + ch * 1000);
            wave += static_cast<char>(v & 0xff);
            wave += static_cast<char>(v >> 8);
        }
    ASSERT_TRUE(xlings::platform::write_all(fd, wave));
    tk::RunOptions o;
    o.argv = {bin->string(), "__audio", fifo.string()};
    o.env = home.env();
    o.timeout = std::chrono::seconds(20);
    const auto r = tk::run(o);
    std::vector<int> fds{fd};
    xlings::platform::close_fds(fds);
    EXPECT_EQ(r.exit_code, 0) << r.transcript();
    openhal::audio::Decoder d;
    d.feed(r.out);
    std::optional<openhal::audio::Open> opened;
    std::string samples;
    for (auto m = d.next(); m && *m; m = d.next()) {
        if (const auto* op = std::get_if<openhal::audio::Open>(&**m)) opened = *op;
        if (const auto* s = std::get_if<openhal::audio::Samples>(&**m)) samples += s->bytes;
    }
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened->format, (openhal::audio::Format{48000, 2}));
    EXPECT_EQ(opened->direction, openhal::audio::Direction::output);
    EXPECT_EQ(samples, wave);
}
