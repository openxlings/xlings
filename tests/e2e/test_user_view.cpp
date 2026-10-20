// A regular user's xlings inside a Luban system is a view of the system's
// store (design 2026-10-20 §4): the system's index and packages are read, not
// copied; what the system provides is not installed again; a version the store
// lacks goes into it once, as the administrator, through the one door; and
// with nobody to ask the exact command is given instead.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.platform;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

// The system's home, written by root and read by its users: read-only here.
struct ReadOnly {
    fs::path dir;
    explicit ReadOnly(fs::path d) : dir(std::move(d)) { set(false); }
    ~ReadOnly() { set(true); }
    void set(bool writable) {
        fs::permissions(dir, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                        writable ? fs::perm_options::add : fs::perm_options::remove);
    }
};

// What `sudo` does for the test: the system's home writable while the
// command runs, as its owner, with the environment root's xlings would have.
fs::path fake_sudo(const tk::Home& home, const fs::path& system) {
    const auto dir = home.root() / "fake-sudo";
    tk::write_file(dir / "sudo", std::format(R"(#!/bin/sh
[ "$1" = "-n" ] && shift
[ "$1" = "--" ] && shift
echo "sudo $*" >> "{0}/sudo.log"
chmod u+w "{1}"
env -u XLINGS_SYSTEM_LAYER XLINGS_HOME="{1}" "$@"
rc=$?
chmod u-w "{1}"
exit $rc
)", dir.string(), system.string()));
    fs::permissions(dir / "sudo", fs::perms::owner_all);
    return dir;
}

}  // namespace

XTEST(UserView, AUsersViewReadsTheSystemsStoreAndAsksOnceForWhatItLacks, .area = "subos",
      .covers = {"LUBAN-USER-VIEW"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "Luban systems are Linux";
    if (xlings::platform::is_root()) GTEST_SKIP() << "root writes every home";
    auto home = tk::Home::isolated("user-view");
    // The system's home where a real one is visible to a sandboxed recipe
    // worker (a sandbox has its own /tmp): outside /tmp, its index a copy.
    struct Away {
        fs::path dir = fs::path("/var/tmp") / std::format("xtest-user-view-{}", xlings::platform::get_pid());
        ~Away() {
            std::error_code ec;
            for (auto& e : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec))
                fs::permissions(e.path(), fs::perms::owner_all, fs::perm_options::add, ec);
            fs::permissions(dir, fs::perms::owner_all, fs::perm_options::add, ec);
            fs::remove_all(dir, ec);
        }
    } away;
    const auto system = away.dir / "system";
    fs::create_directories(system);
    fs::copy_file(home.dir() / ".xlings.json", system / ".xlings.json");
    const std::map<std::string, std::string> asSystem{{"XLINGS_HOME", system.string()}};
    auto r = home.xlings({"self", "init"}, asSystem);
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    r = home.xlings({"install", "fixture-tool", "-y"}, asSystem);
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    {
        // The index as a system carries it: its own tree, not a link.
        const auto index = system / "data/xim-pkgindex";
        const auto source = fs::canonical(index);
        fs::remove(index);
        fs::copy(source, index, fs::copy_options::recursive);
    }
    // A system's home, as a Luban root declares it: shared by its users.
    auto marker = Json::parse(tk::read_file(system / ".xlings-home"));
    marker["mode"] = "root";
    marker["layout"] = "multi";
    marker["root_layout"] = "multi";
    tk::write_file(system / ".xlings-home", marker.dump());
    // The user has nothing of their own yet: the view is made on first write.
    fs::remove_all(home.dir());

    ReadOnly guard(system);
    auto inSystem = asSystem;
    inSystem["XLINGS_ROOT_INSTANCE"] = "default";

    // What the system provides is the user's already: said, not installed.
    r = home.xlings({"install", "fixture-tool"}, inSystem);
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_NE(r.transcript().find("fixture-tool 1.0.0 is provided by the system"), std::string::npos) << r.transcript();
    EXPECT_FALSE(fs::exists(home.dir() / "data/xpkgs/xim-x-fixture-tool")) << "nothing installed in the view";
    EXPECT_FALSE(fs::exists(home.dir() / "data/xim-pkgindex")) << "the view has no index of its own";

    // The system's packages are in the user's list, marked.
    r = home.xlings({"list"}, inSystem);
    EXPECT_NE(r.out.find("xim:fixture-tool@1.0.0"), std::string::npos) << r.transcript();
    EXPECT_NE(r.out.find("system"), std::string::npos) << r.transcript();

    // Its versions are the user's to see and choose (G15).
    r = home.xlings({"use", "fixture-tool"}, inSystem);
    EXPECT_NE(r.transcript().find("1.0.0"), std::string::npos) << r.transcript();
    EXPECT_NE(r.transcript().find("system"), std::string::npos) << r.transcript();

    // Not in the store, and nobody to ask: the exact command, nothing done.
    auto agent = inSystem;
    agent["XLINGS_AGENT_MODE"] = "1";
    agent["PATH"] = "/usr/bin:/bin";
    r = home.xlings({"install", "fixture-data", "-y"}, agent);
    EXPECT_NE(r.exit_code, 0) << r.transcript();
    EXPECT_NE(r.transcript().find("sudo xlings install xim:fixture-data@1.0.0"), std::string::npos) << r.transcript();
    EXPECT_FALSE(fs::exists(system / "data/xpkgs/xim-x-fixture-data"));

    // At a terminal (here -y): into the store once, through sudo; the view
    // borrows it and installs nothing itself.
    const auto sudo = fake_sudo(home, system);
    auto asked = inSystem;
    asked["PATH"] = sudo.string() + ":/usr/bin:/bin";
    r = home.xlings({"install", "fixture-data", "-y"}, asked);
    EXPECT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_NE(tk::read_file(sudo / "sudo.log").find("__store-add xim:fixture-data@1.0.0"), std::string::npos)
        << tk::read_file(sudo / "sudo.log");
    EXPECT_TRUE(fs::exists(system / "data/xpkgs/xim-x-fixture-data/1.0.0")) << r.transcript();
    EXPECT_FALSE(fs::exists(home.dir() / "data/xpkgs/xim-x-fixture-data/1.0.0")) << "borrowed, not copied";

    // The system's own is root's to remove, for everyone: said.
    r = home.xlings({"remove", "fixture-tool", "-y"}, inSystem);
    EXPECT_NE(r.transcript().find("sudo xlings remove fixture-tool"), std::string::npos) << r.transcript();

    // Choosing the system's version is the view's own choice: root removing
    // it keeps the payload while a user has chosen it, and says who (§4.7).
    r = home.xlings({"use", "fixture-tool", "1.0.0"}, inSystem);
    EXPECT_EQ(r.exit_code, 0) << r.transcript();
    const auto passwd = home.root() / "passwd";
    tk::write_file(passwd, std::format("root:x:0:0::/root:/bin/sh\nalice:x:1000:1000::{}:/bin/sh\n", home.root().string()));
    guard.set(true);
    r = home.xlings({"remove", "fixture-tool", "-y"}, {{"XLINGS_HOME", system.string()}, {"XLINGS_ETC_PASSWD", passwd.string()}});
    guard.set(false);
    EXPECT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_NE(r.transcript().find("alice"), std::string::npos) << r.transcript();
    EXPECT_TRUE(fs::exists(system / "data/xpkgs/xim-x-fixture-tool/1.0.0/bin")) << "a user's choice keeps it";

    // Removing in the view takes back the choice only: the store keeps it.
    r = home.xlings({"remove", "fixture-tool", "-y"}, inSystem);
    EXPECT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_TRUE(fs::exists(system / "data/xpkgs/xim-x-fixture-tool/1.0.0/bin")) << "the store is root's";
}
