// One default runtime per environment (Luban OS part 3 §4): a home's runtime
// indirection points at the pinned or newest runtime it has, switches with
// one rename, and says what changed and why.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.core.abi;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace rt = xlings::abi;

namespace {
void glibc(const fs::path& home, std::string_view version) {
    const auto lib = home / "data/xpkgs/xim-x-glibc" / version / "lib64";
    tk::write_file(lib / "ld-linux-x86-64.so.2", std::format("loader {}", version));
    tk::write_file(lib / "libc.so.6", std::format("libc {}", version));
    tk::write_file(lib / "libm.so.6", std::format("libm {}", version));
}
void gcc_runtime(const fs::path& home, std::string_view version) {
    const auto lib = home / "data/xpkgs/xim-x-gcc-runtime" / version / "lib64";
    tk::write_file(lib / "libstdc++.so.6", std::format("libstdc++ {}", version));
    tk::write_file(lib / "libgcc_s.so.1", std::format("libgcc_s {}", version));
}
std::string through(const fs::path& home, std::string_view file) {
    return tk::read_file(home / "abi/linux-x86_64-glibc" / file);
}
}  // namespace

XTEST(Abi, AHomesRuntimeIsItsNewestOrItsPinAndSwitchesWholeWithOneRename,
      .area = "xim", .covers = {"RUNTIME-SCOPE", "RUNTIME-PIN"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "the runtime indirection is Linux's";
    auto home = tk::Home::isolated("abi-home");
    const auto h = home.dir();
    // Nothing installed: an empty generation, so a root can mount over it.
    auto first = rt::refresh(h, "first");
    ASSERT_TRUE(first) << first.error();
    EXPECT_TRUE(fs::is_symlink(h / "abi"));

    glibc(h, "2.39");
    glibc(h, "2.44.3");
    gcc_runtime(h, "15.1.0");
    auto moved = rt::refresh(h, "install xz");
    ASSERT_TRUE(moved) << moved.error();
    EXPECT_TRUE(moved->changed);
    EXPECT_EQ(through(h, "libc.so.6"), "libc 2.44.3") << "the newest installed";
    EXPECT_EQ(through(h, "ld-linux-x86-64.so.2"), "loader 2.44.3") << "loader and libc from one payload";
    EXPECT_EQ(through(h, "libstdc++.so.6"), "libstdc++ 15.1.0") << "the C++ runtime beside it";
    ASSERT_EQ(moved->what.size(), 1u);
    EXPECT_NE(moved->what.front().find("2.44.3"), std::string::npos) << moved->what.front();

    // Asked again with nothing new: nothing written.
    auto same = rt::refresh(h, "again");
    ASSERT_TRUE(same);
    EXPECT_FALSE(same->changed);

    // A pin, and back to the newest.
    auto doc = nlohmann::json::parse(tk::read_file(h / ".xlings.json"), nullptr, false);
    if (!doc.is_object()) doc = nlohmann::json::object();
    doc["runtime"] = {{"glibc", "2.39"}};
    tk::write_file(h / ".xlings.json", doc.dump());
    ASSERT_TRUE(rt::refresh(h, "pin"));
    EXPECT_EQ(through(h, "libc.so.6"), "libc 2.39");
    doc.erase("runtime");
    tk::write_file(h / ".xlings.json", doc.dump());
    ASSERT_TRUE(rt::refresh(h, "unpin"));
    EXPECT_EQ(through(h, "libc.so.6"), "libc 2.44.3");

    // What it is and how it got there: the state file and the log.
    const auto state = nlohmann::json::parse(tk::read_file(h / "abi.json"));
    EXPECT_EQ(state["families"]["linux-x86_64-glibc"]["libc"], "glibc@2.44.3");
    const auto log = tk::read_file(h / "logs/abi.ndjson");
    EXPECT_NE(log.find("\"reason\":\"pin\""), std::string::npos) << log;
    EXPECT_NE(log.find("\"reason\":\"unpin\""), std::string::npos) << log;
    // Old generations are kept for a rollback by pin, three of them.
    std::size_t generations = 0;
    for (auto& e : fs::directory_iterator(h / "abi.gen")) generations += e.is_directory() ? 1 : 0;
    EXPECT_LE(generations, 3u);
}

XTEST(Abi, AConsumerIsBoundToTheIndirectionNotToAPayload, .area = "xim", .covers = {"RUNTIME-SCOPE"}) {
    EXPECT_EQ(rt::loader_path("/xlings", "linux-x86_64-glibc", "/x/xim-x-glibc/2.44.3/lib64/ld-linux-x86-64.so.2"),
              fs::path("/xlings/abi/linux-x86_64-glibc/ld-linux-x86-64.so.2"));
    EXPECT_TRUE(rt::is_runtime("glibc"));
    EXPECT_TRUE(rt::is_runtime("gcc-runtime"));
    EXPECT_FALSE(rt::is_runtime("openssl")) << "an ordinary library stays bound by its payload";
}
