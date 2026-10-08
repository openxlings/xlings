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
import std;
import xlings.libs.json;
import xlings.testkit;
import xlings.xdev.toml;
import xlings.xdev.selection;
import xlings.xdev.resources;
import xlings.xdev.fixture;
import xlings.xdev.history;

namespace fs = std::filesystem;
namespace tk = xlings::testkit;
namespace toml = xlings::xdev::toml;
namespace sel = xlings::xdev::selection;
namespace resource = xlings::xdev::resources;
namespace fixture = xlings::xdev::fixture;
namespace history = xlings::xdev::history;
using nlohmann::json;

namespace {

// ── small utilities ──────────────────────────────────────────────────

std::string env_or(const char* name, std::string fallback = {}) {
    if (const char* v = std::getenv(name); v && *v) return v;
    return fallback;
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
    std::cout.flush();
    return tk::system_exit_code(std::system(full.c_str()));
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
    lane["run"] = env_or("GITHUB_RUN_ID", std::to_string(std::chrono::system_clock::now().time_since_epoch().count()))
        + ":" + env_or("GITHUB_RUN_ATTEMPT", "1");
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
    sel::Options selection;
    bool selecting { false };
    bool no_build { false };
    std::string changed;
    fs::path discovery;
    fs::path timings;
    fs::path write_discovery;
    std::size_t jobs { 1 };
    fs::path fixture_root;
    fs::path lock_directory;
};

// A script states what it covers and what it needs in one header line, the
// way an XTEST does in its metadata:
//
//   # xtest: covers=ID,ID requires=sudo,docker proves=isolation
//
// System scenarios (a root-owned entry, a container, a booted kernel) run
// against the release artifact on lanes that have those capabilities, where
// the C++ test binaries -- dev builds against this runner's mcpp registry --
// cannot travel. The header makes them part of the same requirement map and
// the same capability rule: missing on a lane that declares it fails, missing
// elsewhere skips.
std::optional<tk::Meta> script_meta(const std::string& command, const fs::path& root) {
    std::istringstream words(command);
    for (std::string w; words >> w;) {
        if (!(w.ends_with(".sh") || w.ends_with(".py"))) continue;
        std::ifstream in(root / w);
        if (!in) continue;
        std::string line;
        for (int n = 0; n < 40 && std::getline(in, line); ++n) {
            constexpr std::string_view tag = "# xtest:";
            if (!line.starts_with(tag)) continue;
            tk::Meta m;
            m.area = "scenario";
            std::istringstream kv(line.substr(tag.size()));
            for (std::string pair; kv >> pair;) {
                auto eq = pair.find('=');
                if (eq == std::string::npos) continue;
                auto key = pair.substr(0, eq);
                std::vector<std::string> values;
                std::istringstream vs(pair.substr(eq + 1));
                for (std::string v; std::getline(vs, v, ',');)
                    if (!v.empty()) values.push_back(v);
                if (key == "covers") m.covers = values;
                else if (key == "requires") m.requires_ = values;
                else if (key == "resources") m.resources = values;
                else if (key == "proves" && !values.empty()) m.proves = values.front();
            }
            return m;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

struct Catalog {
    std::vector<sel::Test> tests;
    std::map<std::string, fs::path> binaries;
    std::set<std::string> impacted;
};

struct Scratch {
    fs::path path;
    Scratch() {
        static std::atomic<unsigned long long> serial { 0 };
        for (int attempt = 0; attempt < 100; ++attempt) {
            auto candidate = fs::temp_directory_path() / std::format("xdev-discovery-{}-{}",
                std::chrono::steady_clock::now().time_since_epoch().count(), serial++);
            if (fs::create_directory(candidate)) { path = std::move(candidate); return; }
        }
        throw std::runtime_error("cannot reserve discovery scratch directory");
    }
    ~Scratch() { std::error_code ec; fs::remove_all(path, ec); }
};

std::optional<fs::path> find_test_binary(const fs::path& root, const sel::Test& test) {
    std::error_code ec;
    if (!fs::is_directory(root / "target", ec)) return std::nullopt;
    auto name = test.id.substr(test.member.empty() ? 0 : test.member.size() + 1);
    if constexpr (tk::is_windows) name += ".exe";
    const auto suffix = "/bin/" + name;
    const auto memberSuffix = "/bin/" + test.member + "/" + name;
    std::optional<fs::path> newest;
    fs::file_time_type stamp { fs::file_time_type::min() };
    for (fs::recursive_directory_iterator it(root / "target", ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) { if (ec) break; continue; }
        const auto path = it->path().generic_string();
        if (!path.ends_with(suffix) && (test.member.empty() || !path.ends_with(memberSuffix))) continue;
        const auto time = it->last_write_time(ec);
        if (!ec && time > stamp) { newest = it->path(); stamp = time; }
    }
    return newest;
}

std::vector<sel::Case> listed_cases(std::string_view output, const std::vector<json>& metadata) {
    std::map<std::string, sel::Case> known;
    for (const auto& row : metadata) {
        sel::Case test{.name = row.value("test", ""), .area = row.value("area", ""),
            .cost = row.value("cost", "fast"), .requires_ = row.value("requires", std::vector<std::string>{}),
            .resources = row.value("resources", std::vector<std::string>{})};
        known[test.name] = std::move(test);
    }
    std::istringstream in{std::string(output)};
    std::vector<sel::Case> cases;
    std::string suite;
    for (std::string line; std::getline(in, line);) {
        if (const auto comment = line.find('#'); comment != line.npos) line.resize(comment);
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
        if (line.empty()) continue;
        if (line.starts_with("  ") && !suite.empty()) {
            const auto begin = line.find_first_not_of(' ');
            const auto name = suite + line.substr(begin);
            if (const auto found = known.find(name); found != known.end()) cases.push_back(found->second);
            else cases.push_back({.name = name});
        } else if (line.ends_with('.')) suite = line;
    }
    return cases;
}

std::expected<Catalog, std::string> catalog(const TestArgs& args, const fs::path& root) {
    Catalog result;
    if (args.mcpp) {
        std::string listed;
        if (!args.discovery.empty()) {
            std::ifstream in(args.discovery);
            if (!in) return std::unexpected("cannot read discovery " + args.discovery.string());
            listed = {std::istreambuf_iterator<char>(in), {}};
        } else {
            const auto discovered = tk::run({.argv = {"mcpp", "test", "--list", "--message-format", "json"},
                .env = tk::inherited_env(), .cwd = root});
            if (discovered.exit_code != 0)
                return std::unexpected("mcpp test discovery failed: " + discovered.transcript());
            listed = discovered.out;
        }
        const auto discovered = sel::discovery(listed, root);
        if (!discovered) return std::unexpected(discovered.error());
        result.tests = *discovered;
        Scratch scratch;
        for (auto& test : result.tests) {
            const auto binary = find_test_binary(root, test);
            if (!binary) continue;
            result.binaries[test.id] = *binary;
            if (test.metadata_known) continue;
            std::error_code ec;
            // A registry from an older test source is not authoritative.
            if (fs::last_write_time(*binary, ec) < fs::last_write_time(root / test.source, ec) || ec) continue;
            const auto registry = scratch.path / "meta.ndjson";
            fs::remove(registry, ec);
            auto env = tk::inherited_env();
            env["XTEST_META_OUT"] = registry.string();
            env.erase("XTEST_RESULTS_OUT");
            env.erase("XDEV_LANE_CAPS");
            const auto listing = tk::run({.argv = {binary->string(), "--gtest_list_tests", "--gtest_color=no"},
                .env = std::move(env), .cwd = root, .timeout = std::chrono::seconds(15)});
            if (listing.exit_code != 0) continue;
            test.cases = listed_cases(listing.out, read_ndjson(registry));
            test.metadata_known = !test.cases.empty();
        }
    }
    if (!args.suites.empty()) {
        const auto document = toml::parse_file(root / "tests/suites.toml");
        if (!document) return std::unexpected(document.error());
        for (const auto& suite : args.suites) {
            const auto table = document->tables.find(suite);
            if (table == document->tables.end()) return std::unexpected("unknown suite " + suite);
            const auto commands = table->second.find("commands");
            if (commands == table->second.end() || !commands->second.list())
                return std::unexpected("suite has no commands: " + suite);
            std::size_t index { 0 };
            for (const auto& command : *commands->second.list()) {
                sel::Test test{.id = std::format("script:{}:{}", suite, index), .source = "tests/suites.toml",
                    .script = true, .suite = suite, .command_index = index++, .duration_ms = 5000};
                if (auto platforms = table->second.find("platforms"); platforms != table->second.end() && platforms->second.list())
                    test.platforms = *platforms->second.list();
                std::istringstream words(command);
                for (std::string word; words >> word;)
                    if (word.ends_with(".sh") || word.ends_with(".py")) { test.source = word; break; }
                if (const auto metadata = script_meta(command, root)) {
                    test.metadata_known = true;
                    test.cases.push_back({.name = test.id, .area = metadata->area,
                        .cost = std::string(tk::to_string(metadata->cost)), .requires_ = metadata->requires_,
                        .resources = metadata->resources});
                }
                result.tests.push_back(std::move(test));
            }
        }
    }
    const auto timingFile = args.timings.empty() ? root / "tests/ci-timings.json" : args.timings;
    if (fs::exists(timingFile)) {
        const auto timings = json::parse(tk::read_file(timingFile), nullptr, false);
        if (const auto applied = sel::apply_timings(result.tests, timings, args.selection.platform.empty() ? platform_name() : args.selection.platform); !applied)
            return std::unexpected(applied.error());
    } else if (!args.timings.empty()) return std::unexpected("timings file does not exist: " + timingFile.string());
    if (!args.changed.empty()) {
        if (args.changed.front() == '-') return std::unexpected("--changed needs a git revision, not an option");
        std::vector<std::string> argv{"git", "diff", "--name-only", "-z", "--diff-filter=ACDMRTUXB"};
        argv.push_back(args.changed.find("..") == args.changed.npos ? args.changed + "...HEAD" : args.changed);
        argv.push_back("--");
        const auto diff = tk::run({.argv = std::move(argv), .env = tk::inherited_env(), .cwd = root});
        if (diff.exit_code != 0) return std::unexpected("cannot determine changed files: " + diff.transcript());
        std::vector<fs::path> changed;
        std::istringstream paths(diff.out);
        for (std::string path; std::getline(paths, path, '\0');) if (!path.empty()) changed.emplace_back(path);
        const auto graph = sel::sources(root);
        if (!graph) return std::unexpected(graph.error());
        result.impacted = sel::affected(result.tests, *graph, changed);
    }
    return result;
}

int cmd_plan(const TestArgs& args) {
    const auto root = repo_root();
    const auto inventory = catalog(args, root);
    if (!inventory) { std::println(std::cerr, "xdev: {}", inventory.error()); return 2; }
    auto options = args.selection;
    options.changed = !args.changed.empty();
    options.pattern = args.pattern;
    try {
        auto plan = sel::matrix(inventory->tests, options, inventory->impacted);
        std::vector<std::string> missing;
        for (const auto& test : inventory->tests)
            if (!test.metadata_known && !test.script) missing.push_back(test.id);
        if (!missing.empty())
            std::println(std::cerr, "xdev: compiled metadata unavailable for {}; selection is conservative (use --require-metadata in CI)", missing);
        if (!args.write_discovery.empty()) {
            if (fs::exists(fs::symlink_status(args.write_discovery))) {
                std::println(std::cerr, "xdev: discovery output already exists: {}", args.write_discovery.string());
                return 2;
            }
            std::ofstream output(args.write_discovery);
            for (const auto& test : inventory->tests) {
                if (test.script) continue;
                json row{{"member", test.member}, {"test", test.id.substr(test.member.empty() ? 0 : test.member.size() + 1)},
                    {"main", test.source.generic_string()}};
                if (test.metadata_known) {
                    row["cases"] = json::array();
                    for (const auto& testCase : test.cases)
                        row["cases"].push_back({{"test", testCase.name}, {"area", testCase.area},
                            {"cost", testCase.cost}, {"requires", testCase.requires_}, {"resources", testCase.resources}});
                }
                output << row.dump() << '\n';
            }
            output.close();
            if (!output) { std::println(std::cerr, "xdev: cannot write discovery output"); return 2; }
        }
        std::println("{}", plan.dump());
        return 0;
    } catch (const std::exception& error) {
        std::println(std::cerr, "xdev: {}", error.what());
        return 2;
    }
}

int run_suites(const TestArgs& a, const fs::path& out, const fs::path& root,
               const std::set<std::string>* selected = nullptr) {
    if (a.suites.empty()) return 0;
    auto doc = toml::parse_file(root / "tests" / "suites.toml");
    if (!doc) {
        std::println(std::cerr, "xdev: {}", doc.error());
        return 2;
    }
    fs::create_directories(out / "logs");
    // A suite that reports per test (run_all.sh) appends here too.
    tk::set_env("XDEV_RECORDS", (out / "scripts.ndjson").string());
    int failed = 0;
    const auto bin = tk::xlings_binary().string();
    for (const auto& suite : a.suites) {
        auto it = doc->tables.find(suite);
        if (it == doc->tables.end()) {
            std::println(std::cerr, "xdev: no suite '{}' in tests/suites.toml", suite);
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
            std::println(std::cerr, "xdev: suite '{}' has no commands", suite);
            return 2;
        }
        int index = 0;
        for (const auto& templ : *c->second.list()) {
            const auto commandIndex = index++;
            if (selected && !selected->contains(std::format("script:{}:{}", suite, commandIndex))) continue;
            auto command = templ;
            auto replace_all = [&](std::string_view from, const std::string& to) {
                for (std::size_t pos; (pos = command.find(from)) != std::string::npos;)
                    command.replace(pos, from.size(), to);
            };
            replace_all("{xlings}", bin);
            replace_all("{tarball}", a.tarball);
            auto log = out / "logs" / std::format("{}-{:02}.log", suite, commandIndex);
            const auto name = suite + ": " + templ;
            std::vector<std::string> resourceNames;
            if (auto m = script_meta(templ, root)) {
                resourceNames = m->resources;
                append_ndjson(out / "meta.ndjson",
                              json{{"test", name}, {"area", m->area}, {"covers", m->covers},
                                   {"requires", m->requires_}, {"resources", m->resources}, {"proves", m->proves}});
                if (auto v = tk::check_requirements(*m)) {
                    std::println("xdev: [{}] {} -- {}: {}", suite, templ,
                                 v->fail ? "FAIL" : "skip", v->reason);
                    if (v->fail) ++failed;
                    append_ndjson(out / "scripts.ndjson",
                                  json{{"test", name}, {"status", v->fail ? "fail" : "skip"},
                                       {"ms", 0}, {"message", v->reason}});
                    continue;
                }
            }
            std::println("xdev: [{}] {}", suite, command);
            const auto started = std::chrono::steady_clock::now();
            const auto lockDirectory = a.lock_directory.empty() ? root / "target/xdev/resource-locks" : a.lock_directory;
            auto lease = resource::Lease::acquire(lockDirectory, resourceNames);
            if (!lease) {
                ++failed;
                append_ndjson(out / "scripts.ndjson", json{{"test", name}, {"status", "fail"},
                    {"ms", 0}, {"message", lease.error()}});
                std::println(std::cerr, "xdev: {}", lease.error());
                continue;
            }
            int rc = shell(command, log);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - started).count();
            json r{{"test", name}, {"status", rc == 0 ? "pass" : "fail"},
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
                    const fs::path& requirements, bool fail_uncovered, bool fail_unverified, const fs::path& write_to,
                    const fs::path& trend_from = {}, const fs::path& timings_out = {});

int cmd_test(const TestArgs& a) {
    const auto root = repo_root();
    if (!a.selection.platform.empty() && a.selection.platform != platform_name()) {
        std::println(std::cerr, "xdev: test runs on {}; --platform belongs to ci plan", platform_name());
        return 2;
    }
    std::optional<Catalog> inventory;
    std::vector<sel::Selected> selection;
    std::set<std::string> selectedIds;
    if (a.mcpp || a.selecting || a.no_build || !a.fixture_root.empty()) {
        auto discovered = catalog(a, root);
        if (!discovered) { std::println(std::cerr, "xdev: {}", discovered.error()); return 2; }
        inventory = std::move(*discovered);
        auto options = a.selection;
        options.platform = platform_name();
        options.pattern = a.pattern;
        options.changed = !a.changed.empty();
        auto chosen = sel::select(inventory->tests, options, inventory->impacted);
        if (!chosen) { std::println(std::cerr, "xdev: {}", chosen.error()); return 2; }
        selection = std::move(*chosen);
        for (const auto& test : selection) selectedIds.insert(test.test.id);
    }
    auto out = a.out.empty() ? root / "target" / "xdev" / "run" : a.out;
    std::error_code ec;
    if (fs::exists(out, ec) && (!fs::is_directory(out, ec) || !fs::is_empty(out, ec))) {
        std::println(std::cerr, "xdev: output already exists and is not empty: {} (choose a new --out directory)",
                     out.string());
        return 2;
    }
    if (ec) {
        std::println(std::cerr, "xdev: cannot inspect output {}: {}", out.string(), ec.message());
        return 2;
    }
    fs::create_directories(out, ec);
    if (ec) { std::println(std::cerr, "xdev: {}", ec.message()); return 2; }
    out = fs::absolute(out);

    {
        std::ofstream(out / "lane.json") << describe_lane().dump(2);
    }

    std::optional<fixture::Server> server;
    if (!a.fixture_root.empty()) {
        auto started = fixture::Server::start(a.fixture_root);
        if (!started) { std::println(std::cerr, "xdev: {}", started.error()); return 2; }
        server.emplace(std::move(*started));
        std::println("xdev: fixture {} serves {}", server->url(), a.fixture_root.string());
    }
    const auto lockDirectory = a.lock_directory.empty() ? root / "target/xdev/resource-locks" : a.lock_directory;
    int mcpp_rc = 0;
    if (a.mcpp && inventory) {
        std::vector<resource::Task> tasks;
        std::vector<fs::path> taskDirectories;
        for (const auto& test : selection) {
            if (test.test.script) continue;
            std::string buildOutput;
            if (!a.no_build) {
                std::vector<std::string> argv{"mcpp", "test", "--no-run", "--message-format", "json"};
                const auto separator = std::ranges::find(a.mcpp_args, "--");
                argv.insert(argv.end(), a.mcpp_args.begin(), separator);
                if (!test.test.member.empty()) { argv.push_back("-p"); argv.push_back(test.test.member); }
                argv.push_back(test.test.id.substr(test.test.member.empty() ? 0 : test.test.member.size() + 1));
                const auto build = tk::run({.argv = std::move(argv), .env = tk::inherited_env(), .cwd = root,
                    .timeout = std::chrono::minutes(30)});
                buildOutput = build.transcript();
                if (build.exit_code != 0) {
                    mcpp_rc = 1;
                    append_ndjson(out / "mcpp.ndjson", json{{"test", test.test.id}, {"status", "compile_failed"},
                        {"duration_ms", build.elapsed.count()}, {"compile_output", buildOutput}});
                    continue;
                }
            }
            const auto binary = find_test_binary(root, test.test);
            if (!binary) {
                std::println(std::cerr, "xdev: no built test binary for {} (build tests before --no-build)", test.test.id);
                return 2;
            }
            auto current = test.test;
            Scratch scratch;
            auto listingEnv = tk::inherited_env();
            listingEnv["XTEST_META_OUT"] = (scratch.path / "meta.ndjson").string();
            listingEnv.erase("XTEST_RESULTS_OUT");
            listingEnv.erase("XDEV_LANE_CAPS");
            const auto listing = tk::run({.argv = {binary->string(), "--gtest_list_tests", "--gtest_color=no"},
                .env = std::move(listingEnv), .cwd = root, .timeout = std::chrono::seconds(15)});
            if (listing.exit_code == 0) {
                current.cases = listed_cases(listing.out, read_ndjson(scratch.path / "meta.ndjson"));
                current.metadata_known = !current.cases.empty();
            }
            auto options = a.selection;
            options.platform = platform_name();
            options.pattern.clear();
            options.shards = 1;
            options.shard = 0;
            options.changed = !a.changed.empty();
            // PR's no-network contract needs an actual registry, even on the
            // first run after building a newly discovered test source.
            options.require_metadata = options.require_metadata || options.lane == "pr";
            const auto fresh = sel::select(std::span<const sel::Test>(&current, 1), options, inventory->impacted);
            if (!fresh) { std::println(std::cerr, "xdev: {}", fresh.error()); return 2; }
            if (fresh->empty()) {
                append_ndjson(out / "mcpp.ndjson", json{{"test", test.test.id}, {"status", "skip"},
                    {"duration_ms", 0}, {"message", "excluded by the compiled case registry"}});
                continue;
            }
            std::vector<std::string> argv{binary->string()};
            if (a.no_build) {
                // mcpp arguments before its `--` are build flags; the binary
                // receives only the arguments after that separator.
                const auto separator = std::ranges::find(a.mcpp_args, "--");
                if (separator != a.mcpp_args.end()) argv.insert(argv.end(), separator + 1, a.mcpp_args.end());
                else if (!a.mcpp_args.empty()) {
                    std::println(std::cerr, "xdev: --no-build needs test arguments after -- --"); return 2;
                }
            } else {
                const auto separator = std::ranges::find(a.mcpp_args, "--");
                if (separator != a.mcpp_args.end()) argv.insert(argv.end(), separator + 1, a.mcpp_args.end());
            }
            if (!fresh->front().filter.empty()) argv.push_back("--gtest_filter=" + fresh->front().filter);
            const auto taskDirectory = out / "workers" / std::to_string(tasks.size());
            fs::create_directories(taskDirectory);
            taskDirectories.push_back(taskDirectory);
            auto env = tk::inherited_env();
            env["XTEST_META_OUT"] = (taskDirectory / "meta.ndjson").string();
            env["XTEST_RESULTS_OUT"] = (taskDirectory / "cases.ndjson").string();
            env["XTEST_ARTIFACTS"] = (taskDirectory / "artifacts").string();
            if (server) env["XLINGS_TEST_FIXTURE_URL"] = server->url();
            tasks.push_back({.id = test.test.id, .resources = fresh->front().resources,
                .run = [argv = std::move(argv), env = std::move(env), root, taskDirectory,
                        id = test.test.id, buildOutput] () mutable {
                    const auto run = tk::run({.argv = std::move(argv), .env = std::move(env), .cwd = root,
                        .timeout = std::chrono::minutes(30)});
                    append_ndjson(taskDirectory / "mcpp.ndjson", json{{"test", id},
                        {"status", run.exit_code == 0 ? "pass" : "fail"}, {"duration_ms", run.elapsed.count()},
                        {"run_output", run.transcript()}, {"compile_output", buildOutput}});
                    return run.exit_code;
                }});
        }
        const auto executed = resource::run(tasks, a.jobs, lockDirectory);
        if (!executed) { std::println(std::cerr, "xdev: {}", executed.error()); return 2; }
        for (std::size_t index = 0; index < executed->size(); ++index) {
            const auto& result = (*executed)[index];
            if (result.exit_code != 0) mcpp_rc = 1;
            if (!result.error.empty()) {
                append_ndjson(out / "mcpp.ndjson", json{{"test", result.id}, {"status", "fail"},
                    {"duration_ms", result.elapsed_ms}, {"run_output", result.error}});
                std::println(std::cerr, "xdev: {}: {}", result.id, result.error);
            }
            for (const auto& name : {"mcpp.ndjson", "meta.ndjson", "cases.ndjson"}) {
                for (const auto& row : read_ndjson(taskDirectories[index] / name)) {
                    append_ndjson(out / name, row);
                    if (std::string_view(name) == "mcpp.ndjson" && result.exit_code != 0)
                        std::println(std::cerr, "{}", tail_lines(row.value("run_output", ""), 25));
                }
            }
        }
    }
    // Script adapters inherit a fixture URL once; worker threads have joined.
    const auto oldFixture = env_or("XLINGS_TEST_FIXTURE_URL");
    if (server) tk::set_env("XLINGS_TEST_FIXTURE_URL", server->url());
    int script_failures = run_suites(a, out, root, inventory ? &selectedIds : nullptr);
    if (server) tk::set_env("XLINGS_TEST_FIXTURE_URL", oldFixture);
    if (script_failures == 2 && a.suites.size() > 0 && !fs::exists(out / "scripts.ndjson")) return 2;

    int rc = cmd_report_dirs({out}, env_or("GITHUB_STEP_SUMMARY").size() > 0
                                        && env_or("XDEV_SUMMARY") == "1",
                             {}, false, false, out);
    if (mcpp_rc != 0 && rc == 0) {
        // mcpp itself failed before reporting a test (resolution, build).
        std::println(std::cerr, "xdev: `mcpp test` failed without a failing test record");
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
                    const fs::path& requirements, bool fail_uncovered, bool fail_unverified, const fs::path& write_to,
                    const fs::path& trend_from, const fs::path& timings_out) {
    std::vector<Record> records;
    std::vector<json> lanes;
    std::vector<history::Observation> observations;
    using EvidenceKey = std::pair<std::string, std::string>; // lane, test
    std::map<EvidenceKey, json> meta;

    for (const auto& dir : dirs) {
        std::string lane_name = dir.filename().string();
        if (auto l = json::parse(tk::read_file(dir / "lane.json"), nullptr, false);
            !l.is_discarded() && l.is_object()) {
            lane_name = l.value("name", lane_name);
            lanes.push_back(l);
        }
        const auto first = records.size();
        std::string execution = fs::absolute(dir).lexically_normal().string();
        std::string platform;
        if (!lanes.empty() && lanes.back().value("name", "") == lane_name) {
            platform = lanes.back().value("platform", "");
            execution = lanes.back().value("run", execution);
        }
        for (auto& j : read_ndjson(dir / "mcpp.ndjson")) {
            if (!j.contains("test")) continue;
            auto id = j.value("test", "");
            const auto member = j.value("member", "");
            if (!member.empty() && !id.starts_with(member + ":")) id = member + ":" + id;
            Record r{ .kind = "binary", .name = std::move(id),
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
            meta[{lane_name, j.value("test", "")}] = j;
        if (!platform.empty())
            for (auto i = first; i < records.size(); ++i) {
                const auto& r = records[i];
                observations.push_back({r.kind, r.name, platform, lane_name, execution, r.status, r.ms});
            }
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
    json coverage = json::object();
    if (!requirements.empty()) {
        auto doc = toml::parse_file(requirements);
        if (!doc) {
            std::println(std::cerr, "xdev: {}", doc.error());
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
                std::println(std::cerr, "xdev: {}: unknown status '{}'", r.id, r.status);
                return 2;
            }
            if (r.kind != "flow" && r.kind != "isolation") {
                std::println(std::cerr, "xdev: {}: unknown kind '{}'", r.id, r.kind);
                return 2;
            }
            reqs.push_back(std::move(r));
        }
        std::map<std::string, std::vector<std::string>> covered_by;   // id -> tests
        std::map<std::string, std::vector<std::string>> verified_by;
        std::set<EvidenceKey> passed_tests;
        for (const auto& record : records)
            if (record.status == "pass" && record.kind != "binary") passed_tests.emplace(record.lane, record.name);
        std::vector<std::string> unknown;
        for (auto& [key, m] : meta) {
            const auto test = key.second + " (" + key.first + ")";
            for (auto& id : m.value("covers", json::array())) {
                auto sid = id.get<std::string>();
                auto it = std::ranges::find(reqs, sid, &Requirement::id);
                if (it == reqs.end()) { unknown.push_back(test + " → " + sid); continue; }
                if (it->kind == "isolation" && m.value("proves", "flow") != "isolation") continue;
                covered_by[sid].push_back(test);
                if (passed_tests.contains(key)) verified_by[sid].push_back(test);
            }
        }
        std::vector<const Requirement*> uncovered, unverified, planned, deferred;
        std::size_t required = 0;
        for (auto& r : reqs) {
            if (r.status == "planned") { planned.push_back(&r); continue; }
            if (r.status == "deferred") { deferred.push_back(&r); continue; }
            ++required;
            if (!covered_by.contains(r.id)) uncovered.push_back(&r);
            if (!verified_by.contains(r.id)) unverified.push_back(&r);
            coverage[r.id] = {{"kind", r.kind}, {"declared_by", covered_by[r.id]},
                              {"passed_by", verified_by[r.id]}};
        }
        md += std::format("\n### Requirements\n\n{} required: {} declared, {} undeclared · "
                          "{} planned · {} deferred · {} unknown IDs in tests\n",
                          required, required - uncovered.size(), uncovered.size(),
                          planned.size(), deferred.size(), unknown.size());
        md += std::format("\nExecution evidence: {} verified, {} unverified. Skipped tests do not verify a requirement.\n",
                          required - unverified.size(), unverified.size());
        if (!unverified.empty()) {
            md += "\nUnverified (required):\n\n";
            for (auto* r : unverified)
                md += std::format("- `{}` ({}) {}\n", r->id, r->kind, r->text);
        }
        if (!uncovered.empty()) {
            md += "\nUndeclared (required):\n\n";
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
        if (fail_unverified) coverage_errors += static_cast<int>(unverified.size());
    }

    json previous;
    if (!trend_from.empty()) {
        std::ifstream input(trend_from);
        if (!input) { std::println(std::cerr, "xdev: cannot read trend history {}", trend_from.string()); return 2; }
        previous = json::parse(input, nullptr, false);
    }
    const auto trend = history::append(previous, observations);
    if (!trend) { std::println(std::cerr, "xdev: {}", trend.error()); return 2; }
    md += trend->markdown;
    std::println("{}", md);
    if (!timings_out.empty()) {
        std::ofstream output(timings_out);
        output << trend->timings.dump(2) << '\n';
        output.flush();
        if (!output) { std::println(std::cerr, "xdev: cannot write timings {}", timings_out.string()); return 2; }
    }
    if (!write_to.empty()) {
        fs::create_directories(write_to);
        std::ofstream(write_to / "trend.json") << trend->history.dump(2) << '\n';
        std::ofstream(write_to / "report.md") << md;
        json j;
        j["lanes"] = lanes;
        j["requirements"] = coverage;
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

bool parse_selection_option(const std::vector<std::string>& args, std::size_t& index, TestArgs& out) {
    const auto& option = args[index];
    auto value = [&]() -> std::string {
        if (index + 1 >= args.size()) throw std::invalid_argument("missing value for " + option);
        return args[++index];
    };
    if (option == "--require-metadata") out.selection.require_metadata = true;
    else if (option == "--lane") out.selection.lane = value();
    else if (option == "--platform") out.selection.platform = value();
    else if (option == "--changed") out.changed = value();
    else if (option == "--area") out.selection.area = value();
    else if (option == "--discovery") out.discovery = value();
    else if (option == "--timings") out.timings = value();
    else if (option == "--write-discovery") out.write_discovery = value();
    else if (option == "--shard") {
        const auto shard = sel::parse_shard(value());
        if (!shard) throw std::invalid_argument(shard.error());
        out.selection.shard = shard->first;
        out.selection.shards = shard->second;
    } else if (option == "--shards") {
        const auto count = value();
        const auto shard = sel::parse_shard("1/" + count);
        if (!shard) throw std::invalid_argument(shard.error());
        out.selection.shards = shard->second;
    } else return false;
    out.selecting = true;
    return true;
}

int usage() {
    std::println(std::cerr,
        "usage: xdev <command>\n"
        "  test   [unit|e2e|perf|pattern] [--lane pr|main|nightly] [--shard i/n] [--changed BASE]\n"
        "         [-j N] [--fixture DIR] [--lock-dir DIR] [--suite NAME]... [--no-mcpp|--no-build] [--out DIR] [--tarball FILE] [-- mcpp args]\n"
        "  report [--in DIR]... [--summary] [--requirements FILE] [--fail-uncovered] [--fail-unverified]\n"
        "         [--trend PREVIOUS.json] [--write DIR] [--timings-out FILE]\n"
        "  ci plan [--lane pr|main|nightly] [--shards N] [--platform OS] [--changed BASE..HEAD]\n"
        "          [--suite NAME]... [--discovery FILE] [--timings FILE] [--write-discovery FILE] [--require-metadata]\n"
        "  doctor");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return usage();
    const auto cmd = args[0];

    if (cmd == "ci" && args.size() >= 2 && args[1] == "plan") {
        TestArgs plan;
        for (std::size_t i = 2; i < args.size(); ++i) {
            try {
                if (parse_selection_option(args, i, plan)) continue;
            } catch (const std::exception& error) { std::println(std::cerr, "xdev: {}", error.what()); return 2; }
            if (args[i] == "--suite" && i + 1 < args.size()) plan.suites.push_back(args[++i]);
            else if (args[i] == "--no-mcpp") plan.mcpp = false;
            else if (!args[i].empty() && args[i][0] != '-' && plan.pattern.empty()) plan.pattern = args[i];
            else { std::println(std::cerr, "xdev ci plan: unknown option {}", args[i]); return 2; }
        }
        if (plan.pattern == "unit" || plan.pattern == "e2e" || plan.pattern == "perf")
            plan.selection.category = std::exchange(plan.pattern, {});
        return cmd_plan(plan);
    }
    if (cmd == "test") {
        tk::set_env("XDEV_BIN", fs::absolute(argv[0]).string());
        TestArgs a;
        for (std::size_t i = 1; i < args.size(); ++i) {
            const auto& x = args[i];
            try {
                if (parse_selection_option(args, i, a)) continue;
            } catch (const std::exception& error) { std::println(std::cerr, "xdev: {}", error.what()); return 2; }
            if (x == "-j" || x == "--jobs") {
                if (i + 1 >= args.size()) return usage();
                const auto& count = args[++i];
                const auto [end, error] = std::from_chars(count.data(), count.data() + count.size(), a.jobs);
                if (error != std::errc{} || end != count.data() + count.size() || a.jobs == 0 || a.jobs > 1024) {
                    std::println(std::cerr, "xdev: -j needs 1..1024 workers"); return 2;
                }
                a.selecting = true;
            } else if (x == "--fixture" && i + 1 < args.size()) { a.fixture_root = args[++i]; a.selecting = true; }
            else if (x == "--lock-dir" && i + 1 < args.size()) { a.lock_directory = args[++i]; a.selecting = true; }
            else if (x == "--suite" && i + 1 < args.size()) a.suites.push_back(args[++i]);
            else if (x == "--no-mcpp") a.mcpp = false;
            else if (x == "--no-build") { a.no_build = true; a.selecting = true; }
            else if (x == "--out" && i + 1 < args.size()) a.out = args[++i];
            else if (x == "--tarball" && i + 1 < args.size()) a.tarball = args[++i];
            else if (x == "--") { a.mcpp_args.assign(args.begin() + static_cast<long>(i) + 1, args.end()); break; }
            else if (!x.empty() && x[0] != '-' && a.pattern.empty()) a.pattern = x;
            else { std::println(std::cerr, "xdev test: unknown option {}", x); return 2; }
        }
        if (a.pattern == "unit" || a.pattern == "e2e" || a.pattern == "perf") {
            a.selection.category = std::exchange(a.pattern, {});
            a.selecting = true;
        }
        return cmd_test(a);
    }
    if (cmd == "report") {
        std::vector<fs::path> dirs;
        bool summary = false, fail_uncovered = false, fail_unverified = false;
        fs::path requirements, write_to, trend_from, timings_out;
        for (std::size_t i = 1; i < args.size(); ++i) {
            const auto& x = args[i];
            if (x == "--in" && i + 1 < args.size()) dirs.emplace_back(args[++i]);
            else if (x == "--summary") summary = true;
            else if (x == "--requirements" && i + 1 < args.size()) requirements = args[++i];
            else if (x == "--fail-uncovered") fail_uncovered = true;
            else if (x == "--fail-unverified") fail_unverified = true;
            else if (x == "--write" && i + 1 < args.size()) write_to = args[++i];
            else if (x == "--trend" && i + 1 < args.size()) trend_from = args[++i];
            else if (x == "--timings-out" && i + 1 < args.size()) timings_out = args[++i];
            else { std::println(std::cerr, "xdev report: unknown option {}", x); return 2; }
        }
        if (dirs.empty()) dirs.push_back(repo_root() / "target" / "xdev" / "run");
        return cmd_report_dirs(dirs, summary, requirements, fail_uncovered, fail_unverified, write_to, trend_from, timings_out);
    }
    if (cmd == "doctor") return cmd_doctor();
    return usage();
}
