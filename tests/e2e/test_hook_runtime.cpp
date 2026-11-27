// #724 on Linux (design 2026-11-21-issue-724 §7): the Luban host machine's
// root -- the carrier's own image, written by this xlings: a static xlings,
// busybox and bwrap, nothing of this host -- made here and entered with
// bwrap. A recipe whose install hook starts a program it was given (PT_INTERP
// the standard path, not rewritten yet, as bun's postinstall did) installs
// in it: with the root's own link, which the guest makes at its probe (a),
// and without it, in the worker that gives the hook the environment's
// runtime (b). What only the host machine had is in every PR's Linux CI.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.carrier.wsl2;
import xlings.platform.target;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

namespace {

fs::path fake_wsl() { return fs::path(__FILE__).parent_path().parent_path() / "fixtures/carrier/fake-wsl.sh"; }

std::string outer_bwrap() {
    for (const auto* env : {"XDEV_BWRAP"})
        if (const char* v = std::getenv(env); v && *v) return v;
    for (const auto* candidate : {"/usr/lib/xlings/bwrap", "/usr/bin/bwrap", "/usr/local/bin/bwrap"})
        if (fs::exists(candidate)) return candidate;
    return {};
}

std::optional<fs::path> find_in(const fs::path& dir, std::string_view name) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->path().filename() == name && it->is_regular_file(ec)) return it->path();
    return std::nullopt;
}

// The fixture: its install hook copies glibc's getconf, points its PT_INTERP
// at the standard path and runs it.
void write_fixture_index(const fs::path& dir, std::string_view standard) {
    tk::write_file(dir / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    tk::write_file(dir / "pkgs/h/hookrt.lua", std::format(R"LUA(
package = {{
    spec = "1", name = "hookrt", type = "package", archs = {{"x86_64", "aarch64"}},
    xpm = {{ linux = {{ deps = {{ runtime = {{ "xim:glibc" }} }}, ["1.0.0"] = {{}}, ["1.0.1"] = {{}} }} }},
}}
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.elfpatch")
function install()
    local dir = pkginfo.install_dir()
    os.mkdir(path.join(dir, "bin"))
    local program = path.join(dir, "bin", "hookrt-getconf")
    os.cp(path.join(pkginfo.dep_install_dir("glibc"), "bin", "getconf"), program)
    elfpatch.set_interpreter(program, "{}")
    os.exec(program .. " PAGESIZE")
    return true
end
)LUA", standard));
}

}  // namespace

XTEST(HookRuntime, AHostMachineLikeRootRunsAHooksDownloadedProgram, .area = "xim", .cost = tk::Cost::Slow,
      .covers = {"HOOK-RUNTIME-CONTRACT", "CARRIER-GUEST-CURRENT"},
      .requires_ = {"linux", "xlings-bin", "static-xlings", "bwrap", "userns", "network"}) {
    const bool arm = std::string_view(xlings::platform::build_arch()) != "x86_64";
    const std::string stdDir = arm ? "lib" : "lib64";
    const std::string standard = arm ? "/lib/ld-linux-aarch64.so.1" : "/lib64/ld-linux-x86-64.so.2";

    // The guest's static programs, from the index (as an image gets them).
    auto tools = tk::Home::isolated("hookrt-tools");
    auto fetched = tools.xlings({"install", "-y", "xim:busybox", "xim:bwrap"}, {}, std::chrono::minutes(10));
    ASSERT_EQ(fetched.exit_code, 0) << fetched.transcript();
    const auto busybox = find_in(tools.dir() / "data/xpkgs", "busybox");
    const auto bwrap = find_in(tools.dir() / "data/xpkgs", "bwrap");
    ASSERT_TRUE(busybox && bwrap) << fetched.transcript();

    // The host machine's root: the carrier image, imported by the stand-in
    // wsl.exe as a directory.
    auto home = tk::Home::isolated("hookrt-carrier");
    const auto wsl = home.root() / "wsl";
    fs::create_directories(wsl);
    const std::map<std::string, std::string> env{
        {"XLINGS_WSL_EXE", fake_wsl().string()},
        {"FAKE_WSL_ROOT", wsl.string()},
        {"XLINGS_CARRIER_GUEST_XLINGS", tk::xlings_binary().string()},
        {"XLINGS_CARRIER_GUEST_BUSYBOX", busybox->string()},
        {"XLINGS_CARRIER_GUEST_BWRAP", bwrap->string()},
    };
    auto made = home.xlings({"subos", "new", "box", "--carrier", "wsl2"}, env, std::chrono::minutes(5));
    ASSERT_EQ(made.exit_code, 0) << made.transcript();
    const auto root = wsl / "distros" / xlings::carrier::wsl2::distro_name(home.dir());
    // (a) Its probe linked the standard directory to the home's runtime, and
    // busybox's applets in the root (not in the root of the process that ran
    // the probe, which here is this host's).
    ASSERT_TRUE(fs::is_symlink(root / stdDir)) << made.transcript();
    EXPECT_EQ(fs::read_symlink(root / "bin/cp"), "/bin/busybox") << made.transcript();
    tk::write_file(root / "etc/resolv.conf", tk::read_file("/etc/resolv.conf"));
    write_fixture_index(root / "xlings/hmfix", standard);

    const auto bw = outer_bwrap();
    ASSERT_FALSE(bw.empty());
    const auto inside = [&](std::vector<std::string> argv) {
        std::vector<std::string> command{bw, "--unshare-user", "--uid", "0", "--gid", "0", "--bind", root.string(), "/",
                                         "--dev", "/dev", "--proc", "/proc", "--clearenv",
                                         "--setenv", "HOME", "/root", "--setenv", "XLINGS_HOME", "/xlings",
                                         "--setenv", "XLINGS_NON_INTERACTIVE", "1", "--setenv", "NO_COLOR", "1",
                                         "--setenv", "PATH", "/xlings/bin:/usr/bin:/bin:/usr/sbin:/sbin"};
        for (const auto* proxy : {"http_proxy", "https_proxy", "HTTP_PROXY", "HTTPS_PROXY", "no_proxy", "NO_PROXY"})
            if (const char* v = std::getenv(proxy); v && *v) command.insert(command.end(), {"--setenv", proxy, v});
        command.insert(command.end(), argv.begin(), argv.end());
        return tk::run({.argv = command, .timeout = std::chrono::minutes(15)});
    };
    // Nothing of this host in it.
    auto host = inside({"/bin/busybox", "sh", "-c", "test ! -e /usr/lib/x86_64-linux-gnu && test ! -e /etc/ld.so.cache && echo bare"});
    ASSERT_NE(host.out.find("bare"), std::string::npos) << host.transcript();

    ASSERT_EQ(inside({"/xlings/bin/xlings", "config", "--index-repo", "hmfix:/xlings/hmfix"}).exit_code, 0);
    auto linked = inside({"/xlings/bin/xlings", "install", "-y", "hmfix:hookrt@1.0.0"});
    EXPECT_EQ(linked.exit_code, 0) << linked.transcript();

    // (b) Without the link: the hook runs in a worker that gives it the
    // environment's runtime -- where a worker can start in this root
    // (nested user namespaces; the u-alpine host-matrix leg always can).
    fs::remove(root / stdDir);
    auto nested = inside({"/usr/bin/bwrap", "--ro-bind", "/", "/", "--", "/bin/busybox", "true"});
    if (nested.exit_code != 0) {
        std::println("a worker cannot start in this root here (nested bwrap: {}); (b) is the u-alpine leg's",
                     nested.transcript());
        return;
    }
    auto worker = inside({"/xlings/bin/xlings", "install", "-y", "hmfix:hookrt@1.0.1"});
    EXPECT_EQ(worker.exit_code, 0) << worker.transcript();
    EXPECT_FALSE(fs::exists(fs::symlink_status(root / stdDir))) << "the worker's runtime is the hook's, not the root's";
}
