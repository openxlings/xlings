// What xdesk costs (design 2026-10-20 §13): the overview toggled ten times,
// and a still desktop, measured in CPU time -- xdesk's and the window's
// together -- on the lane that has the machine to itself (`cpu`), with the
// software renderer CI has. A miss says what was measured and the budget.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
import std;
import xlings.libs.json;
import xlings.platform;

namespace tk = xlings::testkit;
namespace platform = xlings::platform;
namespace fs = std::filesystem;
using Json = nlohmann::json;

#include "luban_os_support.hpp"
#include "xdesk_support.hpp"

using namespace luban_os_e2e;

namespace {

// `script` run as xdesk_with does, under bash's `times`: the CPU seconds of
// the window and everything it waited for (xdesk included).
struct Measured { tk::RunResult run; double cpu { -1 }; };
Measured timed_(const tk::Home& home, std::string_view profile, std::string_view script) {
    const auto runtime = home.root() / "run";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    const auto client = colour_client(home).string();
    const auto file = home.root() / "script.txt";
    tk::write_file(file, script);
    tk::RunOptions o;
    // XDESK_PERF_RECORD=<file>: the whole run under `perf record -g` (a person profiling).
    const char* record = std::getenv("XDESK_PERF_RECORD");
    o.argv = {"bash", "-c", record && *record ? std::format(R"(perf record -q -g -o {} -- "$@" >/dev/null 2>&1; rc=$?; times >&2; exit $rc)", record)
                                              : std::string(R"("$@" >/dev/null 2>&1; rc=$?; times >&2; exit $rc)"), "_",
              app_binary()->string(), "--headless", "--script", file.string(),
              "--desktop-exec", xdesk_binary()->string(), "--profile", std::string(profile),
              "--autostart", std::format("python3 {} c04040", client),
              "--autostart", std::format("sh -c 'sleep 0.6; exec python3 {} 40c040'", client),
              "--autostart", std::format("sh -c 'sleep 1.2; exec python3 {} 4040c0'", client)};
    o.env = home.env();
    o.env["LUBAN_XLINGS"] = tk::xlings_binary().string();
    o.env["SDL_VIDEODRIVER"] = "offscreen";
    o.env["SDL_RENDER_DRIVER"] = "software";
    o.env["XDG_RUNTIME_DIR"] = runtime.string();
    o.env["XDESK_RENDERER"] = "pixman";   // the software path: CI's, and every budget's floor
    o.timeout = std::chrono::seconds(240);
    Measured m{tk::run(o)};
    // `times`: the shell's line, then its children's: "0m1.234s 0m0.567s".
    std::istringstream in(m.run.err);
    std::string line, last;
    while (std::getline(in, line))
        if (line.find("m") != std::string::npos && line.find("s ") != std::string::npos) last = line;
    double total = 0;
    std::istringstream fields(last);
    for (std::string f; fields >> f;) {
        const auto mpos = f.find('m');
        if (mpos == std::string::npos) continue;
        total += std::stod(f.substr(0, mpos)) * 60 + std::stod(f.substr(mpos + 1, f.size() - mpos - 2));
    }
    if (!last.empty()) m.cpu = total;
    return m;
}

}  // namespace

XTEST(XdeskPerf, TheOverviewTenTimesStaysWithinItsCpuBudget, .area = "luban", .cost = tk::Cost::Medium,
      .covers = {"XDESK-PERF"}, .requires_ = {"linux", "xlings-bin"}, .resources = {"cpu"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("xdesk-perf-overview");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    // The same session twice: once still, once with the overview ten times;
    // the difference is what the overview cost.
    const std::string settle = "wait desktop\nwaitfor desktop.texture.0 1100\nsleep 3000\n";
    std::string toggles = settle;
    for (int k = 0; k < 10; ++k) toggles += "key Super+o\nsleep 400\n";
    std::string still = settle;
    for (int k = 0; k < 10; ++k) still += "sleep 400\n";
    const auto a = timed_(home, "canvas", still);
    ASSERT_EQ(a.run.exit_code, 0) << a.run.transcript();
    const auto b = timed_(home, "canvas", toggles);
    ASSERT_EQ(b.run.exit_code, 0) << b.run.transcript();
    ASSERT_GE(a.cpu, 0) << a.run.err;
    ASSERT_GE(b.cpu, 0) << b.run.err;
    const double overview = b.cpu - a.cpu;
    std::println(stderr, "xdesk perf: a still session {:.2f} s of CPU, with the overview ten times {:.2f} s: {:.2f} s for the overview",
                 a.cpu, b.cpu, overview);
    EXPECT_LE(overview, 2.0) << "the overview ten times took " << overview << " s of CPU (budget 2 s)";
    EXPECT_LE(a.cpu, 6.0) << "a still session of 7 s took " << a.cpu << " s of CPU";
}

// A still desktop draws nothing, and a minimized window is sent nothing --
// however much changes meanwhile; restored, all of it at once (design
// 2026-10-20 §13.2, §13.3).
XTEST(XdeskPerf, AStillDesktopSendsNoFramesAndAHiddenWindowNone, .area = "luban", .cost = tk::Cost::Medium,
      .covers = {"XDESK-PERF"}, .requires_ = {"linux", "xlings-bin"}, .resources = {"cpu"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("xdesk-perf-still");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto a = home.root() / "a.json", b = home.root() / "b.json", c = home.root() / "c.json", d = home.root() / "d.json";
    // After the first session's hint has gone (8 s): then nothing changes.
    // Minimized, a second for it to reach xdesk (a frame already on its way
    // on a slow machine is not one sent to a hidden window).
    auto r = xdesk_with(home, "canvas", std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 9000
state {}
sleep 2000
state {}
minimize
sleep 1000
state {}
key Super+o
sleep 1500
state {}
restore
sleep 1500
state {}
)", a.string(), b.string(), c.string(), d.string(), (home.root() / "e.json").string()));
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto frames = [](const fs::path& f) { return read_state(f)["perf"]["desktop_frames"].get<int>(); };
    // At most one: the panel's clock may turn its minute in those two seconds.
    EXPECT_LE(frames(b) - frames(a), 1) << "a still desktop sent frames";
    EXPECT_EQ(frames(d), frames(c)) << "a minimized window was sent frames (the overview opened meanwhile)";
    EXPECT_GT(frames(home.root() / "e.json"), frames(d)) << "restored: the desktop as it is now";
}
