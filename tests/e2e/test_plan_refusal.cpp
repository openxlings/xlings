// The plan's refusal (design 2026-11-27 §5.5): entries that enumerate their
// architectures, none of them this machine's, refuse the request before
// anything is downloaded -- every such node of the closure in one report, the
// names by default, each chain with -v, the list as data for a client.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.core.config;
import xlings.platform.target;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

namespace {

std::string other_arch() {
    return std::string_view(xlings::platform::build_arch()) == "x86_64" ? "aarch64" : "x86_64";
}

// A package whose every platform has one artifact, for another machine.
void elsewhere_(const fs::path& index, std::string_view name) {
    const auto entry = std::format(
        "{{ ['1.0.0'] = {{ url = 'https://example.invalid/{0}.tar.gz', sha256 = {{ {1} = '{2}' }} }} }}",
        name, other_arch(), std::string(64, 'a'));
    tk::write_file(index / "pkgs" / name.substr(0, 1) / std::format("{}.lua", name), std::format(R"LUA(
package = {{
    spec = '3', name = '{0}', type = 'package', archs = {{ '{2}' }},
    xpm = {{ linux = {1}, macosx = {1}, windows = {1} }},
}}
)LUA", name, entry, other_arch()));
}

// A package with no artifact and the given runtime dependencies.
void hook_only_(const fs::path& index, std::string_view name, std::string_view deps) {
    const auto entry = std::format("{{ deps = {{ runtime = {{ {} }} }}, ['1.0.0'] = {{}} }}", deps);
    tk::write_file(index / "pkgs" / name.substr(0, 1) / std::format("{}.lua", name), std::format(R"LUA(
package = {{
    spec = '3', name = '{0}', type = 'package', archs = {{ 'x86_64', 'aarch64' }},
    xpm = {{ linux = {1}, macosx = {1}, windows = {1} }},
}}
)LUA", name, entry));
}

}  // namespace

XTEST(PlanRefusal, EveryNodeWithoutAnArtifactHereIsInOneReport, .area = "xim",
      .covers = {"ARCH-PLAN-REFUSAL"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("plan-refusal");
    const auto primary = home.root() / "primary", index = home.root() / "index";
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    fs::create_directories(primary / "pkgs");
    tk::write_file(index / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    elsewhere_(index, "alpha");
    elsewhere_(index, "beta");
    hook_only_(index, "gamma", "");
    hook_only_(index, "middle", "'fixture:beta'");
    hook_only_(index, "top", "'fixture:alpha', 'fixture:middle', 'fixture:gamma'");
    const nlohmann::json config = {
        {"mirror", "GLOBAL"},
        {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", nlohmann::json::array({
            {{"name", std::string(xlings::Config::DEFAULT_INDEX_REPO_NAME)},
             {"url", primary.generic_string()}, {"source", "git"}},
            {{"name", "fixture"}, {"url", index.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin" / (tk::is_windows ? "xlings.exe" : "xlings"));
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);

    const auto r = home.xlings({"install", "fixture:top@1.0.0", "-y"});
    const auto out = r.transcript();
    EXPECT_NE(r.exit_code, 0) << out;
    EXPECT_NE(out.find("E_UNSUPPORTED_TARGET: fixture:top"), std::string::npos) << out;
    EXPECT_NE(out.find("cannot be installed on"), std::string::npos) << out;
    EXPECT_NE(out.find("2 package(s) of its closure have no"), std::string::npos) << out;
    EXPECT_NE(out.find("fixture:alpha"), std::string::npos) << out;
    EXPECT_NE(out.find("fixture:beta"), std::string::npos) << out;
    EXPECT_NE(out.find("-v lists each"), std::string::npos) << out;
    // Once per request, not once per node.
    const auto first = out.find("E_UNSUPPORTED_TARGET");
    EXPECT_EQ(out.find("E_UNSUPPORTED_TARGET", first + 1), std::string::npos) << out;
    EXPECT_FALSE(fs::exists(home.dir() / "data" / "xpkgs" / "fixture-x-gamma")) << out;

    // -v: each chain, and what its entry gives.
    const auto verbose = home.xlings({"install", "fixture:top@1.0.0", "-y", "-v"});
    EXPECT_NE(verbose.transcript().find("-> fixture:middle"), std::string::npos) << verbose.transcript();
    EXPECT_NE(verbose.transcript().find("-" + other_arch()), std::string::npos) << verbose.transcript();

    // The request itself: its own line, with what it has.
    const auto alone = home.xlings({"install", "fixture:alpha@1.0.0", "-y"});
    EXPECT_NE(alone.exit_code, 0) << alone.transcript();
    EXPECT_NE(alone.transcript().find("E_UNSUPPORTED_TARGET: fixture:alpha has no"), std::string::npos)
        << alone.transcript();

    // A client: E_UNSUPPORTED, and the list.
    const auto wire = home.xlings({"interface", "install_packages", "--args",
                                   nlohmann::json{{"targets", {"fixture:top@1.0.0"}}, {"yes", true}}.dump()});
    bool coded = false;
    nlohmann::json listed;
    for (const auto& line : wire.json_lines()) {
        if (line.value("kind", "") == "error" && line.value("code", "") == "E_UNSUPPORTED") coded = true;
        if (line.value("kind", "") == "data" && line.value("dataKind", "") == "install_unsupported") {
            const auto& payload = line["payload"];
            listed = payload.is_string() ? nlohmann::json::parse(payload.get<std::string>(), nullptr, false) : payload;
        }
    }
    EXPECT_TRUE(coded) << wire.transcript();
    ASSERT_TRUE(listed.is_object()) << wire.transcript();
    ASSERT_EQ(listed["requests"].size(), 1u) << listed.dump();
    EXPECT_EQ(listed["requests"][0]["packages"].size(), 2u) << listed.dump();
}
