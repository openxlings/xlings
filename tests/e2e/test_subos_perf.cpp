// Release budgets from Part 1 §21.1 and Part 2 §15. Candidate and baseline
// use the same blocking fork/exec/wait runner; testkit's polling is not timed.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.core.elfread;
import xlings.subos.caps;
import xlings.subos.ports;
import xlings.subos.spec;
import xlings.confine.provider;
import xlings.confine;
import xlings.subos.policy;
import xlings.subos.home_view;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace p = xlings::platform;
namespace subos = xlings::subos;
using Json = nlohmann::json;

namespace {

bool static_candidate() {
    const auto elf = xlings::elfread::read(tk::xlings_binary());
    return elf && elf->interpreter.empty();
}

struct Box {
    tk::Home home = tk::Home::isolated("perf");
    Box() {
        if (!home.seed_sandbox_backend()) throw std::runtime_error("no real sandbox backend");
        const auto created = home.xlings({"subos", "new", "box"});
        if (created.exit_code != 0) throw std::runtime_error(created.transcript());
        fs::create_directories(home.dir() / "bin");
        fs::copy_file(tk::xlings_binary(), home.dir() / "bin/xlings");
    }
    ~Box() { (void)home.xlings({"subos", "stop", "box"}); }
    bool running() const { return fs::exists(home.dir() / "run/subos/box/session.json"); }
    std::vector<std::string> exec(std::string name, std::vector<std::string> command) const {
        std::vector<std::string> argv{tk::xlings_binary().string(), "subos", "exec", std::move(name), "--sandbox=bwrap", "--"};
        argv.insert(argv.end(), command.begin(), command.end());
        return argv;
    }
};

struct Environment {
    std::map<std::string, std::string> previous = tk::inherited_env();
    std::map<std::string, std::string> applied;
    fs::path cwd = fs::current_path();
    Environment(const std::map<std::string, std::string>& values, const fs::path& directory)
        : applied(values) {
        fs::current_path(directory);
        for (const auto& [name, value] : previous) p::unset_env_variable(name);
        for (const auto& [name, value] : applied) p::set_env_variable(name, value);
    }
    ~Environment() {
        for (const auto& [name, value] : applied) p::unset_env_variable(name);
        for (const auto& [name, value] : previous) p::set_env_variable(name, value);
        std::error_code ignored;
        fs::current_path(cwd, ignored);
    }
};

long long measured(const std::vector<std::string>& argv,
                   const std::map<std::string, std::string>& environment, const fs::path& cwd) {
    Environment restored{environment, cwd};
    const auto started = std::chrono::steady_clock::now();
    const int result = p::run_argv(argv);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
    if (result != 0) throw std::runtime_error("performance command failed: " + Json(argv).dump() + " exit=" + std::to_string(result));
    return elapsed;
}

struct Samples {
    std::vector<long long> candidate, baseline, overhead;
    long long median() const {
        auto sorted = overhead;
        std::ranges::sort(sorted);
        return sorted.at(sorted.size() / 2);
    }
    void print(std::string_view label, long long budget) const {
        std::cout << Json{{"perf", label}, {"candidate_us", candidate}, {"baseline_us", baseline},
                         {"paired_overhead_us", overhead}, {"median_overhead_us", median()},
                         {"budget_us", budget}}.dump() << '\n';
    }
};

Samples paired(const std::vector<std::string>& candidate, const std::vector<std::string>& baseline,
               const std::map<std::string, std::string>& candidateEnv,
               const std::map<std::string, std::string>& baselineEnv, const fs::path& cwd) {
    for (int warm = 0; warm != 3; ++warm) {
        (void)measured(baseline, baselineEnv, cwd);
        (void)measured(candidate, candidateEnv, cwd);
    }
    Samples samples;
    for (int sample = 0; sample != 11; ++sample) {
        long long actual{}, direct{};
        if (sample % 2 == 0) {
            direct = measured(baseline, baselineEnv, cwd);
            actual = measured(candidate, candidateEnv, cwd);
        } else {
            actual = measured(candidate, candidateEnv, cwd);
            direct = measured(baseline, baselineEnv, cwd);
        }
        samples.candidate.push_back(actual);
        samples.baseline.push_back(direct);
        samples.overhead.push_back(actual - direct);
    }
    return samples;
}

struct DirectBackend {
    std::vector<std::string> argv;
    std::map<std::string, std::string> env;
};

// The pre-supervisor execution shape, generated from the same view policy,
// mounts and namespaces. This is an equivalent direct-provider baseline,
// not a claim to have measured an historical release artifact.
DirectBackend direct_backend(const Box& box, const std::vector<std::string>& command) {
    const subos::HomeView home{box.home.dir()};
    const auto environment = box.home.env();
    const auto caps = subos::caps::probe(home, subos::Ports{});
    subos::spec::Request request{
        .instance = "box", .instance_dir = home.instance("box"), .user = environment.at("USER"),
        .argv = command, .preferred = subos::spec::Backend::Bwrap, .host_env = environment};
    const auto compiled = xlings::confine::compile(subos::policy::legacy(), home, caps, request);
    if (!compiled) throw std::runtime_error("direct-provider baseline did not compile");
    if (compiled->backend != subos::spec::Backend::Bwrap || compiled->mounts.empty() || !compiled->unshare_pid)
        throw std::runtime_error("direct-provider baseline omitted the sandbox");
    return {xlings::confine::provider::bwrap_argv(*compiled), xlings::confine::provider::process_env(*compiled, environment)};
}

} // namespace

XTEST(SubosPerf, AShimInsideASandboxDispatchesWithoutTheBroker,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"PERF-SHIM"},
      .requires_ = {"linux", "xlings-bin", "bwrap"}, .resources = {"sandbox"}, .proves = "isolation") {
    if (!static_candidate()) GTEST_SKIP() << "release performance requires a static candidate";
    Box box;
    fs::create_directories(box.home.dir() / "subos/box/bin");
    fs::create_symlink("../../../bin/xlings", box.home.dir() / "subos/box/bin/true");
    const auto script = "shim_path=" + p::shell_quote((box.home.dir() / "subos/box/bin/true").string()) + R"SH(
        printf '%s\n' "$shim_path"
        shim() { i=0; while [ "$i" -lt 40 ]; do "$shim_path" || exit; i=$((i+1)); done; }
        direct() { i=0; while [ "$i" -lt 40 ]; do /bin/true || exit; i=$((i+1)); done; }
        sample=0
        while [ "$sample" -lt 7 ]; do
            a=$(date +%s%N)
            if [ $((sample % 2)) -eq 0 ]; then
                shim; b=$(date +%s%N); direct; c=$(date +%s%N)
                echo $(((b-a)/40/1000)) $(((c-b)/40/1000))
            else
                direct; b=$(date +%s%N); shim; c=$(date +%s%N)
                echo $(((c-b)/40/1000)) $(((b-a)/40/1000))
            fi
            sample=$((sample+1))
        done
    )SH";
    const auto result = box.home.xlings({"subos", "exec", "box", "--sandbox=bwrap", "--", "/bin/sh", "-c", script});
    ASSERT_EQ(result.exit_code, 0) << result.transcript();
    // `true` may be a shell builtin; the explicit shim path above guarantees
    // every measured shim invocation executes the candidate dispatcher.
    std::istringstream lines(result.out);
    std::string line;
    ASSERT_TRUE(std::getline(lines, line));
    EXPECT_EQ(line, (box.home.dir() / "subos/box/bin/true").string());
    Samples samples;
    while (std::getline(lines, line)) {
        std::istringstream values(line);
        long long shim{}, direct{};
        ASSERT_TRUE(values >> shim >> direct) << line;
        samples.candidate.push_back(shim); samples.baseline.push_back(direct);
        samples.overhead.push_back(shim - direct);
    }
    ASSERT_EQ(samples.overhead.size(), 7u) << result.transcript();
    samples.print("shim_vs_direct_inside_bwrap", 10'000);
    EXPECT_LT(samples.median(), 10'000);
    const auto events = tk::read_file(box.home.dir() / "logs/subos/box/events.ndjson");
    EXPECT_EQ(events.find("\"kind\":\"perm\""), std::string::npos) << events;
}

XTEST(SubosPerf, JoiningARunningSessionAddsAtMostFiveMilliseconds,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"PERF-EXEC-HOT"},
      .requires_ = {"linux", "xlings-bin", "bwrap"}, .resources = {"sandbox"}, .proves = "isolation") {
    if (!static_candidate()) GTEST_SKIP() << "release performance requires a static candidate";
    Box box;
    const auto started = box.home.xlings({"subos", "start", "box", "--sandbox=bwrap", "--ttl", "60s"});
    ASSERT_EQ(started.exit_code, 0) << started.transcript();
    ASSERT_TRUE(box.running());
    const auto session = tk::read_file(box.home.dir() / "run/subos/box/session.json");
    Samples samples;
    ASSERT_NO_THROW(samples = paired(box.exec("box", {"/bin/true"}), {"/bin/true"},
                                    box.home.env(), box.home.env(), box.home.root()));
    samples.print("hot_exec_vs_direct_exec", 5'000);
    EXPECT_LE(samples.median(), 5'000);
    EXPECT_EQ(tk::read_file(box.home.dir() / "run/subos/box/session.json"), session);
}

XTEST(SubosPerf, ColdExecAddsAtMostTenMillisecondsToTheDirectProvider,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"PERF-EXEC-COLD"},
      .requires_ = {"linux", "xlings-bin", "bwrap"}, .resources = {"sandbox"}, .proves = "isolation") {
    if (!static_candidate()) GTEST_SKIP() << "release performance requires a static candidate";
    Box box;
    const auto environment = box.home.env();
    ASSERT_NO_THROW((void)measured(box.exec("box", {"/bin/true"}), environment, box.home.root()));
    ASSERT_FALSE(box.running()) << "cold samples must not join a previous session";
    DirectBackend direct;
    ASSERT_NO_THROW(direct = direct_backend(box, {"/bin/true"}));
    Samples samples;
    ASSERT_NO_THROW(samples = paired(box.exec("box", {"/bin/true"}), direct.argv,
                                    environment, direct.env, box.home.root()));
    ASSERT_FALSE(box.running());
    samples.print("cold_exec_vs_equivalent_direct_provider", 10'000);
    EXPECT_LE(samples.median(), 10'000);
}

XTEST(SubosPerf, RootEntryWithACheckedPayloadClosureAddsAtMostOneHundredMilliseconds,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"PERF-ROOT-ENTRY"},
      .requires_ = {"linux", "xlings-bin", "bwrap"}, .resources = {"sandbox"}, .proves = "isolation") {
    if (!static_candidate()) GTEST_SKIP() << "release performance requires a static candidate";
    Box box;
    const auto created = box.home.xlings({"subos", "new", "root-perf", "--rootfs"});
    ASSERT_EQ(created.exit_code, 0) << created.transcript();
    const auto payload = box.home.dir() / "data/xpkgs/fixture-x-xlings/1.0.0";
    fs::create_directories(payload / "bin");
    fs::copy_file(tk::xlings_binary(), payload / "bin/xlings");
    tk::write_file(payload / ".xlings-resolution.json", R"({"package":"fixture:xlings@1.0.0","deps":[]})");
    auto primary = Json::parse(tk::read_file(box.home.dir() / ".xlings.json"));
    primary["versions"]["xlings"] = {{"type", "program"},
        {"versions", {{"fixture:1.0.0", {{"path", (payload / "bin").string()}}}}}};
    primary.erase("dbIndex");
    tk::write_file(box.home.dir() / ".xlings.json", primary.dump());
    fs::remove(box.home.dir() / "data/versions.json");
    auto workspace = Json::parse(tk::read_file(box.home.dir() / "subos/root-perf/.xlings.json"));
    workspace["workspace"]["xlings"] = {{"active", "fixture:1.0.0"}, {"installed", {"fixture:1.0.0"}}};
    tk::write_file(box.home.dir() / "subos/root-perf/.xlings.json", workspace.dump());
    Samples samples;
    ASSERT_NO_THROW(samples = paired(box.exec("root-perf", {"/usr/bin/xlings", "--version"}),
                                    box.exec("box", {(payload / "bin/xlings").string(), "--version"}),
                                    box.home.env(), box.home.env(), box.home.root()));
    samples.print("root_exact_closure_vs_view_cold_exec", 100'000);
    EXPECT_LE(samples.median(), 100'000);
    EXPECT_TRUE(fs::is_symlink(box.home.dir() / "subos/root-perf/root/usr/bin/xlings"));
    EXPECT_FALSE(fs::exists(box.home.dir() / "run/subos/root-perf/session.json"));
}
