// What a client that owns an index needs of xlings (design mcpp 2026-10-16
// §14): the index's data and its payloads where the client says (X1, X2), a
// read-only upstream it installs from by link (X3), what a payload is here
// without installing (X4), and the handshake (X5).
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.core.config;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

namespace {

void write_pkg(const fs::path& repo, std::string_view name) {
    tk::write_file(repo / "pkgs" / name.substr(0, 1) / std::format("{}.lua", name), std::format(R"LUA(
        package = {{
            spec = '1', name = '{0}', type = 'package', archs = {{'x86_64', 'aarch64'}},
            programs = {{'{0}'}},
            xpm = {{linux = {{['1.0.0'] = {{}}}}, macosx = {{['1.0.0'] = {{}}}}, windows = {{['1.0.0'] = {{}}}}}}
        }}
        import('xim.libxpkg.pkginfo')
        import('xim.libxpkg.xvm')
        function install()
            local dir = pkginfo.install_dir()
            os.mkdir(path.join(dir, 'bin'))
            io.writefile(path.join(dir, 'bin', '{0}'), '#!/bin/sh\necho {0}\n')
            return true
        end
        function config()
            xvm.add('{0}', {{bindir = path.join(pkginfo.install_dir(), 'bin')}})
            return true
        end
        function uninstall()
            xvm.remove('{0}')
            return true
        end
    )LUA", name));
}

// A home whose index `team` keeps its data in <root>/indices/team and its
// payloads in <root>/xpkgs; `extra` is merged into its .xlings.json.
tk::Home team_home(std::string_view label, const fs::path& repo, const nlohmann::json& extra = nlohmann::json::object()) {
    auto home = tk::Home::isolated(label);
    const auto primary = home.root() / "primary";
    fs::create_directories(primary / "pkgs");
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    nlohmann::json config{
        {"mirror", "GLOBAL"},
        {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", nlohmann::json::array({
            {{"name", std::string(xlings::Config::DEFAULT_INDEX_REPO_NAME)}, {"url", primary.generic_string()}, {"source", "git"}},
            {{"name", "team"}, {"url", repo.generic_string()}, {"source", "git"},
             {"dir", (home.root() / "indices" / "team").generic_string()},
             {"store", (home.root() / "xpkgs").generic_string()}}})}};
    config.update(extra);
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin" / (tk::is_windows ? "xlings.exe" : "xlings"));
    EXPECT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    return home;
}

fs::path repo_with_demo(const fs::path& root) {
    const auto repo = root / "team-index";
    tk::write_file(repo / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    write_pkg(repo, "demo");
    return repo;
}

// An install through the interface, whose events a client reads.
tk::RunResult install_(const tk::Home& home, std::string_view target) {
    return home.xlings({"interface", "install_packages", "--args",
                        nlohmann::json{{"targets", {std::string(target)}}, {"yes", true}}.dump()});
}

nlohmann::json data_event(const tk::RunResult& r, std::string_view kind) {
    for (const auto& line : r.json_lines())
        if (line.value("kind", "") == "data" && line.value("dataKind", "") == kind) {
            const auto& payload = line["payload"];
            return payload.is_string() ? nlohmann::json::parse(payload.get<std::string>(), nullptr, false) : payload;
        }
    return {};
}

}  // namespace

XTEST(IndexStore, AnIndexKeepsItsDataAndPayloadsWhereItsOwnerSays, .area = "xim",
      .covers = {"XIM-INDEX-DIR-STORE"}, .requires_ = {"xlings-bin"}) {
    auto scratch = tk::Home::isolated("index-store-repo");
    const auto repo = repo_with_demo(scratch.root());
    auto home = team_home("index-store", repo);
    const auto installed = home.xlings({"install", "team:demo@1.0.0", "-y"});
    ASSERT_EQ(installed.exit_code, 0) << installed.transcript();
    EXPECT_TRUE(fs::exists(home.root() / "indices" / "team" / "pkgs" / "d" / "demo.lua"));
    EXPECT_FALSE(fs::exists(home.dir() / "data" / "team")) << "not in the home's data dir";
    EXPECT_TRUE(fs::exists(home.root() / "xpkgs" / "team-x-demo" / "1.0.0" / "bin" / "demo"));
    EXPECT_FALSE(fs::exists(home.dir() / "data" / "xpkgs" / "team-x-demo"));

    // What it is here, without installing anything (X4).
    const auto status = home.xlings({"interface", "payload_status", "--args",
                                     R"({"targets":["team:demo@1.0.0","team:demo@9.9.9"]})"});
    const auto targets = data_event(status, "payload_status")["targets"];
    ASSERT_EQ(targets.size(), 2u) << status.transcript();
    EXPECT_TRUE(targets[0]["complete"].get<bool>()) << targets[0];
    EXPECT_EQ(targets[0]["origin"], "local");
    EXPECT_EQ(fs::path(targets[0]["payload_dir"].get<std::string>()),
              (home.root() / "xpkgs" / "team-x-demo" / "1.0.0").lexically_normal());
    EXPECT_TRUE(targets[1].contains("error")) << targets[1];

    // One index, into a directory its owner checks before swapping it in (X1).
    const auto staged = home.root() / "indices" / "team.staging";
    const auto synced = home.xlings({"interface", "sync_index", "--args",
                                     nlohmann::json{{"name", "team"}, {"dest", staged.generic_string()}}.dump()});
    EXPECT_EQ(data_event(synced, "index_synced")["name"], "team") << synced.transcript();
    EXPECT_TRUE(fs::exists(staged / "pkgs" / "d" / "demo.lua"));

    // Once there, it is its owner's to refresh: `update` leaves it as it is
    // (here a copy, not the link a local source is fetched as).
    const auto owned = home.root() / "indices" / "team";
    fs::remove(owned);
    fs::copy(repo, owned, fs::copy_options::recursive);
    write_pkg(repo, "other");
    ASSERT_EQ(home.xlings({"update"}).exit_code, 0);
    EXPECT_FALSE(fs::exists(home.root() / "indices" / "team" / "pkgs" / "o" / "other.lua"));
    home.xlings({"interface", "sync_index", "--args", R"({"name":"team"})"});
    EXPECT_TRUE(fs::exists(home.root() / "indices" / "team" / "pkgs" / "o" / "other.lua"));
}

XTEST(IndexStore, AnUpstreamPayloadIsLinkedNeverWrittenAndHealsWhenGone, .area = "xim",
      .covers = {"XIM-UPSTREAM"}, .requires_ = {"xlings-bin"}) {
    auto scratch = tk::Home::isolated("upstream-repo");
    const auto repo = repo_with_demo(scratch.root());
    auto upstream = team_home("upstream-owner", repo);
    ASSERT_EQ(upstream.xlings({"install", "team:demo@1.0.0", "-y"}).exit_code, 0);
    const auto theirs = upstream.root() / "xpkgs" / "team-x-demo" / "1.0.0";
    ASSERT_TRUE(fs::exists(theirs / "bin" / "demo"));

    auto home = team_home("upstream-borrower", repo, {{"upstreams", nlohmann::json::array({
        {{"home", upstream.dir().generic_string()}, {"store", (upstream.root() / "xpkgs").generic_string()},
         {"namespaces", {"team"}}}})}});
    const auto installed = install_(home, "team:demo@1.0.0");
    ASSERT_EQ(installed.exit_code, 0) << installed.transcript();
    const auto ours = home.root() / "xpkgs" / "team-x-demo" / "1.0.0";
    EXPECT_FALSE(fs::is_directory(fs::symlink_status(ours)) && !fs::is_symlink(ours)) << "a link entry, not a copy";
    EXPECT_TRUE(fs::exists(ours / "bin" / "demo"));
    const auto targets = data_event(installed, "install_targets")["targets"];
    ASSERT_FALSE(targets.empty()) << installed.transcript();
    EXPECT_EQ(targets[0]["origin"], "upstream");
    EXPECT_TRUE(targets[0]["complete"].get<bool>());

    // Removing it here removes the link, never their payload.
    ASSERT_EQ(home.xlings({"remove", "team:demo@1.0.0", "-y"}).exit_code, 0);
    EXPECT_FALSE(fs::exists(fs::symlink_status(ours)));
    EXPECT_TRUE(fs::exists(theirs / "bin" / "demo"));

    // Linked again, then the upstream lets it go: installed here, same path.
    ASSERT_EQ(home.xlings({"install", "team:demo@1.0.0", "-y"}).exit_code, 0);
    ASSERT_EQ(upstream.xlings({"remove", "team:demo@1.0.0", "-y"}).exit_code, 0);
    ASSERT_FALSE(fs::exists(theirs));
    const auto healed = install_(home, "team:demo@1.0.0");
    ASSERT_EQ(healed.exit_code, 0) << healed.transcript();
    EXPECT_TRUE(fs::exists(ours / "bin" / "demo"));
    EXPECT_EQ(data_event(healed, "install_targets")["targets"][0]["origin"], "local");
}

XTEST(IndexStore, TheHandshakeSaysWhatThisXlingsIs, .area = "interface", .covers = {"XIM-HANDSHAKE"},
      .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("handshake");
    const auto r = home.xlings({"interface", "--version"});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto j = nlohmann::json::parse(r.out, nullptr, false);
    ASSERT_TRUE(j.is_object()) << r.out;
    EXPECT_EQ(j["version"], std::string(xlings::Info::VERSION));
    EXPECT_FALSE(j["protocol_version"].get<std::string>().empty());
    const auto has = [&](const char* key, const char* value) {
        return std::ranges::find(j[key], nlohmann::json(value)) != j[key].end();
    };
    EXPECT_TRUE(has("capabilities", "install_packages"));
    EXPECT_TRUE(has("capabilities", "payload_status"));
    EXPECT_TRUE(has("features", "upstreams"));
    EXPECT_FALSE(j["home_layouts"].empty());
}
