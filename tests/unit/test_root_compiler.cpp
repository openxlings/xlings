#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.core.xvm.shim;
import xlings.core.xvm.types;
import xlings.subos.rootfs;
import xlings.platform.target;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace xvm = xlings::xvm;
namespace rf = xlings::subos::rootfs;

XTEST(RootCompiler, LogicalLoaderIsSelectedOnlyForARegisteredDriverInsideItsOwnProjection,
      .area = "subos", .covers = {"ROOT-LIBSEARCH"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "logical ELF loader is a Linux capability";
    auto home = tk::Home::isolated("root-compiler");
    const auto scope = home.dir() / "subos/box";
    const auto machine = home.root() / "machine";
    const auto name = xlings::platform::build_arch() == "aarch64"
        ? "ld-linux-aarch64.so.1" : "ld-linux-x86-64.so.2";
    const std::string loader = xlings::platform::build_arch() == "aarch64"
        ? std::string("/lib/") + name : std::string("/lib64/") + name;
    const auto payload = home.root() / "glibc";
    tk::write_file(payload / "lib" / name, "logical loader");
    rf::Inputs inputs;
    inputs.payloads = {payload};
    ASSERT_TRUE(rf::commit(scope, rf::plan(inputs), "compiler"));
    ASSERT_TRUE(rf::lay_out(machine, rf::usr_of(scope), home.dir()));
    ASSERT_TRUE(fs::is_regular_file(machine / fs::path(loader).relative_path()));
    xvm::VData driver;
    driver.bindingGroup = xvm::BindingGroupRef{.provider = "xim:gcc", .rootTarget = "xim-gnu-gcc"};
    const auto alias = "g++ --sysroot=" + scope.string();
    const auto projected = xvm::root_compiler_alias(alias, driver, scope, machine);
    ASSERT_TRUE(projected) << projected.error();
    EXPECT_EQ(*projected, "g++ --sysroot=/ -Wl,--dynamic-linker," + loader);
    const auto host = xvm::root_compiler_alias(alias, driver, scope, home.root());
    ASSERT_TRUE(host);
    EXPECT_EQ(*host, alias);
    driver.bindingGroup->provider = "custom:gcc";
    EXPECT_EQ(*xvm::root_compiler_alias(alias, driver, scope, machine), alias);
    driver.bindingGroup->provider = "xim:gcc";
    EXPECT_EQ(*xvm::root_compiler_alias("gcc-ar --sysroot=" + scope.string(), driver, scope, machine),
              "gcc-ar --sysroot=" + scope.string());
    EXPECT_EQ(*xvm::root_compiler_alias("g++ --sysroot=" + scope.string() + "-different", driver, scope, machine),
              "g++ --sysroot=" + scope.string() + "-different");
    fs::remove(machine / fs::path(loader).relative_path());
    const auto missing = xvm::root_compiler_alias(alias, driver, scope, machine);
    ASSERT_FALSE(missing);
    EXPECT_NE(missing.error().find("install glibc"), std::string::npos);
}
