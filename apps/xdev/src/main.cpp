// xdev -- the project's development tool (design §24.3).
//
//   xdev test   [pattern] [--suite NAME]... [--no-mcpp] [--out DIR] [--tarball F]
//   xdev report [--in DIR]... [--summary] [--requirements FILE] [--fail-uncovered]
//   xdev ci plan
//   xdev doctor
//
// `test` runs `mcpp test --message-format json` with the XTEST outputs wired,
// then the legacy script suites from tests/suites.toml through an adapter,
// and renders one report. Locally and in CI it is the same command producing
// the same report; CI only adds `--summary` (GitHub step summary) and merges
// the lanes' run directories with `report --in`.
#include <cstdio>
#include <cstdlib>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

import std;
import xlings.libs.json;
import xlings.testkit;
import xlings.xdev.toml;

namespace fs = std::filesystem;
namespace tk = xlings::testkit;
namespace toml = xlings::xdev::toml;
using nlohmann::json;

namespace {

// ── small utilities ──────────────────────────────────────────────────

std::string env_or(const char* name, std::string fallback = {}) {
    if (const char* v = std::getenv(name); v && *v) return v;
    return fallback;
}

void set_env(const std::string& k, const std::string& v) {
#if defined(_WIN32)
    ::_putenv_s(k.c_str(), v.c_str());
#else
    ::setenv(k.c_str(), v.c_str(), 1);
#endif
}

std::string platform_name() {
    if constexpr (tk::is_windows) return "windows";
    else if constexpr (tk::is_macos) return "macos";
    else return "linux";
}

// Runs through the platform shell, output to `log`. Returns the exit code.
int shell(const std::string& command, const fs::path& log) {
    const std::string full = tk::is_windows
        ? command + " > \"" + log.string() + "\" 2>&1"
        : "(" + command + ") > \"" + log.string() + "\" 2>&1";
    std::fflush(stdout);
    int rc = std::system(full.c_str());
#if defined(_WIN32)
    return rc;
#else
    if (rc == -1) return -1;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    return 128 + (WIFSIGNALED(rc) ? WTERMSIG(rc) : 0);
#endif
}

std::string tail_lines(const std::string& text, std::size_t n) {
    std::size_t pos = text.size();
    std::size_t count = 0;
    while (pos > 0 && count <= n) {
        pos = text.rfind('\n', pos - 1);
        if (pos == std::string::npos) { pos = 0; break; }
        ++count;
    }
    return text.substr(pos == 0 ? 0 : pos + 1);
}

std::vector<json> read_ndjson(const fs::path& p) {
    std::vector<json> out;
    std::ifstream f(p);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line.front() != '{') continue;
        auto j = json::parse(line, nullptr, false);
        if (!j.is_discarded()) out.push_back(std::move(j));
    }
    return out;
}

void append_ndjson(const fs::path& p, const json& j) {
    std::ofstream f(p, std::ios::app);
    f << j.dump() << '\n';
}

fs::path repo_root() {
    // xdev is run from the checkout; walk up to the directory holding mcpp.toml
    // and tests/ so it also works from a subdirectory.
    std::error_code ec;
    for (auto d = fs::current_path(ec); !d.empty(); d = d.parent_path()) {
        if (fs::exists(d / "mcpp.toml") && fs::is_directory(d / "tests")) return d;
        if (d == d.parent_path()) break;
    }
    return fs::current_path(ec);
}

// ── lane description ─────────────────────────────────────────────────

json describe_lane() {
    json lane;
    lane["name"] = env_or("XDEV_LANE", "local-" + platform_name());
    lane["platform"] = platform_name();
    lane["declared"] = json::array();
    for (auto cap : tk::capability_names())
        if (tk::lane_declares(cap)) lane["declared"].push_back(std::string(cap));
    json probes = json::object();
    for (auto cap : tk::capability_names()) {
        auto why = tk::probe(cap);
        probes[std::string(cap)] = why ? json(*why) : json(true);
    }
    lane["probes"] = probes;
    return lane;
}

// ── test ─────────────────────────────────────────────────────────────

struct TestArgs {
    std::string pattern;
    std::vector<std::string> suites;
    bool mcpp = true;
    fs::path out;
    std::string tarball;
    std::vector<std::string> mcpp_args;
};

int run_suites(const TestArgs& a, const fs::path& out, const fs::path& root) {
    if (a.suites.empty()) return 0;
    auto doc = toml::parse_file(root / "tests" / "suites.toml");
    if (!doc) {
        std::println(stderr, "xdev: {}", doc.error());
        return 2;
    }
    fs::create_directories(out / "logs");
    // A suite that reports per test (run_all.sh) appends here too.
    set_env("XDEV_RECORDS", (out / "scripts.ndjson").string());
    int failed = 0;
    const auto bin = tk::xlings_binary().string();
    for (const auto& suite : a.suites) {
        auto it = doc->tables.find(suite);
        if (it == doc->tables.end()) {
            std::println(stderr, "xdev: no suite '{}' in tests/suites.toml", suite);
            return 2;
        }
        const auto& t = it->second;
        if (auto p = t.find("platforms"); p != t.end() && p->second.list()) {
            const auto& plats = *p->second.list();
            if (std::ranges::find(plats, platform_name()) == plats.end()) {
                append_ndjson(out / "scripts.ndjson",
                              json{{"test", "suite:" + suite}, {"status", "skip"}, {"ms", 0},
                                   {"message", "suite runs on " + std::format("{}", plats)}});
                continue;
            }
        }
        auto c = t.find("commands");
        if (c == t.end() || !c->second.list()) {
            std::println(stderr, "xdev: suite '{}' has no commands", suite);
            return 2;
        }
        int index = 0;
        for (const auto& templ : *c->second.list()) {
            auto command = templ;
            auto replace_all = [&](std::string_view from, const std::string& to) {
                for (std::size_t pos; (pos = command.find(from)) != std::string::npos;)
                    command.replace(pos, from.size(), to);
            };
            replace_all("{xlings}", bin);
            replace_all("{tarball}", a.tarball);
            auto log = out / "logs" / std::format("{}-{:02}.log", suite, index++);
            std::println("xdev: [{}] {}", suite, command);
            const auto started = std::chrono::steady_clock::now();
            int rc = shell(command, log);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - started).count();
            json r{{"test", suite + ": " + templ}, {"status", rc == 0 ? "pass" : "fail"},
                   {"ms", ms}, {"exit_code", rc}, {"log", log.string()}};
            if (rc != 0) {
                ++failed;
                r["message"] = tail_lines(tk::read_file(log), 30);
                std::println("xdev:   FAIL (exit {}), log: {}", rc, log.string());
            }
            append_ndjson(out / "scripts.ndjson", r);
        }
    }
    return failed;
}

int cmd_report_dirs(const std::vector<fs::path>& dirs, bool summary,
                    const fs::path& requirements, bool fail_uncovered, const fs::path& write_to);

int cmd_test(const TestArgs& a) {
    const auto root = repo_root();
    auto out = a.out.empty() ? root / "target" / "xdev" / "run" : a.out;
    std::error_code ec;
    fs::remove_all(out, ec);
    fs::create_directories(out);
    out = fs::absolute(out);

    {
        std::ofstream(out / "lane.json") << describe_lane().dump(2);
    }

    int mcpp_rc = 0;
    if (a.mcpp) {
        set_env("XTEST_META_OUT", (out / "meta.ndjson").string());
        set_env("XTEST_RESULTS_OUT", (out / "cases.ndjson").string());
        set_env("XTEST_ARTIFACTS", (out / "artifacts").string());
        std::string command = "mcpp test --message-format json";
        for (auto& x : a.mcpp_args) command += " " + x;
        if (!a.pattern.empty()) command += " " + a.pattern;
        std::println("xdev: {}", command);
        std::fflush(stdout);
        auto full = (tk::is_windows ? std::string{} : "cd \"" + root.string() + "\" && ")
                    + command + " > \"" + (out / "mcpp.ndjson").string() + "\"";
        mcpp_rc = std::system(full.c_str());
    }
    int script_failures = run_suites(a, out, root);
    if (script_failures == 2 && a.suites.size() > 0 && !fs::exists(out / "scripts.ndjson")) return 2;

    int rc = cmd_report_dirs({out}, env_or("GITHUB_STEP_SUMMARY").size() > 0
                                        && env_or("XDEV_SUMMARY") == "1",
                             {}, false, out);
    if (mcpp_rc != 0 && rc == 0) {
        // mcpp itself failed before reporting a test (resolution, build).
        std::println(stderr, "xdev: `mcpp test` failed without a failing test record");
        return 1;
    }
    return rc;
}

// ── report ───────────────────────────────────────────────────────────

struct Record {
    std::string kind;     // binary | case | script
    std::string name;
    std::string status;   // pass | fail | skip
    long long ms = 0;
    std::string message;
    std::string lane;
};

std::string normalise_mcpp_status(const std::string& s) {
    if (s == "pass" || s == "ok") return "pass";
    if (s == "skip" || s == "ignored") return "skip";
    return "fail";
}

std::string first_lines(const std::string& s, std::size_t n) {
    std::string out;
    std::size_t lines = 0;
    for (char c : s) {
        if (c == '\n' && ++lines >= n) break;
        out.push_back(c);
    }
    return out;
}

struct Requirement {
    std::string id;
    std::string text;
    std::string kind;     // flow | isolation
    std::string status;   // required | planned | deferred
    std::string why;
};

int cmd_report_dirs(const std::vector<fs::path>& dirs, bool summary,
                    const fs::path& requirements, bool fail_uncovered, const fs::path& write_to) {
    std::vector<Record> records;
    std::vector<json> lanes;
    std::map<std::string, json> meta;   // test -> meta

    for (const auto& dir : dirs) {
        std::string lane_name = dir.filename().string();
        if (auto l = json::parse(tk::read_file(dir / "lane.json"), nullptr, false);
            !l.is_discarded() && l.is_object()) {
            lane_name = l.value("name", lane_name);
            lanes.push_back(l);
        }
        for (auto& j : read_ndjson(dir / "mcpp.ndjson")) {
            if (!j.contains("test")) continue;
            Record r{ .kind = "binary", .name = j.value("test", ""),
                      .status = normalise_mcpp_status(j.value("status", "")),
                      .ms = j.value("duration_ms", 0LL), .lane = lane_name };
            if (r.status == "fail") {
                auto out = j.value("compile_output", std::string());
                if (out.empty()) out = j.value("run_output", std::string());
                r.message = j.value("status", "") + ": " + tail_lines(out, 25);
            }
            records.push_back(std::move(r));
        }
        for (auto& j : read_ndjson(dir / "cases.ndjson")) {
            records.push_back({ .kind = "case", .name = j.value("test", ""),
                                .status = j.value("status", ""), .ms = j.value("ms", 0LL),
                                .message = j.value("message", ""), .lane = lane_name });
        }
        for (auto& j : read_ndjson(dir / "scripts.ndjson")) {
            records.push_back({ .kind = "script", .name = j.value("test", ""),
                                .status = j.value("status", ""), .ms = j.value("ms", 0LL),
                                .message = j.value("message", ""), .lane = lane_name });
        }
        for (auto& j : read_ndjson(dir / "meta.ndjson"))
            meta[j.value("test", "")] = j;
    }

    auto count = [&](std::string_view kind, std::string_view status) {
        return std::ranges::count_if(records, [&](const Record& r) {
            return r.kind == kind && r.status == status;
        });
    };

    std::string md;
    std::string lane_names;
    for (auto& l : lanes) lane_names += (lane_names.empty() ? "" : ", ") + l.value("name", "?");
    md += std::format("## xdev report — {}\n\n", lane_names.empty() ? "no lanes" : lane_names);
    md += "| | pass | fail | skip |\n|---|---:|---:|---:|\n";
    for (auto kind : {"binary", "case", "script"}) {
        md += std::format("| {} | {} | {} | {} |\n",
                          std::string_view(kind) == "binary" ? "test binaries (mcpp)"
                          : std::string_view(kind) == "case" ? "XTEST cases" : "legacy scripts",
                          count(kind, "pass"), count(kind, "fail"), count(kind, "skip"));
    }

    std::vector<const Record*> failures;
    for (auto& r : records) if (r.status == "fail") failures.push_back(&r);
    if (!failures.empty()) {
        md += "\n### Failures\n\n";
        for (auto* r : failures) {
            md += std::format("- **{}** `{}` ({})\n", r->kind, r->name, r->lane);
            if (!r->message.empty())
                md += "\n  ```\n" + first_lines(r->message, 25) + "\n  ```\n";
        }
    }

    std::map<std::string, std::vector<std::string>> skips;
    for (auto& r : records)
        if (r.status == "skip") skips[first_lines(r.message, 1)].push_back(r.name);
    if (!skips.empty()) {
        md += "\n### Skipped, by reason\n\n";
        for (auto& [why, names] : skips) {
            md += std::format("- {} — {}", why.empty() ? "(no reason given)" : why, names.size());
            md += names.size() <= 4 ? std::format(": {}\n", names) : "\n";
        }
    }

    std::vector<const Record*> by_time;
    for (auto& r : records) if (r.kind != "case") by_time.push_back(&r);
    std::ranges::sort(by_time, std::greater<>{}, &Record::ms);
    if (!by_time.empty()) {
        md += "\n### Slowest\n\n| ms | test | lane |\n|---:|---|---|\n";
        for (std::size_t i = 0; i < std::min<std::size_t>(10, by_time.size()); ++i)
            md += std::format("| {} | `{}` | {} |\n", by_time[i]->ms, by_time[i]->name, by_time[i]->lane);
    }

    if (!lanes.empty()) {
        md += "\n### Capabilities\n\n| lane | declared | missing here |\n|---|---|---|\n";
        for (auto& l : lanes) {
            std::string missing;
            for (auto it = l["probes"].begin(); it != l["probes"].end(); ++it)
                if (!it.value().is_boolean()) missing += (missing.empty() ? "" : ", ") + it.key();
            std::string declared;
            for (auto& d : l["declared"]) declared += (declared.empty() ? "" : ", ") + d.get<std::string>();
            md += std::format("| {} | {} | {} |\n", l.value("name", "?"),
                              declared.empty() ? "—" : declared, missing.empty() ? "—" : missing);
        }
    }

    // Requirement coverage (T5): every ID has a test; isolation IDs need an
    // isolation test; every ID a test names exists.
    int coverage_errors = 0;
    if (!requirements.empty()) {
        auto doc = toml::parse_file(requirements);
        if (!doc) {
            std::println(stderr, "xdev: {}", doc.error());
            return 2;
        }
        std::vector<Requirement> reqs;
        for (auto& name : doc->order) {
            if (name.empty()) continue;
            auto& t = doc->tables[name];
            Requirement r{ .id = name };
            if (auto it = t.find("text"); it != t.end() && it->second.str()) r.text = *it->second.str();
            r.kind = "flow";
            if (auto it = t.find("kind"); it != t.end() && it->second.str()) r.kind = *it->second.str();
            r.status = "required";
            if (auto it = t.find("status"); it != t.end() && it->second.str()) r.status = *it->second.str();
            if (auto it = t.find("why"); it != t.end() && it->second.str()) r.why = *it->second.str();
            if (r.status != "required" && r.status != "planned" && r.status != "deferred") {
                std::println(stderr, "xdev: {}: unknown status '{}'", r.id, r.status);
                return 2;
            }
            if (r.kind != "flow" && r.kind != "isolation") {
                std::println(stderr, "xdev: {}: unknown kind '{}'", r.id, r.kind);
                return 2;
            }
            reqs.push_back(std::move(r));
        }
        std::map<std::string, std::vector<std::string>> covered_by;   // id -> tests
        std::vector<std::string> unknown;
        for (auto& [test, m] : meta) {
            for (auto& id : m.value("covers", json::array())) {
                auto sid = id.get<std::string>();
                auto it = std::ranges::find(reqs, sid, &Requirement::id);
                if (it == reqs.end()) { unknown.push_back(test + " → " + sid); continue; }
                if (it->kind == "isolation" && m.value("proves", "flow") != "isolation") continue;
                covered_by[sid].push_back(test);
            }
        }
        std::vector<const Requirement*> uncovered, planned, deferred;
        std::size_t required = 0;
        for (auto& r : reqs) {
            if (r.status == "planned") { planned.push_back(&r); continue; }
            if (r.status == "deferred") { deferred.push_back(&r); continue; }
            ++required;
            if (!covered_by.contains(r.id)) uncovered.push_back(&r);
        }
        md += std::format("\n### Requirements\n\n{} required: {} covered, {} uncovered · "
                          "{} planned · {} deferred · {} unknown IDs in tests\n",
                          required, required - uncovered.size(), uncovered.size(),
                          planned.size(), deferred.size(), unknown.size());
        if (!uncovered.empty()) {
            md += "\nUncovered (required):\n\n";
            for (auto* r : uncovered)
                md += std::format("- `{}` ({}) {}\n", r->id, r->kind, r->text);
        }
        if (!planned.empty()) {
            md += "\n<details><summary>Planned</summary>\n\n";
            for (auto* r : planned)
                md += std::format("- `{}` {}{}\n", r->id, r->text,
                                  covered_by.contains(r->id) ? " — **covered: flip to required**" : "");
            md += "\n</details>\n";
        }
        if (!deferred.empty()) {
            md += "\nDeferred:\n\n";
            for (auto* r : deferred) md += std::format("- `{}` {} — {}\n", r->id, r->text, r->why);
        }
        if (!unknown.empty()) {
            md += "\nTests naming an ID that tests/requirements.toml does not declare:\n\n";
            for (auto& u : unknown) md += "- " + u + "\n";
        }
        if (fail_uncovered) coverage_errors = static_cast<int>(uncovered.size() + unknown.size());
        else coverage_errors = static_cast<int>(unknown.size());
    }

    std::println("{}", md);
    if (!write_to.empty()) {
        std::ofstream(write_to / "report.md") << md;
        json j;
        j["lanes"] = lanes;
        j["records"] = json::array();
        for (auto& r : records)
            j["records"].push_back({{"kind", r.kind}, {"name", r.name}, {"status", r.status},
                                    {"ms", r.ms}, {"lane", r.lane}});
        std::ofstream(write_to / "report.json") << j.dump(2);
    }
    if (summary) {
        if (auto path = env_or("GITHUB_STEP_SUMMARY"); !path.empty())
            std::ofstream(path, std::ios::app) << md << "\n";
    }
    return (failures.empty() && coverage_errors == 0) ? 0 : 1;
}

// ── doctor ───────────────────────────────────────────────────────────

int cmd_doctor() {
    auto lane = describe_lane();
    std::println("xdev doctor — {} ({})", lane["name"].get<std::string>(), lane["platform"].get<std::string>());
    auto bin = tk::xlings_binary();
    std::println("  xlings under test: {}", bin.empty() ? "(none — run `mcpp build`)" : bin.string());
    for (auto it = lane["probes"].begin(); it != lane["probes"].end(); ++it) {
        const auto cap = it.key();
        const auto& v = it.value();
        bool declared = tk::lane_declares(cap);
        std::println("  {:<11} {}{}", cap, v.is_boolean() ? "yes" : "no — " + v.get<std::string>(),
                     declared ? "  [declared by lane]" : "");
    }
    return 0;
}

int usage() {
    std::println(stderr,
        "usage: xdev <command>\n"
        "  test   [pattern] [--suite NAME]... [--no-mcpp] [--out DIR] [--tarball FILE] [-- mcpp args]\n"
        "  report [--in DIR]... [--summary] [--requirements FILE] [--fail-uncovered]\n"
        "  ci plan\n"
        "  doctor");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return usage();
    const auto cmd = args[0];

    if (cmd == "test") {
        TestArgs a;
        for (std::size_t i = 1; i < args.size(); ++i) {
            const auto& x = args[i];
            if (x == "--suite" && i + 1 < args.size()) a.suites.push_back(args[++i]);
            else if (x == "--no-mcpp") a.mcpp = false;
            else if (x == "--out" && i + 1 < args.size()) a.out = args[++i];
            else if (x == "--tarball" && i + 1 < args.size()) a.tarball = args[++i];
            else if (x == "--") { a.mcpp_args.assign(args.begin() + static_cast<long>(i) + 1, args.end()); break; }
            else if (!x.empty() && x[0] != '-' && a.pattern.empty()) a.pattern = x;
            else { std::println(stderr, "xdev test: unknown option {}", x); return 2; }
        }
        return cmd_test(a);
    }
    if (cmd == "report") {
        std::vector<fs::path> dirs;
        bool summary = false, fail_uncovered = false;
        fs::path requirements, write_to;
        for (std::size_t i = 1; i < args.size(); ++i) {
            const auto& x = args[i];
            if (x == "--in" && i + 1 < args.size()) dirs.emplace_back(args[++i]);
            else if (x == "--summary") summary = true;
            else if (x == "--requirements" && i + 1 < args.size()) requirements = args[++i];
            else if (x == "--fail-uncovered") fail_uncovered = true;
            else if (x == "--write" && i + 1 < args.size()) write_to = args[++i];
            else { std::println(stderr, "xdev report: unknown option {}", x); return 2; }
        }
        if (dirs.empty()) dirs.push_back(repo_root() / "target" / "xdev" / "run");
        return cmd_report_dirs(dirs, summary, requirements, fail_uncovered, write_to);
    }
    if (cmd == "doctor") return cmd_doctor();
    return usage();
}
