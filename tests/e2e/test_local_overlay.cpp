// The local index is an overlay with provenance, three verbs, and a GC of the
// recipes the synced index has caught up with (E2E-101 of 2026.9.12, once
// local_overlay_test.sh; #592). A real machine had 159 recipes in its overlay,
// 157 byte-identical to what the synced index shipped -- each a bare name
// resolving ambiguously, with no command to list, attribute or clear them.
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

constexpr std::string_view kMake = R"LUA(package = {
    spec = "1",
    name = "make",
    description = "GNU Make — tool which controls the generation of executables from source files",
    type = "package", archs = {"x86_64", "aarch64"}, status = "stable",
    xpm = {
        linux   = { ["latest"] = { ref = "4.3" }, ["4.3"] = {}, ["4.4"] = {} },
        macosx  = { ["latest"] = { ref = "4.3" }, ["4.3"] = {}, ["4.4"] = {} },
        windows = { ["latest"] = { ref = "4.3" }, ["4.3"] = {}, ["4.4"] = {} },
    },
}
function install() return true end
)LUA";

// A recipe of the overlay only, of type config.
std::string local_only(std::string_view name, std::string_view what) {
    return std::format(R"LUA(package = {{
    spec = "1", name = "{}", description = "an overlay fixture: {}",
    type = "config", archs = {{"x86_64", "aarch64"}}, status = "dev",
    xpm = {{
        linux   = {{ ["latest"] = {{ ref = "1.0.0" }}, ["1.0.0"] = {{}} }},
        macosx  = {{ ["latest"] = {{ ref = "1.0.0" }}, ["1.0.0"] = {{}} }},
        windows = {{ ["latest"] = {{ ref = "1.0.0" }}, ["1.0.0"] = {{}} }},
    }},
}}
function install() return true end
function config() return true end
function uninstall() return true end
)LUA", name, what);
}

std::string replaced(std::string text, std::string_view from, std::string_view to) {
    for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
    return text;
}

// The line of `config --list-xpkg` for a recipe ("<name> ..."), or empty.
std::string row(const tk::RunResult& r, std::string_view name) {
    std::istringstream in(hc::plain(r.transcript()));
    for (std::string line; std::getline(in, line);)
        if (line.starts_with(std::string(name) + " ")) return line;
    return {};
}

}  // namespace

XTEST(LocalOverlay, ProvenanceVerbsAndTheGcOfWhatTheIndexCaughtUpWith, .area = "xim",
      .covers = {"LOCAL-OVERLAY"}, .requires_ = {"xlings-bin"}) {
    auto home = hc::seeded("local-overlay", {{"make", std::string(kMake)}});
    const auto upstream = hc::recipe_file(hc::index_of(home), "make");
    const auto overlay = home.dir() / "data" / "xim-pkgindex-local" / "pkgs";
    const auto local = home.root() / "local-src";
    const auto run = [&](std::vector<std::string> args) {
        return home.xlings(std::move(args), {{"XLINGS_NON_INTERACTIVE", "1"}});
    };
    ASSERT_EQ(run({"update"}).exit_code, 0);

    // S1: a byte-identical file is refused, and nothing lands in the overlay.
    auto r = run({"config", "--add-xpkg", upstream.string()});
    EXPECT_TRUE(hc::has_nocase(r.transcript(), "identical")) << r.transcript();
    EXPECT_FALSE(fs::exists(overlay / "m" / "make.lua"));

    // S2: a modified copy (same version, other bytes) is added, and listed
    // as modified.
    tk::write_file(local / "make.lua", replaced(std::string(kMake), "GNU Make —", "GNU Make (local variant) —"));
    r = run({"config", "--add-xpkg", (local / "make.lua").string()});
    EXPECT_TRUE(hc::has_nocase(r.transcript(), "add xpkg")) << r.transcript();
    EXPECT_TRUE(fs::exists(overlay / "m" / "make.lua"));
    r = run({"config", "--list-xpkg"});
    EXPECT_TRUE(hc::has_nocase(row(r, "make"), "modified")) << r.transcript();

    // S3: the synced index moving to a newer version makes it behind.
    tk::write_file(upstream, replaced(replaced(std::string(kMake), "ref = \"4.3\"", "ref = \"4.4\""),
                                      "GNU Make —", "GNU Make v4.4 —"));
    ASSERT_EQ(run({"update"}).exit_code, 0);
    r = run({"config", "--list-xpkg"});
    EXPECT_TRUE(hc::has_nocase(row(r, "make"), "behind")) << r.transcript();

    // S4: `--clear-xpkg stale` removes it.
    r = run({"config", "--clear-xpkg", "stale"});
    EXPECT_TRUE(hc::has(r.transcript(), "make")) << r.transcript();
    EXPECT_FALSE(fs::exists(overlay / "m" / "make.lua"));
    EXPECT_TRUE(row(run({"config", "--list-xpkg"}), "make").empty());

    // S5: `--remove-xpkg` deletes a unique entry by name; an unknown name
    // fails.
    tk::write_file(local / "zz-uniquepkg.lua", local_only("zz-uniquepkg", "exists only locally"));
    r = run({"config", "--add-xpkg", (local / "zz-uniquepkg.lua").string()});
    EXPECT_TRUE(hc::has_nocase(r.transcript(), "add xpkg")) << r.transcript();
    r = run({"config", "--list-xpkg"});
    EXPECT_TRUE(hc::has_nocase(row(r, "zz-uniquepkg"), "unique")) << r.transcript();
    r = run({"config", "--remove-xpkg", "zz-uniquepkg"});
    EXPECT_TRUE(hc::has(r.transcript(), "zz-uniquepkg")) << r.transcript();
    EXPECT_FALSE(fs::exists(overlay / "z" / "zz-uniquepkg.lua"));
    EXPECT_NE(run({"config", "--remove-xpkg", "no-such-package"}).exit_code, 0);

    // S5c: a recipe dropped into pkgs/ with no add-xpkg (the shape every
    // build before 2026.9.12 left) is listed as untracked and removed by name.
    tk::write_file(overlay / "z" / "zz-untrackedpkg.lua", local_only("zz-untrackedpkg", "dropped in"));
    r = run({"config", "--list-xpkg"});
    EXPECT_TRUE(hc::has_nocase(row(r, "zz-untrackedpkg"), "untracked")) << r.transcript();
    r = run({"config", "--remove-xpkg", "zz-untrackedpkg"});
    EXPECT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_TRUE(hc::has(r.transcript(), "zz-untrackedpkg")) << r.transcript();
    EXPECT_FALSE(fs::exists(overlay / "z" / "zz-untrackedpkg.lua"));

    // S6: `update` removes, unasked, an entry that has become identical to
    // the synced index.
    const auto synced = tk::read_file(upstream);
    tk::write_file(local / "make.lua", replaced(synced, "GNU Make v4.4 —", "GNU Make v4.4 (about to converge) —"));
    r = run({"config", "--add-xpkg", (local / "make.lua").string()});
    EXPECT_TRUE(hc::has_nocase(r.transcript(), "add xpkg")) << r.transcript();
    tk::write_file(overlay / "m" / "make.lua", synced);
    r = run({"update"});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_TRUE(hc::has_nocase(r.transcript(), "identical to the synced index were removed")) << r.transcript();
    EXPECT_FALSE(fs::exists(overlay / "m" / "make.lua"));

    // S7: an untracked recipe is collected when identical ...
    tk::write_file(overlay / "m" / "make.lua", synced);
    r = run({"config", "--list-xpkg"});
    EXPECT_TRUE(hc::has_nocase(row(r, "make"), "untracked")) << r.transcript();
    r = run({"update"});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_TRUE(hc::has_nocase(r.transcript(), "identical to the synced index were removed")) << r.transcript();
    EXPECT_FALSE(fs::exists(overlay / "m" / "make.lua"));
    // ... and survives, listed, when modified.
    tk::write_file(overlay / "m" / "make.lua", replaced(synced, "GNU Make v4.4 —", "GNU Make v4.4 (untracked, hand-edited) —"));
    r = run({"update"});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_FALSE(hc::has_nocase(r.transcript(), "identical to the synced index were removed")) << r.transcript();
    EXPECT_TRUE(fs::exists(overlay / "m" / "make.lua"));
    r = run({"config", "--list-xpkg"});
    EXPECT_TRUE(hc::has_nocase(row(r, "make"), "modified")) << r.transcript();
    EXPECT_TRUE(hc::has_nocase(row(r, "make"), "untracked")) << r.transcript();

    // I6: a same-version local duplicate does not print "namespace priority".
    run({"config", "--clear-xpkg", "all"});
    tk::write_file(overlay / "m" / "make.lua", replaced(synced, "GNU Make v4.4 —", "GNU Make v4.4 (local, same version) —"));
    r = run({"info", "make"});
    EXPECT_FALSE(hc::has_nocase(r.transcript(), "namespace priority")) << r.transcript();
}
