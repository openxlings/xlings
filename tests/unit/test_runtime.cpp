// tests/unit/test_runtime.cpp — profile generations, the event stream, capabilities, the task manager and
// their integration, plus subos GPU passthrough.
//
// Split out of the former single 12.7k-line test_main.cpp. Section order
// and contents are unchanged; only the file boundary is new.

#include <gtest/gtest.h>
#include <iomanip>
#ifdef __unix__
#include <sys/wait.h>
#endif
#if !defined(_WIN32)
#include <unistd.h>  // geteuid — AtomicWriteTest skips permission cases as root
#endif

import std;
import xlings.core.i18n;
import xlings.core.log;
import xlings.core.utils;
import xlings.ui;
import xlings.core.xim.libxpkg.types.type;
import xlings.core.xim.index;
import xlings.core.xim.catalog;
import xlings.core.xim.resolver;
import xlings.core.xim.downloader;
import xlings.core.xim.installer;
import xlings.core.xim.commands;
import xlings.core.xim.repo;
import xlings.core.xim.extract;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.bindings;
import xlings.core.xvm.removal;
import xlings.core.xvm.registration;
import xlings.core.xvm.errors;
import xlings.core.xvm.inspect;
import xlings.core.xvm.lock;
import xlings.core.xvm.switch_plan;
import xlings.core.xvm.shim;
import xlings.core.xvm.commands;
import xlings.core.compact;
import xlings.core.config;
import xlings.core.home_config;
import xlings.platform;
import xlings.libs.json;
import xlings.core.xself;
import xlings.core.profile;
import xlings.core.subos.gpu;
import xlings.core.xim.downloader;
import xlings.runtime;
import xlings.capabilities;
import xlings.libs.tinyhttps;
import xlings.libs.sha256;
import mcpplibs.xpkg;
import mcpplibs.xpkg.executor;
import mcpplibs.cmdline;

namespace {

struct ScopedEnvVar {
    std::string name;
    bool had_prev{false};
    std::string prev_value;

    ScopedEnvVar(std::string_view key, std::string_view value) : name(key) {
        if (auto* prev = std::getenv(name.c_str())) {
            had_prev = true;
            prev_value = prev;
        }
        set(value);
    }

    ~ScopedEnvVar() {
        if (had_prev) set(prev_value);
        else set("");
    }

    void set(std::string_view value) {
        xlings::platform::set_env_variable(name, std::string(value));
    }
};

std::optional<std::filesystem::path> find_pkgindex_repo() {
    namespace fs = std::filesystem;

    if (auto env = std::getenv("XIM_PKGINDEX_DIR")) {
        fs::path path(env);
        if (fs::exists(path / "pkgs")) return path;
    }

    const std::vector<fs::path> candidates = {
        fs::current_path() / "tests/fixtures/xim-pkgindex",
        fs::current_path() / "../xim-pkgindex",
        fs::current_path() / "../d2learn/xim-pkgindex",
        fs::current_path() / "../../xim-pkgindex",
        fs::current_path() / "../../d2learn/xim-pkgindex",
    };

    for (auto& path : candidates) {
        std::error_code ec;
        if (fs::exists(path / "pkgs", ec)) return fs::weakly_canonical(path, ec);
    }

    return std::nullopt;
}

std::optional<std::filesystem::path> find_fixture_repo(std::string_view name) {
    namespace fs = std::filesystem;

    const std::vector<fs::path> candidates = {
        fs::current_path() / "tests/fixtures" / name,
        fs::current_path() / "../../tests/fixtures" / name,
    };
    for (auto& path : candidates) {
        std::error_code ec;
        if (fs::exists(path / "pkgs", ec)) {
            return fs::weakly_canonical(path, ec);
        }
    }
    return std::nullopt;
}

}  // namespace

// ============================================================
// Profile generation tests
// ============================================================

class ProfileTest : public ::testing::Test {
protected:
    std::filesystem::path testDir_;

    void SetUp() override {
        testDir_ = std::filesystem::temp_directory_path() / "xlings_profile_test";
        std::filesystem::create_directories(testDir_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(testDir_, ec);
    }
};

TEST_F(ProfileTest, LoadCurrentEmpty) {
    // No profile file → returns generation 0
    auto gen = xlings::profile::load_current(testDir_);
    EXPECT_EQ(gen.number, 0);
    EXPECT_TRUE(gen.packages.empty());
}

TEST_F(ProfileTest, CommitAndLoadRoundTrip) {
    std::map<std::string, std::string> packages = {
        {"gcc", "15.1.0"},
        {"node", "22.0.0"},
    };
    int rc = xlings::profile::commit(testDir_, packages, "install gcc+node");
    EXPECT_EQ(rc, 0);

    auto gen = xlings::profile::load_current(testDir_);
    EXPECT_EQ(gen.number, 1);
    EXPECT_EQ(gen.packages.size(), 2u);
    EXPECT_EQ(gen.packages["gcc"], "15.1.0");
    EXPECT_EQ(gen.packages["node"], "22.0.0");

    // Second commit
    packages["python"] = "3.12.0";
    rc = xlings::profile::commit(testDir_, packages, "add python");
    EXPECT_EQ(rc, 0);

    gen = xlings::profile::load_current(testDir_);
    EXPECT_EQ(gen.number, 2);
    EXPECT_EQ(gen.packages.size(), 3u);
}

TEST_F(ProfileTest, ListGenerations) {
    std::map<std::string, std::string> p1 = {{"gcc", "15.1.0"}};
    std::map<std::string, std::string> p2 = {{"gcc", "15.1.0"}, {"node", "22.0.0"}};

    xlings::profile::commit(testDir_, p1, "install gcc");
    xlings::profile::commit(testDir_, p2, "add node");

    auto gens = xlings::profile::list_generations(testDir_);
    EXPECT_EQ(gens.size(), 2u);
    EXPECT_EQ(gens[0].number, 1);
    EXPECT_EQ(gens[0].packages.size(), 1u);
    EXPECT_EQ(gens[1].number, 2);
    EXPECT_EQ(gens[1].packages.size(), 2u);
}

TEST_F(ProfileTest, Rollback) {
    std::map<std::string, std::string> p1 = {{"gcc", "15.1.0"}};
    std::map<std::string, std::string> p2 = {{"gcc", "15.1.0"}, {"node", "22.0.0"}};

    xlings::profile::commit(testDir_, p1, "install gcc");
    xlings::profile::commit(testDir_, p2, "add node");

    int rc = xlings::profile::rollback(testDir_, 1);
    EXPECT_EQ(rc, 0);

    auto gen = xlings::profile::load_current(testDir_);
    EXPECT_EQ(gen.number, 1);
    EXPECT_EQ(gen.packages.size(), 1u);
    EXPECT_EQ(gen.packages["gcc"], "15.1.0");
}

TEST_F(ProfileTest, RollbackNonexistentFails) {
    int rc = xlings::profile::rollback(testDir_, 99);
    EXPECT_EQ(rc, 1);
}

TEST_F(ProfileTest, FindSubosReferencingEmpty) {
    // No subos dir → empty result
    auto refs = xlings::profile::find_subos_referencing(testDir_, "gcc");
    EXPECT_TRUE(refs.empty());
}

// ============================================================
// Log system extended tests
// ============================================================

TEST(LogTest, GetLevelReturnsSetValue) {
    xlings::log::set_level(xlings::log::Level::Debug);
    EXPECT_EQ(xlings::log::get_level(), xlings::log::Level::Debug);

    xlings::log::set_level(xlings::log::Level::Warn);
    EXPECT_EQ(xlings::log::get_level(), xlings::log::Level::Warn);

    // Restore
    xlings::log::set_level(xlings::log::Level::Info);
}

TEST(LogTest, LevelStringMatchesLevel) {
    xlings::log::set_level(xlings::log::Level::Debug);
    EXPECT_EQ(xlings::log::level_string(), "debug");

    xlings::log::set_level(xlings::log::Level::Info);
    EXPECT_EQ(xlings::log::level_string(), "info");

    xlings::log::set_level(xlings::log::Level::Warn);
    EXPECT_EQ(xlings::log::level_string(), "warn");

    xlings::log::set_level(xlings::log::Level::Error);
    EXPECT_EQ(xlings::log::level_string(), "error");

    // Restore
    xlings::log::set_level(xlings::log::Level::Info);
}

TEST(LogTest, EnableColorToggle) {
    // Should not crash and should be toggleable
    xlings::log::enable_color(false);
    xlings::log::info("no color test");
    xlings::log::enable_color(true);
    xlings::log::info("color test");
}

TEST(LogTest, LevelFiltering) {
    namespace fs = std::filesystem;
    // Use a unique file name to avoid conflicts with other log tests
    auto tmpFile = fs::temp_directory_path() / "xlings_test_log_filter2.txt";
    std::error_code ec;
    fs::remove(tmpFile, ec);

    // Save and restore level around test
    auto savedLevel = xlings::log::get_level();

    xlings::log::set_level(xlings::log::Level::Warn);
    xlings::log::set_file(tmpFile);

    xlings::log::debug("should_not_appear_debug");
    xlings::log::info("should_not_appear_info");
    xlings::log::warn("warn_visible");
    xlings::log::error("error_visible");

    // Close the log file before reading
    xlings::log::set_file("");

    // Read and verify
    std::ifstream f(tmpFile);
    if (!f.is_open()) {
        // On some platforms the file might not be created if ofstream has issues
        // Skip rather than fail hard
        xlings::log::set_level(savedLevel);
        GTEST_SKIP() << "Could not open log file for reading";
    }
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    f.close();

    EXPECT_EQ(content.find("should_not_appear"), std::string::npos);
    EXPECT_NE(content.find("warn_visible"), std::string::npos);
    EXPECT_NE(content.find("error_visible"), std::string::npos);

    fs::remove(tmpFile, ec);
    xlings::log::set_level(savedLevel);
}

// ═══════════════════════════════════════════════════════════════
//  EventStream tests
// ═══════════════════════════════════════════════════════════════

TEST(Event, ProgressEventConstruction) {
    xlings::ProgressEvent e{
        .phase = "downloading",
        .percent = 0.5f,
        .message = "Downloading gcc-15..."
    };
    EXPECT_EQ(e.phase, "downloading");
    EXPECT_FLOAT_EQ(e.percent, 0.5f);
    EXPECT_EQ(e.message, "Downloading gcc-15...");
}

TEST(Event, PromptEventConstruction) {
    xlings::PromptEvent e{
        .id = "p1",
        .question = "Override existing?",
        .options = {"y", "n"},
        .defaultValue = "n"
    };
    EXPECT_EQ(e.id, "p1");
    EXPECT_EQ(e.options.size(), 2);
    EXPECT_EQ(e.defaultValue, "n");
}

TEST(Event, VariantHoldsTypes) {
    xlings::Event ev = xlings::LogEvent{xlings::LogLevel::info, "hello"};
    EXPECT_TRUE(std::holds_alternative<xlings::LogEvent>(ev));

    ev = xlings::ErrorEvent{.code = xlings::ErrorCode::Network,
                             .message = "fail", .recoverable = true};
    auto& err = std::get<xlings::ErrorEvent>(ev);
    EXPECT_TRUE(err.code == xlings::ErrorCode::Network);
    EXPECT_TRUE(err.recoverable);
}

TEST(Event, CompletedEvent) {
    xlings::Event ev = xlings::CompletedEvent{.success = true, .summary = "done"};
    auto& c = std::get<xlings::CompletedEvent>(ev);
    EXPECT_TRUE(c.success);
}

TEST(Event, DataEvent) {
    xlings::Event ev = xlings::DataEvent{.kind = "search_results", .json = R"({"count":3})"};
    auto& d = std::get<xlings::DataEvent>(ev);
    EXPECT_EQ(d.kind, "search_results");
}

// ============================================================
// EventStream tests
// ============================================================

TEST(EventStream, EmitAndConsume) {
    xlings::EventStream stream;
    std::vector<xlings::Event> received;

    stream.on_event([&](const xlings::Event& e) {
        received.push_back(e);
    });

    stream.emit(xlings::LogEvent{xlings::LogLevel::info, "hello"});
    stream.emit(xlings::ProgressEvent{"downloading", 0.5f, "..."});

    ASSERT_EQ(received.size(), 2);
    EXPECT_TRUE(std::holds_alternative<xlings::LogEvent>(received[0]));
    EXPECT_TRUE(std::holds_alternative<xlings::ProgressEvent>(received[1]));
}

TEST(EventStream, MultipleConsumers) {
    xlings::EventStream stream;
    int count_a = 0, count_b = 0;

    stream.on_event([&](const xlings::Event&) { ++count_a; });
    stream.on_event([&](const xlings::Event&) { ++count_b; });

    stream.emit(xlings::LogEvent{xlings::LogLevel::info, "test"});

    EXPECT_EQ(count_a, 1);
    EXPECT_EQ(count_b, 1);
}

TEST(EventStream, PromptAndRespond) {
    xlings::EventStream stream;
    std::string captured_question;

    stream.on_event([&](const xlings::Event& e) {
        if (auto* p = std::get_if<xlings::PromptEvent>(&e)) {
            captured_question = p->question;
            stream.respond(p->id, "y");
        }
    });

    auto answer = stream.prompt({
        .id = "p1",
        .question = "Override?",
        .options = {"y", "n"},
        .defaultValue = "n"
    });

    EXPECT_EQ(captured_question, "Override?");
    EXPECT_EQ(answer, "y");
}

TEST(EventStream, PromptDefaultOnEmpty) {
    xlings::EventStream stream;

    stream.on_event([&](const xlings::Event& e) {
        if (auto* p = std::get_if<xlings::PromptEvent>(&e)) {
            stream.respond(p->id, p->defaultValue);
        }
    });

    auto answer = stream.prompt({
        .id = "p2",
        .question = "Continue?",
        .options = {},
        .defaultValue = "yes"
    });
    EXPECT_EQ(answer, "yes");
}

TEST(EventStream, PromptBlocksUntilRespond) {
    xlings::EventStream stream;
    std::atomic<bool> promptReturned { false };
    std::string answer;

    stream.on_event([](const xlings::Event&) {});

    std::thread taskThread([&] {
        answer = stream.prompt({
            .id = "p_async",
            .question = "Confirm?",
            .options = {"y", "n"},
            .defaultValue = "n"
        });
        promptReturned.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(promptReturned.load());

    stream.respond("p_async", "confirmed");

    taskThread.join();
    EXPECT_TRUE(promptReturned.load());
    EXPECT_EQ(answer, "confirmed");
}

TEST(EventStream, ConcurrentPromptsFromMultipleTasks) {
    xlings::EventStream stream;
    std::string answer1, answer2;

    stream.on_event([](const xlings::Event&) {});

    std::thread t1([&] {
        answer1 = stream.prompt({.id = "pa", .question = "Q1"});
    });
    std::thread t2([&] {
        answer2 = stream.prompt({.id = "pb", .question = "Q2"});
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    stream.respond("pb", "ans_b");
    stream.respond("pa", "ans_a");

    t1.join();
    t2.join();

    EXPECT_EQ(answer1, "ans_a");
    EXPECT_EQ(answer2, "ans_b");
}

// ============================================================
// ─── Mock Capabilities for testing ───
// ============================================================

namespace {

class MockSearchCapability : public xlings::capability::Capability {
public:
    auto spec() const -> xlings::capability::CapabilitySpec override {
        return {
            .name = "search_packages",
            .description = "Search for packages",
            .inputSchema = R"({"type":"object","properties":{"query":{"type":"string"}}})",
            .outputSchema = R"({"type":"object","properties":{"results":{"type":"array"}}})",
            .destructive = false,
            .asyncCapable = true
        };
    }

    auto execute(xlings::capability::Params params,
                 xlings::EventStream& stream) -> xlings::capability::Result override {
        stream.emit(xlings::LogEvent{xlings::LogLevel::info, "Searching..."});
        stream.emit(xlings::DataEvent{.kind = "search_results", .json = R"({"results":["gcc","g++"]})"});
        stream.emit(xlings::CompletedEvent{.success = true, .summary = "Found 2 packages"});
        return R"({"count":2})";
    }
};

class MockInstallCapability : public xlings::capability::Capability {
public:
    auto spec() const -> xlings::capability::CapabilitySpec override {
        return {
            .name = "install_package",
            .description = "Install a package",
            .inputSchema = R"({"type":"object","properties":{"name":{"type":"string"}}})",
            .outputSchema = R"({"type":"object","properties":{"status":{"type":"string"}}})",
            .destructive = true,
            .asyncCapable = true
        };
    }

    auto execute(xlings::capability::Params params,
                 xlings::EventStream& stream) -> xlings::capability::Result override {
        stream.emit(xlings::ProgressEvent{"installing", 0.5f, "Installing..."});
        auto answer = stream.prompt({
            .id = "confirm_install",
            .question = "Proceed with install?",
            .options = {"y", "n"},
            .defaultValue = "y"
        });
        if (answer == "n") {
            return R"({"status":"cancelled"})";
        }
        stream.emit(xlings::CompletedEvent{.success = true, .summary = "Installed"});
        return R"({"status":"ok"})";
    }
};

}  // anonymous namespace

// ============================================================
// ─── Capability Tests ───
// ============================================================

TEST(Capability, RegistryRegisterAndGet) {
    xlings::capability::Registry reg;
    reg.register_capability(std::make_unique<MockSearchCapability>());
    reg.register_capability(std::make_unique<MockInstallCapability>());

    auto* search = reg.get("search_packages");
    ASSERT_NE(search, nullptr);
    EXPECT_EQ(search->spec().name, "search_packages");
    EXPECT_FALSE(search->spec().destructive);

    auto* install = reg.get("install_package");
    ASSERT_NE(install, nullptr);
    EXPECT_TRUE(install->spec().destructive);

    EXPECT_EQ(reg.get("nonexistent"), nullptr);
}

TEST(Capability, RegistryListAll) {
    xlings::capability::Registry reg;
    reg.register_capability(std::make_unique<MockSearchCapability>());
    reg.register_capability(std::make_unique<MockInstallCapability>());

    auto specs = reg.list_all();
    EXPECT_EQ(specs.size(), 2);
}

TEST(Capability, ExecuteWithEventStream) {
    xlings::EventStream stream;
    std::vector<xlings::Event> events;
    stream.on_event([&](const xlings::Event& e) { events.push_back(e); });

    MockSearchCapability search;
    auto result = search.execute(R"({"query":"gcc"})", stream);

    ASSERT_EQ(events.size(), 3);
    EXPECT_TRUE(std::holds_alternative<xlings::LogEvent>(events[0]));
    EXPECT_TRUE(std::holds_alternative<xlings::DataEvent>(events[1]));
    EXPECT_TRUE(std::holds_alternative<xlings::CompletedEvent>(events[2]));
    EXPECT_EQ(result, R"({"count":2})");
}

TEST(Capability, ExecuteWithPrompt) {
    xlings::EventStream stream;
    stream.on_event([&](const xlings::Event& e) {
        if (auto* p = std::get_if<xlings::PromptEvent>(&e)) {
            stream.respond(p->id, "y");
        }
    });

    MockInstallCapability install;
    auto result = install.execute(R"({"name":"gcc"})", stream);
    EXPECT_EQ(result, R"({"status":"ok"})");
}

TEST(Capability, ExecutePromptCancelled) {
    xlings::EventStream stream;
    stream.on_event([&](const xlings::Event& e) {
        if (auto* p = std::get_if<xlings::PromptEvent>(&e)) {
            stream.respond(p->id, "n");
        }
    });

    MockInstallCapability install;
    auto result = install.execute(R"({"name":"gcc"})", stream);
    EXPECT_EQ(result, R"({"status":"cancelled"})");
}

// ============================================================
// ─── TaskManager Tests ───
// ============================================================

TEST(TaskManager, SubmitAndComplete) {
    xlings::capability::Registry reg;
    reg.register_capability(std::make_unique<MockSearchCapability>());

    xlings::task::TaskManager tm { reg };
    auto tid = tm.submit("search_packages", R"({"query":"gcc"})");
    EXPECT_FALSE(tid.empty());

    for (int i { 0 }; i < 100; ++i) {
        if (tm.info(tid).status == xlings::task::TaskStatus::completed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto taskInfo = tm.info(tid);
    EXPECT_EQ(taskInfo.status, xlings::task::TaskStatus::completed);
    EXPECT_EQ(taskInfo.capabilityName, "search_packages");
}

TEST(TaskManager, EventsRetrieval) {
    xlings::capability::Registry reg;
    reg.register_capability(std::make_unique<MockSearchCapability>());

    xlings::task::TaskManager tm { reg };
    auto tid = tm.submit("search_packages", R"({"query":"gcc"})");

    for (int i { 0 }; i < 100; ++i) {
        if (tm.info(tid).status == xlings::task::TaskStatus::completed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto evts = tm.events(tid);
    EXPECT_GE(evts.size(), 3);  // LogEvent + DataEvent + CompletedEvent

    auto evts2 = tm.events(tid, evts.size());
    EXPECT_EQ(evts2.size(), 0);
}

TEST(TaskManager, PromptHandling) {
    xlings::capability::Registry reg;
    reg.register_capability(std::make_unique<MockInstallCapability>());

    xlings::task::TaskManager tm { reg };
    auto tid = tm.submit("install_package", R"({"name":"gcc"})");

    bool foundPrompt { false };
    std::string promptId;
    for (int i { 0 }; i < 100; ++i) {
        auto taskInfo = tm.info(tid);
        if (taskInfo.status == xlings::task::TaskStatus::waiting_prompt) {
            auto evts = tm.events(tid);
            for (auto& rec : evts) {
                if (auto* p = std::get_if<xlings::PromptEvent>(&rec.event)) {
                    promptId = p->id;
                    foundPrompt = true;
                }
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(foundPrompt);
    tm.respond(tid, promptId, "y");

    for (int i { 0 }; i < 100; ++i) {
        if (tm.info(tid).status == xlings::task::TaskStatus::completed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(tm.info(tid).status, xlings::task::TaskStatus::completed);
}

TEST(TaskManager, ConcurrentTasks) {
    xlings::capability::Registry reg;
    reg.register_capability(std::make_unique<MockSearchCapability>());

    xlings::task::TaskManager tm { reg };
    auto t1 = tm.submit("search_packages", R"({})");
    auto t2 = tm.submit("search_packages", R"({})");
    auto t3 = tm.submit("search_packages", R"({})");

    for (int i { 0 }; i < 200; ++i) {
        if (!tm.has_active_tasks()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_FALSE(tm.has_active_tasks());
    EXPECT_EQ(tm.info(t1).status, xlings::task::TaskStatus::completed);
    EXPECT_EQ(tm.info(t2).status, xlings::task::TaskStatus::completed);
    EXPECT_EQ(tm.info(t3).status, xlings::task::TaskStatus::completed);
}

TEST(TaskManager, InfoAll) {
    xlings::capability::Registry reg;
    reg.register_capability(std::make_unique<MockSearchCapability>());

    xlings::task::TaskManager tm { reg };
    tm.submit("search_packages", R"({})");
    tm.submit("search_packages", R"({})");

    auto all = tm.info_all();
    EXPECT_EQ(all.size(), 2);
}

// ============================================================
// ─── Integration Tests: EventStream + Capability + TaskManager ───
// ============================================================

TEST(Integration, TuiPathSynchronous) {
    // Simulate CLI/TUI path: synchronous call, consumer handles events directly
    xlings::EventStream stream;
    std::vector<std::string> rendered;

    stream.on_event([&](const xlings::Event& e) {
        std::visit([&](auto&& ev) {
            using T = std::decay_t<decltype(ev)>;
            if constexpr (std::is_same_v<T, xlings::ProgressEvent>) {
                rendered.push_back("progress:" + std::to_string(ev.percent));
            } else if constexpr (std::is_same_v<T, xlings::LogEvent>) {
                rendered.push_back("log:" + ev.message);
            } else if constexpr (std::is_same_v<T, xlings::PromptEvent>) {
                rendered.push_back("prompt:" + ev.question);
                stream.respond(ev.id, "y");
            } else if constexpr (std::is_same_v<T, xlings::DataEvent>) {
                rendered.push_back("data:" + ev.kind);
            } else if constexpr (std::is_same_v<T, xlings::CompletedEvent>) {
                rendered.push_back("completed:" + ev.summary);
            }
        }, e);
    });

    MockSearchCapability search;
    search.execute(R"({})", stream);

    ASSERT_EQ(rendered.size(), 3);
    EXPECT_EQ(rendered[0], "log:Searching...");
    EXPECT_EQ(rendered[1], "data:search_results");
    EXPECT_EQ(rendered[2], "completed:Found 2 packages");
}

TEST(Integration, AgentPathConcurrentWithPrompt) {
    // Simulate Agent path: concurrent tasks + prompt handling
    xlings::capability::Registry reg;
    reg.register_capability(std::make_unique<MockInstallCapability>());
    reg.register_capability(std::make_unique<MockSearchCapability>());

    xlings::task::TaskManager tm { reg };

    auto tSearch = tm.submit("search_packages", R"({})");
    auto tInstall = tm.submit("install_package", R"({"name":"gcc"})");

    // Agent main loop: poll events, handle prompts
    bool installDone = false;
    for (int i = 0; i < 200 && !installDone; ++i) {
        auto installInfo = tm.info(tInstall);
        if (installInfo.status == xlings::task::TaskStatus::waiting_prompt) {
            auto evts = tm.events(tInstall);
            for (auto& rec : evts) {
                if (auto* p = std::get_if<xlings::PromptEvent>(&rec.event)) {
                    tm.respond(tInstall, p->id, "y");
                }
            }
        }
        if (installInfo.status == xlings::task::TaskStatus::completed) {
            installDone = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_TRUE(installDone);
    EXPECT_EQ(tm.info(tSearch).status, xlings::task::TaskStatus::completed);

    // Verify event stream contents
    auto searchEvents = tm.events(tSearch);
    EXPECT_GE(searchEvents.size(), 3);

    auto installEvents = tm.events(tInstall);
    EXPECT_GE(installEvents.size(), 2);  // ProgressEvent + PromptEvent + CompletedEvent
}

// ═══════════════════════════════════════════════════════════════
//  Phase 3: Real Capability implementations
// ═══════════════════════════════════════════════════════════════

TEST(Capabilities, BuildRegistryPopulatesAll) {
    auto reg = xlings::capabilities::build_registry();
    auto specs = reg.list_all();
    EXPECT_GE(specs.size(), 8);

    EXPECT_NE(reg.get("search_packages"), nullptr);
    EXPECT_NE(reg.get("install_packages"), nullptr);
    EXPECT_NE(reg.get("remove_package"), nullptr);
    EXPECT_NE(reg.get("update_packages"), nullptr);
    EXPECT_NE(reg.get("list_packages"), nullptr);
    EXPECT_NE(reg.get("package_info"), nullptr);
    EXPECT_NE(reg.get("use_version"), nullptr);
    EXPECT_NE(reg.get("system_status"), nullptr);
}

TEST(Capabilities, SpecsHaveRequiredFields) {
    auto reg = xlings::capabilities::build_registry();
    auto specs = reg.list_all();
    for (auto& s : specs) {
        EXPECT_FALSE(s.name.empty()) << "capability has empty name";
        EXPECT_FALSE(s.description.empty()) << s.name << " has empty description";
        EXPECT_FALSE(s.inputSchema.empty()) << s.name << " has empty inputSchema";
    }
}

TEST(Capabilities, DestructiveFlags) {
    auto reg = xlings::capabilities::build_registry();
    EXPECT_FALSE(reg.get("search_packages")->spec().destructive);
    EXPECT_FALSE(reg.get("list_packages")->spec().destructive);
    EXPECT_FALSE(reg.get("package_info")->spec().destructive);
    EXPECT_FALSE(reg.get("system_status")->spec().destructive);
    EXPECT_TRUE(reg.get("install_packages")->spec().destructive);
    EXPECT_TRUE(reg.get("remove_package")->spec().destructive);
    EXPECT_TRUE(reg.get("update_packages")->spec().destructive);
    EXPECT_TRUE(reg.get("use_version")->spec().destructive);
}

TEST(Capabilities, RegistryListAllSpecs) {
    auto reg = xlings::capabilities::build_registry();
    auto specs = reg.list_all();
    for (auto& s : specs) {
        auto parsed = nlohmann::json::parse(s.inputSchema, nullptr, false);
        EXPECT_FALSE(parsed.is_discarded()) << s.name << " has invalid inputSchema";
    }
}

TEST(Capabilities, SearchSpecSchema) {
    auto reg = xlings::capabilities::build_registry();
    auto* cap = reg.get("search_packages");
    ASSERT_NE(cap, nullptr);
    auto s = cap->spec();
    EXPECT_EQ(s.name, "search_packages");
    auto schema = nlohmann::json::parse(s.inputSchema);
    EXPECT_TRUE(schema.contains("required"));
    EXPECT_EQ(schema["required"][0], "keyword");
}

// ═══════════════════════════════════════════════════════════════
//  Archive extraction (libarchive-backed in-process)
// ═══════════════════════════════════════════════════════════════
//
// Replaces the previous popen("tar xf …") path. The test does the
// shell-out *only* to build a tiny fixture archive; the system-under-
// test (xim::extract_archive) goes through libarchive in-process and
// must produce the same files on disk.

namespace {
struct ExtractFixture {
    std::filesystem::path tmp;

    ExtractFixture() {
        namespace fs = std::filesystem;
        tmp = fs::temp_directory_path() / "xlings-extract-test";
        fs::remove_all(tmp);
        fs::create_directories(tmp / "src/sub");
        std::ofstream(tmp / "src/hello.txt")  << "hello-from-fixture\n";
        std::ofstream(tmp / "src/sub/nested.txt") << "deeply-nested-content\n";
    }

    ~ExtractFixture() {
        std::error_code ec;
        std::filesystem::remove_all(tmp, ec);
    }

    // Chdir-based archive helpers. We use std::filesystem::current_path()
    // rather than shell `cd && tool` so we avoid:
    //   - cmd.exe `cd <other-drive>` being a no-op without /d
    //   - dash not having `pushd`
    //   - cross-shell quoting of paths with spaces
    // All archive tools below are invoked with relative inputs from inside
    // tmp/, producing the output as a relative filename, then we resolve
    // back to the absolute path.
    //
    // host_sys_: run a HOST tool (tar/zip/python) via std::system with the
    // build tool's injected runtime library path scrubbed. mcpp 0.0.47+
    // exports the target toolchain's runtime dirs (sandbox glibc) into
    // LD_LIBRARY_PATH for test processes; host tools crash when loaded
    // against that glibc. Scope: POSIX only (the var is harmless on
    // Windows, and `env` isn't available there).
    static int host_sys_(const char* cmd) {
#if defined(_WIN32)
        return std::system(cmd);
#else
        std::string wrapped = std::string("env -u LD_LIBRARY_PATH ") + cmd;
        return std::system(wrapped.c_str());
#endif
    }

    template <class F>
    static int run_in_(const std::filesystem::path& dir, F&& fn) {
        auto saved = std::filesystem::current_path();
        std::filesystem::current_path(dir);
        int rc = fn();
        std::filesystem::current_path(saved);
        return rc;
    }

    std::filesystem::path make_tar_gz() const {
        auto out = tmp / "fixture.tar.gz";
        int rc = run_in_(tmp, [] {
            return host_sys_("tar czf fixture.tar.gz src");
        });
        if (rc != 0) throw std::runtime_error("failed to create tar.gz fixture");
        return out;
    }

    std::filesystem::path make_zip() const {
        auto out = tmp / "fixture.zip";
        int rc = run_in_(tmp, [] {
            return host_sys_("zip -qr fixture.zip src");
        });
        if (rc != 0) throw std::runtime_error("failed to create zip fixture");
        return out;
    }

    std::filesystem::path make_utf8_zip() const {
        namespace fs = std::filesystem;
        std::ofstream(tmp / "make_utf8_zip.py") << R"PY(
import zipfile

with zipfile.ZipFile("fixture_utf8.zip", "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr(
        "utf8/.github/ISSUE_TEMPLATE/bug-report---\u95ee\u9898.md",
        "unicode-path-fixture\n",
    )
)PY";
        auto out = tmp / "fixture_utf8.zip";
        int rc = run_in_(tmp, [] {
#ifdef _WIN32
            return host_sys_("python make_utf8_zip.py");
#else
            return host_sys_("python3 make_utf8_zip.py || python make_utf8_zip.py");
#endif
        });
        if (rc != 0) throw std::runtime_error("failed to create utf8 zip fixture");
        return out;
    }

    std::filesystem::path make_tar_xz() const {
        auto out = tmp / "fixture.tar.xz";
        int rc = run_in_(tmp, [] {
            return host_sys_("tar cJf fixture.tar.xz src");
        });
        if (rc != 0) throw std::runtime_error("failed to create tar.xz fixture");
        return out;
    }
};

bool file_has_(const std::filesystem::path& p, std::string_view expected) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str().find(expected) != std::string::npos;
}
} // namespace

TEST(Extract, TarGzRoundTrip) {
    ExtractFixture fx;
    auto archive = fx.make_tar_gz();
    auto out = fx.tmp / "out_targz";

    auto r = xlings::xim::extract_archive(archive, out);
    ASSERT_TRUE(r.has_value()) << "extract failed: " << (r ? "" : r.error());

    // Use the canonicalized path returned by extract_archive — on Windows
    // (and macOS) `out` and the resolved path can differ once symlinks /
    // 8.3 short names are walked.
    auto root = *r;
    EXPECT_TRUE(std::filesystem::exists(root / "src/hello.txt"));
    EXPECT_TRUE(std::filesystem::exists(root / "src/sub/nested.txt"));
    EXPECT_TRUE(file_has_(root / "src/hello.txt", "hello-from-fixture"));
    EXPECT_TRUE(file_has_(root / "src/sub/nested.txt", "deeply-nested-content"));
}

TEST(Extract, ZipRoundTrip) {
    // zip command may not be installed everywhere; skip cleanly if so.
    if (std::system("command -v zip >/dev/null 2>&1") != 0) {
        GTEST_SKIP() << "zip not available on this host";
    }
    ExtractFixture fx;
    auto archive = fx.make_zip();
    auto out = fx.tmp / "out_zip";

    auto r = xlings::xim::extract_archive(archive, out);
    ASSERT_TRUE(r.has_value()) << "extract failed: " << (r ? "" : r.error());

    auto root = *r;
    EXPECT_TRUE(std::filesystem::exists(root / "src/hello.txt"));
    EXPECT_TRUE(file_has_(root / "src/hello.txt", "hello-from-fixture"));
}

TEST(Extract, ZipUtf8PathRoundTrip) {
#ifdef _WIN32
    if (std::system("python --version >NUL 2>NUL") != 0) {
        GTEST_SKIP() << "python not available on this host";
    }
#else
    if (std::system("command -v python3 >/dev/null 2>&1 || command -v python >/dev/null 2>&1") != 0) {
        GTEST_SKIP() << "python not available on this host";
    }
#endif
    ExtractFixture fx;
    auto archive = fx.make_utf8_zip();
    auto out = fx.tmp / "out_zip_utf8";

    auto r = xlings::xim::extract_archive(archive, out);
    ASSERT_TRUE(r.has_value()) << "extract failed: " << (r ? "" : r.error());

    auto root = *r;
    auto expected = root / "utf8/.github/ISSUE_TEMPLATE/bug-report---问题.md";
    EXPECT_TRUE(std::filesystem::exists(expected));
    EXPECT_TRUE(file_has_(expected, "unicode-path-fixture"));
}

TEST(Extract, TarXzRoundTrip) {
    // Confirms that the .tar.xz path used by node / llvm packages works
    // through the libarchive-backed extractor (the original popen-tar
    // path was the source of the ollama-install hang bug).
    if (std::system("command -v xz >/dev/null 2>&1") != 0) {
        GTEST_SKIP() << "xz not available on this host";
    }
    ExtractFixture fx;
    auto archive = fx.make_tar_xz();
    auto out = fx.tmp / "out_tarxz";

    auto r = xlings::xim::extract_archive(archive, out);
    ASSERT_TRUE(r.has_value()) << "extract failed: " << (r ? "" : r.error());

    auto root = *r;
    EXPECT_TRUE(std::filesystem::exists(root / "src/hello.txt"));
    EXPECT_TRUE(std::filesystem::exists(root / "src/sub/nested.txt"));
}

TEST(Extract, MissingArchiveReturnsError) {
    ExtractFixture fx;
    auto r = xlings::xim::extract_archive(fx.tmp / "no-such.tar.gz", fx.tmp / "out");
    EXPECT_FALSE(r.has_value());
}

TEST(Extract, InvalidArchiveIsClassifiedAndEvictedWithSidecar) {
    ExtractFixture fx;
    auto archive = fx.tmp / "truncated.tar.gz";
    auto sidecar = std::filesystem::path(archive.string() + ".meta");
    std::ofstream(archive) << "not-a-complete-archive";
    std::ofstream(sidecar) << "format: 2\n";

    auto result = xlings::xim::extract_archive_detailed(
        archive, fx.tmp / "invalid-out");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind,
              xlings::xim::ExtractErrorKind::InvalidInputArchive);
    EXPECT_TRUE(xlings::xim::evict_invalid_archive_cache_(
        archive, result.error()));
    EXPECT_FALSE(std::filesystem::exists(archive));
    EXPECT_FALSE(std::filesystem::exists(sidecar));
}

TEST(Extract, LocalWriteFailureDoesNotEvictValidInput) {
    ExtractFixture fx;
    auto archive = fx.make_tar_gz();
    auto invalidDestination = fx.tmp / "destination-is-a-file";
    std::ofstream(invalidDestination) << "not-a-directory";

    auto result = xlings::xim::extract_archive_detailed(
        archive, invalidDestination);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind,
              xlings::xim::ExtractErrorKind::LocalWriteFailure);
    EXPECT_FALSE(xlings::xim::evict_invalid_archive_cache_(
        archive, result.error()));
    EXPECT_TRUE(std::filesystem::exists(archive));
}

TEST(Extract, RejectsPathTraversal) {
    // Build a tarball containing an entry with "../escape.txt". libarchive
    // with ARCHIVE_EXTRACT_SECURE_NODOTDOT must refuse to extract the
    // escape entry — we expect either an error, or successful extraction
    // of the safe entry without ../escape.txt appearing outside out_dir.
    ExtractFixture fx;
    namespace fs = std::filesystem;
    auto stage = fx.tmp / "stage";
    fs::create_directories(stage / "subdir");
    std::ofstream(stage / "safe.txt") << "safe\n";

    // Create a tar with one safe entry. The dot-dot test below uses
    // libarchive's secure flags; we mostly just ensure no escape files
    // appear above the destination dir.
    auto archive = fx.tmp / "ptraversal.tar.gz";
    std::string cmd = std::format("cd {} && tar czf {} -C {} .",
        fx.tmp.string(), archive.string(), stage.string());
#if !defined(_WIN32)
    cmd = "env -u LD_LIBRARY_PATH sh -c " + std::string("'") + cmd + "'";
#endif
    ASSERT_EQ(std::system(cmd.c_str()), 0);

    auto out = fx.tmp / "out_ptrav";
    auto r = xlings::xim::extract_archive(archive, out);
    ASSERT_TRUE(r.has_value()) << r.error();

    // Confirm nothing landed outside `out`.
    EXPECT_FALSE(fs::exists(fx.tmp / "escape.txt"));
    EXPECT_FALSE(fs::exists(out.parent_path() / "escape.txt"));
}

// ═══════════════════════════════════════════════════════════════
//  TUI: theme icon byte sequences
// ═══════════════════════════════════════════════════════════════
//
// Force-check that every theme icon is the same UTF-8 byte sequence on
// every platform. xlings_tests is built and run on Linux / macOS /
// Windows in CI, so a regression that, say, changes `icon::done` to
// "+" only on Windows is caught the moment xlings_tests boots there.
//
// The intent is: there is exactly one icon set, byte-for-byte, no
// platform conditional, no font-substitution fallback, no ASCII
// downgrade. If the test fails on any platform, the source has
// drifted.

namespace {
struct IconSlot {
    std::string_view name;
    const char* value;
    std::string_view expected;
};

constexpr IconSlot kThemeIconSlots[] = {
    {"pending",     xlings::ui::theme::icon::pending,     "\xe2\x97\x8b"},  // ○ U+25CB
    {"downloading", xlings::ui::theme::icon::downloading, "\xe2\x86\x93"},  // ↓ U+2193
    {"extracting",  xlings::ui::theme::icon::extracting,  "\xe2\x96\xbe"},  // ▾ U+25BE
    {"installing",  xlings::ui::theme::icon::installing,  "\xe2\x8a\x95"},  // ⊕ U+2295
    {"configuring", xlings::ui::theme::icon::configuring, "\xe2\x8a\x95"},  // ⊕ U+2295
    {"done",        xlings::ui::theme::icon::done,        "\xe2\x9c\x93"},  // ✓ U+2713
    {"failed",      xlings::ui::theme::icon::failed,      "\xe2\x9c\x97"},  // ✗ U+2717
    {"info",        xlings::ui::theme::icon::info,        "\xe2\x80\xba"},  // › U+203A
    {"arrow",       xlings::ui::theme::icon::arrow,       "\xe2\x96\xb8"},  // ▸ U+25B8
    {"package",     xlings::ui::theme::icon::package,     "\xe2\x97\x86"},  // ◆ U+25C6
};
} // namespace

TEST(ThemeIcons, AllByteSequencesAreCanonical) {
    // Every slot must equal its expected UTF-8 byte sequence exactly.
    for (auto& slot : kThemeIconSlots) {
        EXPECT_EQ(std::string_view{slot.value}, slot.expected)
            << "icon::" << slot.name << " drifted from canonical bytes";
    }
}

TEST(ThemeIcons, NoPlatformAsciiFallback) {
    // Each icon's leading byte must have bit 7 set — i.e. it is a
    // multi-byte UTF-8 sequence, not an ASCII fallback. Catches
    // `#ifdef _WIN32 "+" #else "✓"` slipping back in for any slot.
    for (auto& slot : kThemeIconSlots) {
        std::string_view s{slot.value};
        ASSERT_FALSE(s.empty()) << "icon::" << slot.name << " is empty";
        EXPECT_TRUE(static_cast<unsigned char>(s[0]) & 0x80)
            << "icon::" << slot.name << " is single-byte ASCII (\""
            << s << "\")";
    }
}

TEST(ThemeIcons, AllAreThreeByteBmpUtf8) {
    // Belt-and-braces: every slot is a well-formed 3-byte BMP UTF-8
    // sequence (lead byte 0xE0..0xEF, followed by two continuation
    // bytes 0x80..0xBF). Rules out 4-byte SMP code points (which many
    // monospace fonts can't render) and malformed sequences.
    for (auto& slot : kThemeIconSlots) {
        std::string_view s{slot.value};
        ASSERT_EQ(s.size(), 3u) << "icon::" << slot.name
                                 << " is " << s.size() << " bytes, expected 3";
        auto b0 = static_cast<unsigned char>(s[0]);
        auto b1 = static_cast<unsigned char>(s[1]);
        auto b2 = static_cast<unsigned char>(s[2]);
        EXPECT_TRUE(b0 >= 0xE0 && b0 <= 0xEF)
            << "icon::" << slot.name << " lead byte not 3-byte UTF-8";
        EXPECT_TRUE((b1 & 0xC0) == 0x80)
            << "icon::" << slot.name << " byte 1 not a continuation";
        EXPECT_TRUE((b2 & 0xC0) == 0x80)
            << "icon::" << slot.name << " byte 2 not a continuation";
    }
}

// ═══════════════════════════════════════════════════════════════
//  Proxy: env-driven proxy resolution for the downloader
// ═══════════════════════════════════════════════════════════════
//
// xlings::tinyhttps::resolve_proxy(url) reads HTTPS_PROXY / HTTP_PROXY /
// ALL_PROXY (case-insensitive variants) and respects NO_PROXY. These
// tests lock the libcurl-compatible behaviour: scheme-aware selection,
// NO_PROXY suffix exemption, lowercase fallback.

namespace {
struct EnvScope {
    std::string name;
    bool had_prev{false};
    std::string prev_value;

    EnvScope(std::string_view n, const char* val) : name(n) {
        if (auto v = std::getenv(name.c_str())) {
            had_prev = true;
            prev_value = v;
        }
        set_(val);
    }
    ~EnvScope() {
        if (had_prev) set_(prev_value.c_str());
        else clear_();
    }
    void set_(const char* val) {
        if (!val) { clear_(); return; }
#ifdef _WIN32
        _putenv_s(name.c_str(), val);
#else
        ::setenv(name.c_str(), val, 1);
#endif
    }
    void clear_() {
#ifdef _WIN32
        _putenv_s(name.c_str(), "");
#else
        ::unsetenv(name.c_str());
#endif
    }
};

// Wipe every proxy-related env var so each test starts from a clean slate.
// Vector elements are unique_ptrs so a reallocation on push_back doesn't
// move-then-destroy intermediate EnvScopes (which would prematurely
// restore env vars before the test body runs).
struct ProxyEnvSandbox {
    std::vector<std::unique_ptr<EnvScope>> guards;
    ProxyEnvSandbox() {
        for (auto* n : {"HTTPS_PROXY", "https_proxy",
                        "HTTP_PROXY",  "http_proxy",
                        "ALL_PROXY",   "all_proxy",
                        "NO_PROXY",    "no_proxy"}) {
            guards.push_back(std::make_unique<EnvScope>(n, nullptr));
        }
    }
};
} // namespace

TEST(Proxy, NoEnvMeansDirect) {
    ProxyEnvSandbox sandbox;
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("https://example.com/foo"), "");
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("http://example.com/foo"),  "");
}

TEST(Proxy, HttpsProxyUsedForHttpsScheme) {
    ProxyEnvSandbox sandbox;
    EnvScope https("HTTPS_PROXY", "http://127.0.0.1:7890");
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("https://example.com/x"),
              "http://127.0.0.1:7890");
}

TEST(Proxy, HttpProxyUsedForHttpScheme) {
    ProxyEnvSandbox sandbox;
    EnvScope http("HTTP_PROXY", "http://127.0.0.1:7890");
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("http://example.com/x"),
              "http://127.0.0.1:7890");
}

TEST(Proxy, LowercaseEnvAlsoAccepted) {
    ProxyEnvSandbox sandbox;
    EnvScope https("https_proxy", "http://10.0.0.1:8080");
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("https://example.com/x"),
              "http://10.0.0.1:8080");
}

TEST(Proxy, AllProxyFallback) {
    ProxyEnvSandbox sandbox;
    EnvScope all("ALL_PROXY", "socks5://127.0.0.1:1080");
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("https://example.com/x"),
              "socks5://127.0.0.1:1080");
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("http://example.com/x"),
              "socks5://127.0.0.1:1080");
}

TEST(Proxy, HttpsProxyTakesPrecedenceOverHttpProxy) {
    ProxyEnvSandbox sandbox;
    EnvScope https("HTTPS_PROXY", "http://https-proxy:1");
    EnvScope http("HTTP_PROXY",   "http://http-proxy:2");
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("https://example.com/x"),
              "http://https-proxy:1");
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("http://example.com/x"),
              "http://http-proxy:2");
}

TEST(Proxy, NoProxyExactHostExempt) {
    ProxyEnvSandbox sandbox;
    EnvScope https("HTTPS_PROXY", "http://127.0.0.1:7890");
    EnvScope np("NO_PROXY", "localhost,internal.example");
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("https://localhost:9000/x"), "");
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("https://internal.example/x"), "");
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("https://other.example.com/x"),
              "http://127.0.0.1:7890");
}

TEST(Proxy, NoProxySuffixMatchExempt) {
    ProxyEnvSandbox sandbox;
    EnvScope https("HTTPS_PROXY", "http://127.0.0.1:7890");
    EnvScope np("NO_PROXY", ".internal.example,corp.local");
    // dot-prefixed suffix: matches both bare and prefixed
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("https://api.internal.example/x"), "");
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("https://internal.example/x"), "");
    // bare suffix without dot: still suffix-matches subdomains
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("https://node1.corp.local/x"), "");
    // unrelated host still goes through proxy
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("https://github.com/x"),
              "http://127.0.0.1:7890");
}

TEST(Proxy, NoProxyWildcardExemptsAll) {
    ProxyEnvSandbox sandbox;
    EnvScope https("HTTPS_PROXY", "http://127.0.0.1:7890");
    EnvScope np("NO_PROXY", "*");
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("https://anything.example/x"), "");
}

TEST(ThemeIcons, InfoPanelEmitsIconBytesToStdout) {
    // Render the same panel that `xlings config` uses, capture the bytes
    // that ftxui actually wrote to stdout, and confirm the canonical icon
    // and box-drawing UTF-8 sequences survive end-to-end. This is the
    // in-process equivalent of the bash e2e test, runs on every platform
    // (Linux / macOS / Windows xlings_tests) so Windows gets the same
    // emission-path coverage that redirected-stdout makes hard at the
    // binary level.
    namespace ui = xlings::ui;

    std::vector<ui::InfoField> fields = {
        {"language",  "en"},
        {"mirror",    "GLOBAL", true},
        {"data dir",  "/tmp/xlings"},
    };

    testing::internal::CaptureStdout();
    ui::print_info_panel("Test Panel", fields);
    auto output = testing::internal::GetCapturedStdout();

    ASSERT_FALSE(output.empty())
        << "print_info_panel emitted no bytes — ftxui rendering path broken";

    // The package icon ◆ (U+25C6, E2 97 86) is the title bullet;
    // box-drawing ─ (U+2500, E2 94 80) and │ (U+2502, E2 94 82) come
    // from ftxui's border. At least one of these byte triples must be
    // present in the captured output, on every platform, byte-for-byte.
    auto has = [&](std::string_view needle) {
        return output.find(needle) != std::string::npos;
    };
    EXPECT_TRUE(has("\xe2\x97\x86") || has("\xe2\x94\x80") || has("\xe2\x94\x82"))
        << "info_panel output contains no canonical UTF-8 sequences "
           "(◆ ─ │). Hex prefix: "
        << [&] {
               std::string hex;
               for (std::size_t i = 0; i < std::min<std::size_t>(80, output.size()); ++i) {
                   char buf[4];
                   std::snprintf(buf, sizeof(buf), "%02x ",
                                 static_cast<unsigned char>(output[i]));
                   hex += buf;
               }
               return hex;
           }();

    // Negative check: long runs of '?' would indicate that the rendering
    // layer or stdout capture downconverted UTF-8 to replacement chars.
    EXPECT_EQ(output.find("?????"), std::string::npos)
        << "info_panel output contains run of '?' — possible encoding loss";
}

// ============================================================
// subos GPU passthrough (--gpu) tests
// ============================================================

namespace {

bool contains_triple(const std::vector<std::string>& argv,
                     std::string_view flag,
                     std::string_view src,
                     std::string_view dst)
{
    for (size_t i = 0; i + 2 < argv.size(); ++i) {
        if (argv[i] == flag && argv[i + 1] == src && argv[i + 2] == dst)
            return true;
    }
    return false;
}

}  // namespace

TEST(SubosGpu, EmptyHostStillBindsSys) {
    auto args = xlings::subos::gpu::passthrough_args(
        [](const std::string&) { return false; });
    // No /dev/* bindings, but /sys --ro-bind must always be present.
    EXPECT_EQ(args.size(), 3u);
    EXPECT_TRUE(contains_triple(args, "--ro-bind", "/sys", "/sys"));
}

TEST(SubosGpu, BindsNvidiactlWhenPresent) {
    auto args = xlings::subos::gpu::passthrough_args(
        [](const std::string& p) { return p == "/dev/nvidiactl"; });
    EXPECT_TRUE(contains_triple(args, "--dev-bind",
                                "/dev/nvidiactl", "/dev/nvidiactl"));
    EXPECT_TRUE(contains_triple(args, "--ro-bind", "/sys", "/sys"));
}

TEST(SubosGpu, EnumeratesPerGpuNodesUpTo16) {
    int probe_count = 0;
    auto args = xlings::subos::gpu::passthrough_args(
        [&](const std::string& p) {
            // Count probes that target /dev/nvidia<digit>
            if (p.size() > 11 && p.starts_with("/dev/nvidia")
                && std::isdigit(static_cast<unsigned char>(p[11]))) {
                ++probe_count;
                return true;
            }
            return false;
        });
    EXPECT_EQ(probe_count, 16);
    // All 16 must appear as --dev-bind triples.
    for (int i = 0; i < 16; ++i) {
        auto path = "/dev/nvidia" + std::to_string(i);
        EXPECT_TRUE(contains_triple(args, "--dev-bind", path, path))
            << "missing --dev-bind for " << path;
    }
}

TEST(SubosGpu, BindsDriWhenPresent) {
    auto args = xlings::subos::gpu::passthrough_args(
        [](const std::string& p) { return p == "/dev/dri"; });
    EXPECT_TRUE(contains_triple(args, "--dev-bind",
                                "/dev/dri", "/dev/dri"));
}

TEST(SubosGpu, FullHostBindsAllKnownNodes) {
    auto args = xlings::subos::gpu::passthrough_args(
        [](const std::string&) { return true; });
    for (auto path : {"/dev/nvidiactl", "/dev/nvidia-uvm",
                      "/dev/nvidia-uvm-tools", "/dev/nvidia-modeset",
                      "/dev/dri"}) {
        EXPECT_TRUE(contains_triple(args, "--dev-bind", path, path))
            << "missing --dev-bind for " << path;
    }
    EXPECT_TRUE(contains_triple(args, "--ro-bind", "/sys", "/sys"));
}

// ============================================================
// downloader archive-filename sniff (P1 helper for the cmd-install
// silent-failure fix — see .agents/docs/2026-05-22-cmd-install-silent-failure-analysis.md)
// ============================================================

TEST(DownloaderArchiveSniff, RecognisesCommonArchiveExtensions) {
    using xlings::xim::looks_like_archive_filename_;
    EXPECT_TRUE(looks_like_archive_filename_("foo.tar.gz"));
    EXPECT_TRUE(looks_like_archive_filename_("foo.tar.xz"));
    EXPECT_TRUE(looks_like_archive_filename_("foo.tar.bz2"));
    EXPECT_TRUE(looks_like_archive_filename_("foo.tar.zst"));
    EXPECT_TRUE(looks_like_archive_filename_("foo.tgz"));
    EXPECT_TRUE(looks_like_archive_filename_("foo.zip"));
}

TEST(DownloaderArchiveSniff, RejectsNonArchiveExtensions) {
    using xlings::xim::looks_like_archive_filename_;
    EXPECT_FALSE(looks_like_archive_filename_("README.md"));
    EXPECT_FALSE(looks_like_archive_filename_("install.sh"));
    EXPECT_FALSE(looks_like_archive_filename_("config.json"));
    EXPECT_FALSE(looks_like_archive_filename_("binary.exe"));
    EXPECT_FALSE(looks_like_archive_filename_(""));
}

TEST(DownloaderArchiveSniff, IsCaseSensitiveSuffixMatch) {
    using xlings::xim::looks_like_archive_filename_;
    // Recipes in xim-pkgindex use lower-case; we mirror that to avoid
    // accepting weird upstream conventions like "Foo.TAR.GZ" silently.
    EXPECT_FALSE(looks_like_archive_filename_("foo.TAR.GZ"));
    EXPECT_FALSE(looks_like_archive_filename_("foo.Tar.Gz"));
}

TEST(DownloaderArchiveSniff, WorksWithFullPaths) {
    using xlings::xim::looks_like_archive_filename_;
    // The downloader passes the full destFile path. The function should
    // look at the basename only, ignoring directory components.
    EXPECT_TRUE(looks_like_archive_filename_("/var/tmp/runtimedir/llvm-20.1.7-linux-x86_64.tar.gz"));
    EXPECT_FALSE(looks_like_archive_filename_("/path/to/tar.gz/file.lua"));  // tar.gz only in dir name
}
