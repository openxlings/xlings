// Rule P at install (design 2026-11-27 §8.2): a payload's RPATH/RUNPATH, its
// pkg-config files and libtool archives name no path of the machine it was
// built on; this home, the payload, the system's directories and variables
// are fine.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.core.xim.build_paths;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace bp = xlings::xim::build_paths;

XTEST(BuildPaths, ABuildMachinePathIsFoundAndThisHomesAreNot, .area = "xim", .covers = {"PAYLOAD-BUILD-PATH"}) {
    auto home = tk::Home::isolated("build-paths");
    const auto payload = home.dir() / "data" / "xpkgs" / "xim-x-probe" / "1.0.0";
    tk::write_elf(payload / "bin" / "built-elsewhere", { .runpath = std::string("/home/builder/work/probe/lib") });
    tk::write_elf(payload / "bin" / "relative", { .runpath = std::string("$ORIGIN/../lib") });
    tk::write_elf(payload / "bin" / "relocated", { .rpath = (payload / "lib").generic_string() });
    tk::write_elf(payload / "bin" / "system", { .runpath = std::string("/usr/lib/x86_64-linux-gnu") });
    tk::write_file(payload / "lib" / "pkgconfig" / "probe.pc",
                   "prefix=/tmp/build-1234/probe\nlibdir=${prefix}/lib\nLibs: -L${libdir} -lprobe\n");
    tk::write_file(payload / "lib" / "pkgconfig" / "ok.pc",
                   "prefix=" + payload.generic_string() + "\nLibs: -L" + payload.generic_string() + "/lib\n");
    tk::write_file(payload / "lib" / "libprobe.la", "libdir='/usr/lib'\ndependency_libs=' -L/usr/lib64'\n");

    const auto report = bp::scan(payload, {home.dir()});
    ASSERT_EQ(report.files, 2u);
    std::set<std::string> paths;
    for (const auto& f : report.examples) paths.insert(f.path);
    EXPECT_EQ(paths, (std::set<std::string>{"/home/builder/work/probe/lib", "/tmp/build-1234/probe"}));
    const auto line = bp::describe(report, payload);
    EXPECT_NE(line.find("2 file(s) name a path of the machine the payload was built on"), std::string::npos) << line;
    EXPECT_NE(line.find("rule P"), std::string::npos) << line;

    // A libtool archive of another home, and a placeholder a build pads with.
    tk::write_file(payload / "lib" / "libother.la", "libdir='/Users/someone/.xlings/data/xpkgs/x/lib'\n");
    tk::write_elf(payload / "bin" / "padded", { .runpath = std::string("/nonexistent/xlings-use-rpath-not-default-search") });
    EXPECT_EQ(bp::scan(payload, {home.dir()}).files, 3u);
    EXPECT_TRUE(bp::describe({}, payload).empty());
}
