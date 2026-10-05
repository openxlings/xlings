// Performance budgets as assertions (design §21.1, C25). Generous on
// purpose -- they run on shared CI machines and against a debug build -- so
// what they catch is a change of KIND: a shim that starts going through the
// supervisor or the broker, a join that starts a sandbox.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

namespace {

struct Box {
    tk::Home home = tk::Home::isolated("perf");
    Box() {
        home.seed_sandbox_backend();
        auto r = home.xlings({"subos", "new", "box"});
        if (r.exit_code != 0) ADD_FAILURE() << r.transcript();
    }
    bool running() const { return fs::exists(home.dir() / "run" / "subos" / "box" / "session.json"); }
};

std::chrono::milliseconds time_of(const std::function<void()>& f) {
    const auto t0 = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
}

}  // namespace

XTEST(SubosPerf, AShimInsideASandboxDispatchesWithoutTheBroker,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"PERF-SHIM"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"}) {
    Box box;
    // The home's entry and a shim for `uname` in the instance's bin: with no
    // version active, the shim hands the name back to PATH -- the whole
    // dispatch path, and nothing that needs the owner.
    fs::create_directories(box.home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), box.home.dir() / "bin" / "xlings");
    fs::create_directories(box.home.dir() / "subos" / "box" / "bin");
    fs::create_symlink("../../../bin/xlings", box.home.dir() / "subos" / "box" / "bin" / "uname");
    const int n = 40;
    auto r = box.home.xlings({"subos", "exec", "box", "--sandbox", "--", "/bin/sh", "-c", std::format(
        "command -v uname; "
        "a=$(date +%s%N); i=0; while [ $i -lt {0} ]; do uname >/dev/null; i=$((i+1)); done; "
        "b=$(date +%s%N); i=0; while [ $i -lt {0} ]; do /bin/uname >/dev/null; i=$((i+1)); done; "
        "c=$(date +%s%N); echo shim_us=$(( (b-a)/{0}/1000 )) direct_us=$(( (c-b)/{0}/1000 ))", n)});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_NE(r.out.find("/subos/box/bin/uname"), std::string::npos) << "the shim was not what ran\n" << r.out;
    const auto at = r.out.find("shim_us=");
    ASSERT_NE(at, std::string::npos) << r.out;
    long shim = 0, direct = 0;
    std::sscanf(r.out.c_str() + at, "shim_us=%ld direct_us=%ld", &shim, &direct);
    // Budget: the dispatch costs what it costs outside (single-digit ms for a
    // release build); 50 ms per call for a debug build on a shared runner
    // still fails a round trip to a broker that starts an xlings outside.
    EXPECT_LT(shim - direct, 50'000) << "per-dispatch overhead " << (shim - direct) << " us\n" << r.out;
    // And none of it reached the owner.
    const auto events = tk::read_file(box.home.dir() / "logs" / "subos" / "box" / "events.ndjson");
    EXPECT_EQ(events.find("\"kind\":\"perm\""), std::string::npos) << events;
}

XTEST(SubosPerf, JoiningARunningSessionIsCheaperThanStartingOne,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"PERF-EXEC-HOT"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"}) {
    Box box;
    const int n = 5;
    std::chrono::milliseconds cold{0}, hot{0};
    for (int i = 0; i < n; ++i)
        cold += time_of([&] { EXPECT_EQ(box.home.xlings({"subos", "exec", "box", "--sandbox", "--", "true"}).exit_code, 0); });
    ASSERT_EQ(box.home.xlings({"subos", "start", "box"}).exit_code, 0);
    ASSERT_TRUE(box.running());
    for (int i = 0; i < n; ++i)
        hot += time_of([&] { EXPECT_EQ(box.home.xlings({"subos", "exec", "box", "--", "true"}).exit_code, 0); });
    (void)box.home.xlings({"subos", "stop", "box"});
    // A join is a socket and fd passing; a cold start is a sandbox.
    EXPECT_LT(hot.count(), cold.count()) << "hot " << hot.count() / n << " ms, cold " << cold.count() / n << " ms";
}
