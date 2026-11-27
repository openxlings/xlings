// A package's shell completion is a declaration (design 2026-11-27 §20.2,
// #744): xvm.add(name, { completion = { bash, zsh, fish, pwsh } }) links the
// payload's files into the subos's usr/share, they follow `xlings use` and go
// with the release, and xlings' profile loads them. The profile is xlings'
// own file: an upgrade rewrites it and keeps what it replaced.
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

constexpr std::string_view kRecipe = R"LUA(package = {
    spec = "1", name = "probe", description = "a program with shell completion",
    type = "package", archs = {"x86_64", "aarch64"}, status = "stable", programs = {"probe"},
    xpm = {
        linux = { ["1.0.0"] = {}, ["2.0.0"] = {} },
        macosx = { ["1.0.0"] = {}, ["2.0.0"] = {} },
        windows = { ["1.0.0"] = {}, ["2.0.0"] = {} },
    },
}
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")
function install()
    local dir = pkginfo.install_dir()
    local v = pkginfo.version()
    os.tryrm(dir)
    os.mkdir(path.join(dir, "bin"))
    os.mkdir(path.join(dir, "share", "completion"))
    io.writefile(path.join(dir, "bin", "probe"), "#!/bin/sh\necho probe@" .. v .. "\n")
    if os.host() ~= "windows" then os.exec("chmod +x " .. path.join(dir, "bin", "probe")) end
    io.writefile(path.join(dir, "share", "completion", "probe.bash"), "complete -W 'v" .. v .. "' probe\n")
    io.writefile(path.join(dir, "share", "completion", "_probe"), "#compdef probe\n_arguments '1: :(v" .. v .. ")'\n")
    io.writefile(path.join(dir, "share", "completion", "probe.fish"), "complete -c probe -a v" .. v .. "\n")
    io.writefile(path.join(dir, "share", "completion", "probe.ps1"), "# v" .. v .. "\n")
    return true
end
function config()
    xvm.add("probe", {
        bindir = path.join(pkginfo.install_dir(), "bin"),
        completion = {
            bash = "share/completion/probe.bash",
            zsh = "share/completion/_probe",
            fish = "share/completion/probe.fish",
            pwsh = "share/completion/probe.ps1",
        },
    })
    return true
end
function uninstall()
    xvm.remove("probe")
    return true
end
)LUA";

// Where each shell's file goes, under the subos root.
const std::vector<fs::path> kDestinations = {
    fs::path("usr/share/bash-completion/completions/probe"),
    fs::path("usr/share/zsh/site-functions/_probe"),
    fs::path("usr/share/fish/vendor_completions.d/probe.fish"),
    fs::path("usr/share/powershell/completions/probe.ps1"),
};

std::string read_or_none(const fs::path& file) {
    std::error_code ec;
    return fs::exists(file, ec) ? tk::read_file(file) : std::string("<none>");
}

}  // namespace

XTEST(PkgCompletion, DeclaredCompletionFollowsTheActiveVersionAndGoesWithTheRelease, .area = "xim",
      .covers = {"PKG-COMPLETION"}, .requires_ = {"xlings-bin"}) {
    auto home = hc::seeded("pkg-completion", {{"probe", std::string(kRecipe)}});
    const auto subos = home.dir() / "subos" / "default";

    ASSERT_EQ(home.xlings({"install", "probe@1.0.0", "-y"}).exit_code, 0);
    for (const auto& dst : kDestinations)
        EXPECT_NE(read_or_none(subos / dst).find("v1.0.0"), std::string::npos) << dst;
    // Another version installed beside it leaves the active one active.
    const auto second = home.xlings({"install", "probe@2.0.0", "-y"});
    ASSERT_EQ(second.exit_code, 0) << second.transcript();

    // `use` moves the program and its completion together, either way.
    for (const auto* version : {"2.0.0", "1.0.0"}) {
        const auto use = home.xlings({"use", "probe", version});
        ASSERT_EQ(use.exit_code, 0) << use.transcript();
        for (const auto& dst : kDestinations)
            EXPECT_NE(read_or_none(subos / dst).find(std::string("v") + version), std::string::npos)
                << dst << " after use " << version;
    }

    // The profile loads it: bash has the completion the active version declares.
    if constexpr (!tk::is_windows) {
        const auto profile = home.dir() / "config" / "shell" / "xlings-profile.sh";
        auto env = home.env();
        env["XLINGS_ACTIVE_SUBOS"] = "default";
        const auto bash = tk::run({.argv = {"/usr/bin/env", "bash", "--norc", "--noprofile", "-c",
                                            ". \"$0\" && complete -p probe", profile.string()},
                                   .env = std::move(env)});
        EXPECT_EQ(bash.exit_code, 0) << bash.transcript();
        EXPECT_TRUE(hc::has(bash.transcript(), "v1.0.0")) << bash.transcript();

        // zsh and fish where the machine has them (macOS has zsh): zsh binds
        // `_probe` once compinit has run, fish completes from its path.
        auto has_shell = [&](std::string_view shell) {
            return tk::run({.argv = {"/usr/bin/env", std::string(shell), "--version"}, .env = home.env()})
                       .exit_code == 0;
        };
        if (has_shell("zsh")) {
            auto zenv = home.env();
            zenv["XLINGS_ACTIVE_SUBOS"] = "default";
            const auto zsh = tk::run({.argv = {"/usr/bin/env", "zsh", "-f", "-c",
                                               "autoload -Uz compinit && compinit -u -D && . \"$0\" && "
                                               "print -r -- \"bound:${_comps[probe]}\"", profile.string()},
                                      .env = std::move(zenv)});
            EXPECT_EQ(zsh.exit_code, 0) << zsh.transcript();
            EXPECT_TRUE(hc::has(zsh.transcript(), "bound:_probe")) << zsh.transcript();
        }
        if (has_shell("fish")) {
            auto fenv = home.env();
            fenv["XLINGS_ACTIVE_SUBOS"] = "default";
            const auto fish_profile = home.dir() / "config" / "shell" / "xlings-profile.fish";
            const auto fish = tk::run({.argv = {"/usr/bin/env", "fish", "--no-config", "-c",
                                                "source $argv[1]; complete -C'probe '", fish_profile.string()},
                                       .env = std::move(fenv)});
            EXPECT_EQ(fish.exit_code, 0) << fish.transcript();
            EXPECT_TRUE(hc::has(fish.transcript(), "v1.0.0")) << fish.transcript();
        }
    }

    const auto remove = home.xlings({"remove", "probe", "--all", "-y"});
    EXPECT_EQ(remove.exit_code, 0) << remove.transcript();
    for (const auto& dst : kDestinations) {
        std::error_code ec;
        EXPECT_FALSE(fs::exists(fs::symlink_status(subos / dst, ec))) << dst;
    }
}

XTEST(PkgCompletion, AnUpgradeOfTheProfileKeepsWhatItReplaces, .area = "xim",
      .covers = {"PROFILE-UPGRADE-KEEPS"}, .requires_ = {"xlings-bin"}) {
    auto home = hc::seeded("profile-upgrade-keeps", {});
    const auto profile = home.dir() / "config" / "shell" / "xlings-profile.sh";
    ASSERT_TRUE(hc::has(tk::read_file(profile), "# xlings-profile-version: 14"));

    // An older profile with a block a package wrote into it.
    const std::string older = "# xlings-profile-version: 13\n# Xlings Shell Profile (bash/zsh)\n"
                              "# >>> a package's block >>>\ncomplete -W 'x' probe\n";
    tk::write_file(profile, older);
    const auto init = home.xlings({"self", "init"});
    ASSERT_EQ(init.exit_code, 0) << init.transcript();
    EXPECT_TRUE(hc::has(tk::read_file(profile), "# xlings-profile-version: 14"));
    const auto kept = home.dir() / "logs" / "profile-upgrades" / "xlings-profile.sh.v13";
    EXPECT_EQ(read_or_none(kept), older);

    // The current version is left as it is, edits included.
    tk::write_file(profile, tk::read_file(profile) + "# an edit of the current version\n");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    EXPECT_TRUE(hc::has(tk::read_file(profile), "# an edit of the current version"));
}
