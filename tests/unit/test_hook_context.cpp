// The context a recipe's code runs with has one constructor (#566): every
// entry gets the same bin directory, SubOS root and architecture.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import mcpplibs.xpkg.executor;
import xlings.core.config;
import xlings.core.xim.hook_context;
import xlings.platform.target;

namespace fs = std::filesystem;

XTEST(HookContext, EveryEntryGetsTheSameArtifactPathsAndTheArchitecture,
      .area = "xim", .covers = {"HOOK-CONTEXT-ONE"}) {
    // A package's hooks and a script: two entries, one constructor.
    const auto hook = xlings::xim::make_execution_context({
        .pkg_name = "probe", .version = "1.0.0",
        .recipe = "/index/pkgs/p/probe.lua", .install_dir = "/store/probe/1.0.0",
        .xpkg_dir = "/index/pkgs/p",
    });
    const auto script = xlings::xim::make_execution_context({
        .recipe = "/scripts/s.lua", .xpkg_dir = "/store", .args = {"a", "b"},
    });
    const auto subos = xlings::Config::xvm_artifact_subos_dir();
    for (const auto* ctx : {&hook, &script}) {
        EXPECT_EQ(ctx->bin_dir, subos / "bin");
        EXPECT_EQ(fs::path(ctx->subos_sysrootdir), subos);
        // The home's host machine: what pkginfo.arch() answers in a hook.
        EXPECT_EQ(ctx->arch, std::string(xlings::platform::build_arch()));
        EXPECT_FALSE(ctx->platform.empty());
        EXPECT_FALSE(ctx->run_dir.empty());
        EXPECT_FALSE(ctx->dependency_store_roots.empty());
    }
    EXPECT_EQ(hook.install_dir, fs::path("/store/probe/1.0.0"));
    EXPECT_EQ(script.args, (std::vector<std::string>{"a", "b"}));
}
