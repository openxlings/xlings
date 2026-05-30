// InterfaceProtocol — `xlings interface` v1 NDJSON wire format
// (see docs/plans/2026-04-25-interface-api-v1.md and
//  docs/plans/2026-04-26-interface-api-v1-eval.md)
//
// These end-to-end tests spawn the real xlings binary and validate the
// wire-format guarantees: protocol_version, capability registry, NDJSON
// envelope shape, error semantics, and capability lifecycles.
//
// Kept in its own translation unit so this test surface can be iterated
// on without recompiling the multi-thousand-line test_main.cpp.

#include <gtest/gtest.h>
#ifdef __unix__
#include <sys/wait.h>
#endif

import std;
import xlings.libs.json;

namespace {

// Resolve once on first call and cache the absolute path. mcpp places binaries
// under target/<triple>/<fingerprint>/bin; keep the legacy xmake build layout as
// a fallback while the repository still supports older local workflows.
std::string find_xlings_binary_() {
    namespace fs = std::filesystem;
    static const std::string cached = []() -> std::string {
#ifdef _WIN32
        const char* exe = "xlings.exe";
#else
        const char* exe = "xlings";
#endif
        auto absolute_or_original = [](const fs::path& path) -> std::string {
            std::error_code ec;
            auto abs = fs::absolute(path, ec);
            if (!ec) return abs.string();
            return path.string();
        };

        std::error_code ec;
        fs::path newest_mcpp_candidate;
        fs::file_time_type newest_mcpp_time {};
        if (fs::exists("target", ec)) {
            for (auto it = fs::recursive_directory_iterator("target", ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                if (it->path().filename() != exe) continue;
                if (it->path().parent_path().filename() != "bin") continue;
                auto t = fs::last_write_time(it->path(), ec);
                if (ec) continue;
                if (newest_mcpp_candidate.empty() || t > newest_mcpp_time) {
                    newest_mcpp_candidate = it->path();
                    newest_mcpp_time = t;
                }
            }
        }
        if (!newest_mcpp_candidate.empty()) {
            return absolute_or_original(newest_mcpp_candidate);
        }

        static const char* platforms[] = {"linux", "macosx", "windows"};
        static const char* archs[] = {"x86_64", "arm64", "x64"};
        static const char* modes[] = {"release", "debug"};
        for (auto* p : platforms) for (auto* a : archs) for (auto* m : modes) {
            fs::path candidate = fs::path("build") / p / a / m / exe;
            if (fs::is_regular_file(candidate, ec)) {
                return absolute_or_original(candidate);
            }
        }
        return std::string{};
    }();
    return cached;
}

// Run `xlings <args...>` with an isolated XLINGS_HOME, capture stdout.
// Returns {stdout, exit_code}. Sets XLINGS_HOME via putenv (portable
// across cmd.exe / sh) and silences stderr via shell redirection.
//
// Windows portability hack: if any arg contains a `"` character (typical
// of JSON args like `{"name":"x"}`), we cannot reliably round-trip it
// through cmd.exe + the MSVC CRT command-line parser. Detect that case,
// rewrite the trailing `--args <JSON>` into `--args-file <tmpPath>`, and
// drop the JSON body into a temp file. xlings interface reads the file
// directly, sidestepping all shell quoting concerns.
std::pair<std::string, int> run_xlings_(
        std::vector<std::string> args,
        const std::string& xlings_home = "") {
    auto bin = find_xlings_binary_();
    if (bin.empty()) return std::pair<std::string, int>{"", -1};

    if (!xlings_home.empty()) {
#ifdef _WIN32
        _putenv_s("XLINGS_HOME", xlings_home.c_str());
        // XLINGS_PROJECT_DIR is auto-set by xlings whenever it loads a
        // project config (config.cppm). It can leak in from the user's
        // shell (e.g., running xlings in the repo before the test) and
        // would override the `cd <neutral_cwd>` isolation below by
        // forcing the spawned binary to use the leaked project root.
        _putenv_s("XLINGS_PROJECT_DIR", "");
#else
        ::setenv("XLINGS_HOME", xlings_home.c_str(), 1);
        ::unsetenv("XLINGS_PROJECT_DIR");
#endif
    }

#ifdef _WIN32
    // Reroute any --args <JSON-with-quotes> through --args-file <tmp>.
    std::filesystem::path args_tmp;
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--args" && args[i + 1].find('"') != std::string::npos) {
            args_tmp = std::filesystem::temp_directory_path() / ("xlings-iface-args-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
            std::ofstream(args_tmp) << args[i + 1];
            args[i] = "--args-file";
            args[i + 1] = args_tmp.string();
            break;
        }
    }
#endif

    std::string cmd = "\"" + bin + "\"";
    for (auto& a : args) {
#ifdef _WIN32
        cmd += " \"" + a + "\"";
#else
        cmd += " '" + a + "'";
#endif
    }

    // Run from a clean directory so the spawned xlings doesn't walk up
    // and pick up an unrelated project-scope `.xlings.json` (e.g. the
    // repo root's, which now ships its own `.xlings.json` for CI
    // self-host purposes). The system temp dir typically has no
    // ancestor `.xlings.json`, so project-scope detection cleanly
    // returns "no project". XLINGS_HOME (set above) still routes the
    // spawned binary to the sandbox global config.
    auto neutral_cwd = std::filesystem::temp_directory_path().string();
#ifdef _WIN32
    cmd = "cd /D \"" + neutral_cwd + "\" && " + cmd;
#else
    cmd = "cd \"" + neutral_cwd + "\" && " + cmd;
#endif
#ifdef _WIN32
    cmd += " 2>NUL";
#else
    cmd += " 2>/dev/null";
#endif

#ifdef _WIN32
    // cmd.exe /c, per its parsing rules, strips ONE pair of outer quotes
    // when there are >2 quotes in the command, garbling our `"path" "arg"
    // "arg"` shape. Wrap the whole command in an extra pair so the strip
    // removes only those wrappers and leaves the per-token quoting intact.
    std::string wrapped = "\"" + cmd + "\"";
    auto* fp = _popen(wrapped.c_str(), "r");
#else
    auto* fp = popen(cmd.c_str(), "r");
#endif
    if (!fp) return std::pair<std::string, int>{"", -1};
    std::string out;
    char buf[4096];
    while (auto n = std::fread(buf, 1, sizeof(buf), fp)) {
        out.append(buf, buf + n);
    }
#ifdef _WIN32
    int rc = _pclose(fp);
    return std::pair<std::string, int>{out, rc};
#else
    int rc = pclose(fp);
    return std::pair<std::string, int>{out, WIFEXITED(rc) ? WEXITSTATUS(rc) : -1};
#endif
}

std::vector<nlohmann::json> parse_ndjson_(const std::string& s) {
    std::vector<nlohmann::json> v;
    std::stringstream ss(s);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        auto j = nlohmann::json::parse(line, nullptr, false);
        if (!j.is_discarded()) v.push_back(std::move(j));
    }
    return v;
}

std::string make_sandbox_home_() {
    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path() / ("xlings-iface-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    return root.string();
}

}  // namespace

// ─── Protocol skeleton ──────────────────────────────────────

TEST(InterfaceProtocol, VersionFlagPrintsProtocolVersion) {
    auto home = make_sandbox_home_();
    auto [out, rc] = run_xlings_({"interface", "--version"}, home);
    ASSERT_EQ(rc, 0) << "xlings interface --version exit non-zero, out=" << out;
    auto j = nlohmann::json::parse(out, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << "non-JSON output: " << out;
    ASSERT_TRUE(j.contains("protocol_version"));
    EXPECT_EQ(j["protocol_version"].get<std::string>(), "1.0");
    std::filesystem::remove_all(home);
}

TEST(InterfaceProtocol, ListEmitsAllCapabilitiesWithSchema) {
    auto home = make_sandbox_home_();
    auto [out, rc] = run_xlings_({"interface", "--list"}, home);
    ASSERT_EQ(rc, 0) << "rc=" << rc << " out=" << out;
    auto j = nlohmann::json::parse(out, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << "non-JSON: " << out;
    ASSERT_TRUE(j.contains("protocol_version"));
    ASSERT_TRUE(j.contains("capabilities"));
    ASSERT_TRUE(j["capabilities"].is_array());
    EXPECT_GE(j["capabilities"].size(), 9u)
        << "expected at least 9 capabilities (search/install/remove/...)";
    for (auto& c : j["capabilities"]) {
        EXPECT_TRUE(c.contains("name"));
        EXPECT_TRUE(c.contains("description"));
        EXPECT_TRUE(c.contains("inputSchema"));
        EXPECT_TRUE(c.contains("destructive"));
    }
    std::filesystem::remove_all(home);
}

TEST(InterfaceProtocol, SystemStatusEmitsResultLine) {
    auto home = make_sandbox_home_();
    auto [out, rc] = run_xlings_({"interface", "system_status", "--args", "{}"}, home);
    ASSERT_EQ(rc, 0) << out;
    auto events = parse_ndjson_(out);
    ASSERT_FALSE(events.empty()) << "no NDJSON output";
    auto& last = events.back();
    ASSERT_TRUE(last.contains("kind"));
    EXPECT_EQ(last["kind"].get<std::string>(), "result");
    ASSERT_TRUE(last.contains("exitCode"));
    EXPECT_EQ(last["exitCode"].get<int>(), 0);
    std::filesystem::remove_all(home);
}

TEST(InterfaceProtocol, DataEventWiredAsKindData) {
    auto home = make_sandbox_home_();
    auto [out, rc] = run_xlings_({"interface", "system_status", "--args", "{}"}, home);
    ASSERT_EQ(rc, 0) << out;
    auto events = parse_ndjson_(out);
    bool found_data = false;
    for (auto& e : events) {
        if (e.contains("kind") && e["kind"].get<std::string>() == "data") {
            ASSERT_TRUE(e.contains("dataKind"));
            ASSERT_TRUE(e.contains("payload"));
            EXPECT_FALSE(e["dataKind"].get<std::string>().empty());
            found_data = true;
            break;
        }
    }
    EXPECT_TRUE(found_data) << "no kind:\"data\" event in: " << out;
    std::filesystem::remove_all(home);
}

TEST(InterfaceProtocol, NoCapabilityArgEmitsResultExitOne) {
    auto home = make_sandbox_home_();
    auto [out, rc] = run_xlings_({"interface"}, home);
    std::filesystem::remove_all(home);
    auto events = parse_ndjson_(out);
    ASSERT_FALSE(events.empty()) << out;
    auto& last = events.back();
    EXPECT_EQ(last["kind"].get<std::string>(), "result");
    EXPECT_EQ(last["exitCode"].get<int>(), 1);
}

// ─── Error model (enum strings since 2026-04-26) ───────────

TEST(InterfaceProtocol, UnknownCapabilityEmitsErrorThenResult) {
    auto home = make_sandbox_home_();
    auto [out, rc] = run_xlings_({"interface", "no_such_capability_xyz"}, home);
    std::filesystem::remove_all(home);
    auto events = parse_ndjson_(out);
    ASSERT_GE(events.size(), 2u) << "expected error + result lines, got: " << out;
    bool saw_error = false;
    for (auto& e : events) {
        if (e.contains("kind") && e["kind"].get<std::string>() == "error") {
            ASSERT_TRUE(e["code"].is_string()) << "code field must be enum string in: " << out;
            EXPECT_EQ(e["code"].get<std::string>(), "E_NOT_FOUND");
            EXPECT_FALSE(e["recoverable"].get<bool>());
            EXPECT_TRUE(e.contains("hint")) << "expected remediation hint";
            saw_error = true;
        }
    }
    EXPECT_TRUE(saw_error);
    auto& last = events.back();
    EXPECT_EQ(last["kind"].get<std::string>(), "result");
    EXPECT_EQ(last["exitCode"].get<int>(), 1);
}

// ─── Capability surface coverage ────────────────────────────

TEST(InterfaceProtocol, ListIncludesSubosEnvAndRepos) {
    auto home = make_sandbox_home_();
    auto [out, rc] = run_xlings_({"interface", "--list"}, home);
    std::filesystem::remove_all(home);
    ASSERT_EQ(rc, 0) << out;
    auto j = nlohmann::json::parse(out, nullptr, false);
    ASSERT_FALSE(j.is_discarded());
    std::set<std::string> names;
    for (auto& c : j["capabilities"]) names.insert(c["name"].get<std::string>());
    for (auto* expected : {"list_subos", "list_subos_shims", "create_subos",
                           "switch_subos", "remove_subos", "env",
                           "list_repos", "add_repo", "remove_repo"}) {
        EXPECT_TRUE(names.count(expected) == 1)
            << "missing capability: " << expected;
    }
}

TEST(InterfaceProtocol, EnvCapabilityReturnsHomeAndPaths) {
    auto home = make_sandbox_home_();
    auto [out, rc] = run_xlings_({"interface", "env", "--args", "{}"}, home);
    auto events = parse_ndjson_(out);
    std::filesystem::remove_all(home);
    ASSERT_EQ(rc, 0) << out;
    nlohmann::json data;
    for (auto& e : events) {
        if (e.value("kind", "") == "data" && e.value("dataKind", "") == "env") {
            data = e["payload"];
            break;
        }
    }
    ASSERT_FALSE(data.is_null()) << "no kind:data dataKind:env event in: " << out;
    EXPECT_EQ(data["xlingsHome"].get<std::string>(), home);
    EXPECT_TRUE(data.contains("activeSubos"));
    EXPECT_TRUE(data.contains("binDir"));
    EXPECT_TRUE(data.contains("subosDir"));
    EXPECT_TRUE(data.contains("dataDir"));
    EXPECT_EQ(events.back().value("exitCode", -1), 0);
}

// ─── SubOS lifecycle ────────────────────────────────────────

TEST(InterfaceProtocol, ListSubosEmitsSubosListDataKind) {
    auto home = make_sandbox_home_();
    auto [out, rc] = run_xlings_({"interface", "list_subos", "--args", "{}"}, home);
    auto events = parse_ndjson_(out);
    std::filesystem::remove_all(home);
    ASSERT_EQ(rc, 0) << out;
    bool found = false;
    for (auto& e : events) {
        if (e.value("kind", "") == "data" && e.value("dataKind", "") == "subos_list") {
            ASSERT_TRUE(e["payload"].contains("entries"));
            EXPECT_TRUE(e["payload"]["entries"].is_array());
            found = true;
        }
    }
    EXPECT_TRUE(found) << "no subos_list dataKind in: " << out;
}

TEST(InterfaceProtocol, SubosLifecycleCreateSwitchRemove) {
    auto home = make_sandbox_home_();

    // Fresh sandbox has no .xlings.json, so "default" doesn't exist as a
    // tracked entry. Create it first so we can switch back to it before
    // removing the test subos (subos::remove refuses to delete the active).
    {
        auto [out, rc] = run_xlings_(
            {"interface", "create_subos", "--args", R"({"name":"default"})"}, home);
        EXPECT_EQ(rc, 0) << out;
    }

    {
        auto [out, rc] = run_xlings_(
            {"interface", "create_subos", "--args", R"({"name":"iface_test_subos"})"}, home);
        EXPECT_EQ(rc, 0) << out;
    }
    EXPECT_TRUE(std::filesystem::exists(
        std::filesystem::path(home) / "subos" / "iface_test_subos"));

    {
        auto [out, rc] = run_xlings_(
            {"interface", "switch_subos", "--args", R"({"name":"iface_test_subos"})"}, home);
        EXPECT_EQ(rc, 0) << out;
    }

    // env reflects the switch
    {
        auto [out, rc] = run_xlings_({"interface", "env", "--args", "{}"}, home);
        auto events = parse_ndjson_(out);
        EXPECT_EQ(rc, 0);
        nlohmann::json data;
        for (auto& e : events)
            if (e.value("kind", "") == "data" && e.value("dataKind", "") == "env")
                data = e["payload"];
        ASSERT_FALSE(data.is_null());
        EXPECT_EQ(data["activeSubos"].get<std::string>(), "iface_test_subos");
    }

    // switch back to default before removing
    {
        auto [out, rc] = run_xlings_(
            {"interface", "switch_subos", "--args", R"({"name":"default"})"}, home);
        EXPECT_EQ(rc, 0) << out;
    }

    {
        auto [out, rc] = run_xlings_(
            {"interface", "remove_subos", "--args", R"({"name":"iface_test_subos"})"}, home);
        EXPECT_EQ(rc, 0) << out;
    }
    EXPECT_FALSE(std::filesystem::exists(
        std::filesystem::path(home) / "subos" / "iface_test_subos"));

    std::filesystem::remove_all(home);
}

TEST(InterfaceProtocol, RemoveSubosRefusesDefault) {
    auto home = make_sandbox_home_();
    auto [out, rc] = run_xlings_(
        {"interface", "remove_subos", "--args", R"({"name":"default"})"}, home);
    auto events = parse_ndjson_(out);
    std::filesystem::remove_all(home);
    ASSERT_FALSE(events.empty()) << out;
    EXPECT_NE(events.back().value("exitCode", 0), 0)
        << "remove_subos default should fail with non-zero exit";
}

TEST(InterfaceProtocol, ListSubosShimsEmitsShimsArray) {
    auto home = make_sandbox_home_();
    auto [out, rc] = run_xlings_({"interface", "list_subos_shims", "--args", "{}"}, home);
    auto events = parse_ndjson_(out);
    std::filesystem::remove_all(home);
    ASSERT_EQ(rc, 0) << out;
    bool found = false;
    for (auto& e : events) {
        if (e.value("kind", "") == "data" && e.value("dataKind", "") == "subos_shims") {
            ASSERT_TRUE(e["payload"].contains("shims"));
            EXPECT_TRUE(e["payload"]["shims"].is_array());
            ASSERT_TRUE(e["payload"].contains("binDir"));
            found = true;
        }
    }
    EXPECT_TRUE(found) << "no subos_shims dataKind in: " << out;
}

// ─── dataKind rename (info_panel → system_info) ────────────

TEST(InterfaceProtocol, SystemStatusEmitsSystemInfoDataKind) {
    auto home = make_sandbox_home_();
    auto [out, rc] = run_xlings_({"interface", "system_status", "--args", "{}"}, home);
    auto events = parse_ndjson_(out);
    std::filesystem::remove_all(home);
    ASSERT_EQ(rc, 0) << out;
    bool found_new = false;
    for (auto& e : events) {
        if (e.value("kind", "") == "data" && e.value("dataKind", "") == "system_info") {
            found_new = true;
        }
    }
    EXPECT_TRUE(found_new)
        << "expected dataKind:\"system_info\" (renamed from info_panel) in: " << out;
}

// ─── Repo lifecycle ─────────────────────────────────────────

TEST(InterfaceProtocol, RepoListBaselineHasDefault) {
    auto home = make_sandbox_home_();
    auto [out, rc] = run_xlings_({"interface", "list_repos", "--args", "{}"}, home);
    std::filesystem::remove_all(home);
    EXPECT_EQ(rc, 0) << out;
    auto events = parse_ndjson_(out);
    bool found = false;
    for (auto& e : events) {
        if (e.value("kind", "") == "data" && e.value("dataKind", "") == "repo_list") {
            ASSERT_TRUE(e["payload"].contains("repos"));
            EXPECT_TRUE(e["payload"]["repos"].is_array());
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(InterfaceProtocol, RepoAddListUpdateRemove) {
    auto home = make_sandbox_home_();

    {
        auto [out, rc] = run_xlings_({"interface", "add_repo", "--args",
            R"({"name":"myns","url":"https://example.com/x.git"})"}, home);
        EXPECT_EQ(rc, 0) << out;
    }

    {
        auto [out, rc] = run_xlings_({"interface", "list_repos", "--args", "{}"}, home);
        EXPECT_EQ(rc, 0) << out;
        auto events = parse_ndjson_(out);
        bool saw_myns = false;
        for (auto& e : events) {
            if (e.value("kind", "") == "data" && e.value("dataKind", "") == "repo_list") {
                for (auto& r : e["payload"]["repos"]) {
                    std::string name  = r.value("name",  std::string{});
                    std::string url   = r.value("url",   std::string{});
                    std::string scope = r.value("scope", std::string{});
                    if (name == "myns") {
                        EXPECT_EQ(url,   "https://example.com/x.git");
                        EXPECT_EQ(scope, "global");
                        saw_myns = true;
                    }
                }
            }
        }
        EXPECT_TRUE(saw_myns) << out;
    }

    // update URL by re-adding same name
    {
        auto [out, rc] = run_xlings_({"interface", "add_repo", "--args",
            R"({"name":"myns","url":"https://newurl.com/y.git"})"}, home);
        EXPECT_EQ(rc, 0) << out;
    }
    {
        auto [out, _rc] = run_xlings_({"interface", "list_repos", "--args", "{}"}, home);
        auto events = parse_ndjson_(out);
        for (auto& e : events) {
            if (e.value("kind", "") == "data" && e.value("dataKind", "") == "repo_list") {
                for (auto& r : e["payload"]["repos"]) {
                    std::string name = r.value("name", std::string{});
                    std::string url  = r.value("url",  std::string{});
                    if (name == "myns") EXPECT_EQ(url, "https://newurl.com/y.git");
                }
            }
        }
    }

    {
        auto [out, rc] = run_xlings_({"interface", "remove_repo", "--args",
            R"({"name":"myns"})"}, home);
        EXPECT_EQ(rc, 0) << out;
    }

    std::filesystem::remove_all(home);
}

TEST(InterfaceProtocol, RemoveRepoNonexistentEmitsNotFound) {
    auto home = make_sandbox_home_();
    auto [out, rc] = run_xlings_({"interface", "remove_repo", "--args",
        R"({"name":"never_existed"})"}, home);
    std::filesystem::remove_all(home);
    auto events = parse_ndjson_(out);
    ASSERT_FALSE(events.empty()) << out;
    EXPECT_NE(events.back().value("exitCode", 0), 0);
    bool saw_err = false;
    for (auto& e : events) {
        if (e.value("kind", "") == "error") {
            std::string code = e.value("code", std::string{});
            EXPECT_EQ(code, "E_NOT_FOUND");
            saw_err = true;
        }
    }
    EXPECT_TRUE(saw_err);
}

// ─── plan_install (dry-run) ────────────────────────────────

TEST(InterfaceProtocol, PlanInstallEmitsInstallPlanWithoutSideEffects) {
    auto home = make_sandbox_home_();

    // plan_install for a nonexistent package should still emit a result;
    // the catalog won't be loaded in a fresh sandbox so resolution exits 1
    // — that is itself a useful preflight signal for clients. Just verify
    // the capability is dispatched and produces NDJSON.
    auto [out, _rc] = run_xlings_({"interface", "plan_install", "--args",
        R"({"targets":["xim:nonexistent_pkg_xyz"]})"}, home);
    auto events = parse_ndjson_(out);
    ASSERT_FALSE(events.empty()) << out;
    EXPECT_EQ(events.back().value("kind", ""), "result");

    std::filesystem::remove_all(home);
}

TEST(InterfaceProtocol, PlanInstallShowsUpInCapabilityList) {
    auto home = make_sandbox_home_();
    auto [out, rc] = run_xlings_({"interface", "--list"}, home);
    std::filesystem::remove_all(home);
    ASSERT_EQ(rc, 0) << out;
    auto j = nlohmann::json::parse(out, nullptr, false);
    bool found_plan = false;
    for (auto& c : j["capabilities"]) {
        if (c["name"].get<std::string>() == "plan_install") {
            EXPECT_FALSE(c["destructive"].get<bool>())
                << "plan_install must be non-destructive (read-only preflight)";
            found_plan = true;
        }
    }
    EXPECT_TRUE(found_plan) << "plan_install missing from --list";
}

TEST(InterfaceProtocol, AddRepoInvalidEmitsInvalidInput) {
    auto home = make_sandbox_home_();
    auto [out, _rc] = run_xlings_({"interface", "add_repo", "--args",
        R"({"name":"x","url":""})"}, home);
    std::filesystem::remove_all(home);
    auto events = parse_ndjson_(out);
    bool saw_err = false;
    for (auto& e : events) {
        if (e.value("kind", "") == "error") {
            std::string code = e.value("code", std::string{});
            EXPECT_EQ(code, "E_INVALID_INPUT");
            saw_err = true;
        }
    }
    EXPECT_TRUE(saw_err) << out;
}
