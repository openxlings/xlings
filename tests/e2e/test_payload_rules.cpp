// The cases of docs/specs/payload-rules.md §5 through the index's check,
// `xlings __payload-check`: rule M on a payload's static surface against the
// architecture an entry lists it under, on any host; rule P on its search
// paths, .pc and .la files (design 2026-11-27 §8.2, K2).
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

namespace {

constexpr std::uint16_t kX86_64 = 62, kAarch64 = 183;

nlohmann::json check(const tk::Home& home, const fs::path& payload, std::string_view arch, int& exit_code) {
    const auto r = home.xlings({"__payload-check", payload.string(), "--arch", std::string(arch), "--json"});
    exit_code = r.exit_code;
    return nlohmann::json::parse(r.out, nullptr, false);
}

}  // namespace

XTEST(PayloadRules, TheIndexCheckFindsRuleMAndRulePAsTheSpecSays, .area = "xim",
      .covers = {"PAYLOAD-RULES-CHECK"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("payload-rules");
    int rc = -1;

    // An x86_64 program: holds for x86_64, rule M for aarch64.
    const auto one = home.root() / "one";
    tk::write_elf(one / "bin" / "tool", { .machine = kX86_64 });
    auto j = check(home, one, "x86_64", rc);
    EXPECT_EQ(rc, 0) << j.dump();
    EXPECT_TRUE(j.value("ok", false));
    j = check(home, one, "aarch64", rc);
    EXPECT_EQ(rc, 1) << j.dump();
    ASSERT_EQ(j["M"].size(), 1u) << j.dump();
    EXPECT_NE(j["M"][0].value("file", "").find("tool"), std::string::npos);
    EXPECT_EQ(j["M"][0].value("machine", ""), "x86_64");
    EXPECT_EQ(j["P"].value("files", -1), 0);

    // Off the surface: firmware under share/, a library below a library directory.
    const auto two = home.root() / "two";
    tk::write_elf(two / "lib" / "libx.so", { .machine = kAarch64 });
    tk::write_elf(two / "share" / "firmware.bin", { .machine = kX86_64 });
    tk::write_elf(two / "lib" / "sub" / "libdeep.so", { .machine = kX86_64 });
    j = check(home, two, "aarch64", rc);
    EXPECT_EQ(rc, 0) << j.dump();

    // Rule P: a build machine's path; $ORIGIN and the system's are fine.
    const auto three = home.root() / "three";
    tk::write_elf(three / "bin" / "built", { .runpath = std::string("/srv/ci/work/lib") });
    tk::write_elf(three / "bin" / "fine", { .runpath = std::string("$ORIGIN/../lib:/usr/lib") });
    tk::write_file(three / "lib" / "pkgconfig" / "x.pc", "prefix=/tmp/build-1/x\nLibs: -L${prefix}/lib\n");
    tk::write_file(three / "lib" / "libx.la", "libdir='D:/a/x/lib'\n");
    j = check(home, three, "x86_64", rc);
    EXPECT_EQ(rc, 1) << j.dump();
    EXPECT_EQ(j["P"].value("files", -1), 3) << j.dump();
    std::set<std::string> named;
    for (const auto& e : j["P"]["examples"]) named.insert(e.value("path", ""));
    EXPECT_EQ(named, (std::set<std::string>{"/srv/ci/work/lib", "/tmp/build-1/x", "D:/a/x/lib"})) << j.dump();
    EXPECT_TRUE(j["M"].empty());

    // The text form names the rule; a bad call is 2.
    const auto text = home.xlings({"__payload-check", one.string(), "--arch", "aarch64"});
    EXPECT_EQ(text.exit_code, 1);
    EXPECT_NE(text.transcript().find("rule M"), std::string::npos) << text.transcript();
    EXPECT_EQ(home.xlings({"__payload-check", one.string(), "--arch", "pdp11"}).exit_code, 2);
    EXPECT_EQ(home.xlings({"__payload-check", (home.root() / "absent").string()}).exit_code, 2);
}
