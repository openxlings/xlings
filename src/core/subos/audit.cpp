// `subos requests / approve / deny / report / ps / log`: what happened in an
// instance, read and answered from outside it (module xlings.core.subos).
module xlings.core.subos;

import std;
import xlings.core.config;
import xlings.core.home_config;
import xlings.libs.json;
import xlings.core.log;
import xlings.platform;
import xlings.runtime;
import xlings.core.utils;
import xlings.core.xself;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xim.commands;
import xlings.subos.gpu;
import xlings.subos.graphics;
import xlings.core.subos.sandbox;
import xlings.subos.manifest;
import xlings.cli.spec;
import xlings.i18n;
import xlings.core.confirm;
import xlings.core.destructive_log;
import xlings.subos.userdata;
import xlings.subos.model;
import xlings.core.subos.ports;
import xlings.subos.session;
import xlings.subos.policy;
import xlings.subos.policy_store;
import xlings.subos.broker;
import xlings.subos.home_view;
import xlings.observe;
import xlings.core.home;
import xlings.core.home.domain_producer;
import xlings.core.home.prefix_domain;
import xlings.libs.sha256;
import xlings.core.version_order;

namespace xlings::subos {

// The homes a system's records may be in (part 2 J40): this one, and the
// domain's when the system lives in one -- a session started from here keeps
// its requests, audit and session files here, one started where the system
// lives (luban's) keeps them there. Read from both; nothing is entered.
std::vector<std::pair<HomeView, std::optional<xlings::home::prefix_domain::Domain>>>
record_homes_(std::string_view name) {
    std::vector<std::pair<HomeView, std::optional<xlings::home::prefix_domain::Domain>>> out{{home_view(), std::nullopt}};
    if (auto scope = xlings::home::domain_producer::read_scope(Config::paths().homeDir, name); scope && *scope)
        out.push_back({HomeView{(**scope).domain.physicalHome}, (**scope).domain});
    return out;
}

// A system's events, from every home it keeps them in, in time order.
std::vector<nlohmann::json> events_of_(std::string_view name) {
    std::vector<nlohmann::json> all;
    for (const auto& [home, domain] : record_homes_(name))
        for (auto& e : observe::read(home.logs_dir(name) / "events.ndjson")) all.push_back(std::move(e));
    std::ranges::stable_sort(all, {}, [](const nlohmann::json& e) { return e.value("ts", std::string{}); });
    return all;
}

// `subos requests / approve / deny <name> [id]`: what a sandbox asked for
// under fetch=ask (design §8.2). Only from outside -- the sandbox cannot see
// the queue, and approval never travels through a channel it can forge.
int run_requests_(std::string_view sub, int argc, char* argv[], EventStream& stream,
                  const std::function<void(std::string_view)>& usageError) {
    std::vector<std::string> pos;
    bool json = false;
    for (int i = 3; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--json") json = true;
        else if (!a.empty() && a[0] != '-') pos.emplace_back(a);
        else { usageError(std::format("unknown option for `xlings subos {}`: {}", sub, a)); return 1; }
    }
    const std::size_t want = sub == "requests" ? 1 : 2;
    if (pos.size() != want) {
        usageError(sub == "requests" ? "usage: xlings subos requests <name>"
                                     : std::format("usage: xlings subos {} <name> <id>", sub));
        return 1;
    }
    if (utils::get_env_or_default("XLINGS_SUBOS_MODE") == "sandbox") {
        stream.emit(ErrorEvent{ .code = ErrorCode::Permission,
            .message = "E_PERMISSION: requests are answered by the owner, outside the sandbox",
            .recoverable = false });
        return 13;
    }
    const auto home = home_view();
    const auto& name = pos[0];
    const auto homes = record_homes_(name);
    if (sub == "requests") {
        std::vector<subos::broker::Request> reqs;
        for (const auto& [where, domain] : homes)
            for (auto& r : subos::broker::pending(where, name)) reqs.push_back(std::move(r));
        std::ranges::stable_sort(reqs, {}, &subos::broker::Request::created);
        if (json) {
            for (auto& r : reqs)
                std::println(std::cout, "{}", nlohmann::json{{"id", r.id}, {"argv", r.argv}, {"created", r.created},
                                                  {"reason", r.reason}}.dump());
            return 0;
        }
        if (reqs.empty()) { log::info("'{}' has no pending requests", name); return 0; }
        for (auto& r : reqs) {
            std::string line;
            for (auto& a : r.argv) line += " " + a;
            std::println(std::cout, "{}  {}  xlings{}   ({})", r.id, r.created, line, r.reason);
        }
        return 0;
    }
    // Queued where the system lives: answered there, and what is approved runs
    // there (J37).
    for (const auto& [where, domain] : homes) {
        if (!domain) continue;
        const auto queued = subos::broker::pending(where, name);
        if (std::ranges::none_of(queued, [&](const auto& q) { return q.id == pos[1]; })) continue;
        std::vector<std::string> again{"subos", std::string(sub), name, pos[1]};
        auto rc = xlings::home::domain_producer::run(*domain, again);
        if (!rc) {
            stream.emit(ErrorEvent{.code = ErrorCode::Internal, .message = rc.error(), .recoverable = true});
            return 1;
        }
        return *rc;
    }
    auto r = subos::broker::take(home, name, pos[1]);
    if (!r) {
        stream.emit(ErrorEvent{ .code = ErrorCode::NotFound,
            .message = std::format("no pending request {} for '{}'", pos[1], name),
            .recoverable = false, .hint = "xlings subos requests " + name });
        return 1;
    }
    const auto logfile = home.logs_dir(name) / "events.ndjson";
    if (sub == "deny") {
        observe::append(logfile, observe::Event{ .kind = observe::Kind::Perm,
            .fields = {{"event", "request-denied"}, {"instance", name}, {"request", r->id},
                       {"program", r->argv.empty() ? "" : r->argv[0]}}});
        log::info("denied {}", r->id);
        return 0;
    }
    // Approved: run it for that instance, as the broker would have.
    observe::append(logfile, observe::Event{ .kind = observe::Kind::Perm,
        .fields = {{"event", "request-approved"}, {"instance", name}, {"request", r->id},
                   {"program", r->argv.empty() ? "" : r->argv[0]}}});
    std::vector<std::string> full{ platform::get_executable_path().string() };
    full.insert(full.end(), r->argv.begin(), r->argv.end());
    if (std::ranges::find(full, std::string("-y")) == full.end()) full.push_back("-y");
    platform::set_env_variable("XLINGS_ACTIVE_SUBOS", name);
    const int rc = platform::run_argv(full);
    observe::append(logfile, observe::Event{ .kind = observe::Kind::Perm,
        .fields = {{"event", "request-done"}, {"instance", name}, {"request", r->id}, {"exit", rc}}});
    return rc;
}

// `subos __broker-serve <name> [--actor <who>] -- <argv...>`: a machine's
// broker (design 2026-10-21 §3.3). A Luban machine's supervisor runs it for
// a request its guest agent took from root inside (`sudo xlings ...`), on the
// terminal or pipes it relays to the caller: the verdict a sandbox's
// supervisor gives (broker::judge, with the instance's policy), recorded in
// the instance's audit with who asked, then the command run for the
// instance, as the owner. Plumbing: in no help, no completion.
int run_broker_serve_(int argc, char* argv[], const std::function<void(std::string_view)>& usageError) {
    std::string name, actor;
    std::vector<std::string> request;
    for (int i = 3; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--") {
            for (int k = i + 1; k < argc; ++k) request.emplace_back(argv[k]);
            break;
        }
        if (a == "--actor" && i + 1 < argc) actor = argv[++i];
        else if (!a.empty() && a[0] != '-' && name.empty()) name = std::string(a);
        else { usageError(std::format("unknown option for `xlings subos __broker-serve`: {}", a)); return 1; }
    }
    if (name.empty() || request.empty()) {
        usageError("usage: xlings subos __broker-serve <name> [--actor <who>] -- <argv...>");
        return 1;
    }
    // A system in a prefix domain is answered where it lives (part 3 §3.4):
    // its policy, its audit and its store are its home's, not this one's --
    // an install run here went into the owner's copy of it, which the
    // machine never reads (luban-os-ci L2-02, a desktop in its own userland).
    if (auto scope = xlings::home::domain_producer::read_scope(Config::paths().homeDir, name); !scope) {
        std::println(std::cerr, "[xlings] {}", scope.error());
        return subos::broker::kExitPermission;
    } else if (*scope) {
        std::vector<std::string> again{"subos", "__broker-serve", name};
        if (!actor.empty()) again.insert(again.end(), {"--actor", actor});
        again.emplace_back("--");
        again.insert(again.end(), request.begin(), request.end());
        auto rc = xlings::home::domain_producer::run((**scope).domain, again);
        if (!rc) {
            std::println(std::cerr, "[xlings] {}", rc.error());
            return subos::broker::kExitPermission;
        }
        return *rc;
    }
    const auto home = home_view();
    const auto logfile = home.logs_dir(name) / "events.ndjson";
    auto policy = subos::policy_store::effective(home, name, std::nullopt, {}, Info::VERSION);
    if (!policy) {
        std::println(std::cerr, "[xlings] E_PERMISSION: {}'s policy could not be read: {}", name, policy.error());
        return subos::broker::kExitPermission;
    }
    auto verdict = subos::broker::judge(home, *policy, name, request, "on the host");
    auto event = verdict.event;
    event["instance"] = name;
    event["via"] = "machine";
    if (!actor.empty()) event["actor"] = actor;
    if (!observe::append_checked(logfile, observe::Event{.kind = observe::Kind::Perm, .fields = event})
        && policy->preset == subos::policy::Preset::Locked) {
        std::println(std::cerr, "[xlings] E_AUDIT_WRITE: cannot write {}", logfile.string());
        return subos::session::kExitSetup;
    }
    if (!verdict.run) {
        if (!verdict.error.empty()) std::println(std::cerr, "[xlings] {}", verdict.error);
        if (!verdict.hint.empty()) std::println(std::cerr, "[xlings] {}", verdict.hint);
        return verdict.exit;
    }
    // As a sandbox's broker runs it: this host's environment, acting on this
    // instance, from the home (never the caller's project).
    std::vector<std::string> full{platform::get_executable_path().string()};
    full.insert(full.end(), request.begin(), request.end());
    platform::set_env_variable("XLINGS_ACTIVE_SUBOS", name);
    for (const auto* k : {"XLINGS_SUBOS_MODE", "XLINGS_PROJECT_DIR"}) platform::unset_env_variable(k);
    if (!platform::stdin_is_terminal()) platform::set_env_variable("XLINGS_NON_INTERACTIVE", "1");
    std::error_code ec;
    fs::current_path(home.home, ec);
    const int rc = platform::run_argv(full);
    observe::append(logfile, observe::Event{.kind = observe::Kind::Perm,
        .fields = {{"event", "broker-done"}, {"instance", name}, {"via", "machine"}, {"actor", actor},
                   {"program", request.front()}, {"exit", rc}}});
    return rc;
}

// `subos report <name> [--session ID] [--json]`: the audit, summarised
// (design §22) -- what an agent did in an instance, in one screen.
int run_report_(int argc, char* argv[], EventStream& stream,
                const std::function<void(std::string_view)>& usageError) {
    std::string name, only;
    bool json = false;
    for (int i = 3; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--json") json = true;
        else if (a == "--session" && i + 1 < argc) only = argv[++i];
        else if (!a.empty() && a[0] != '-' && name.empty()) name = std::string(a);
        else { usageError("unknown option for `xlings subos report`: " + std::string(a)); return 1; }
    }
    if (name.empty()) {
        int rc = 0;
        name = pick_subos_or_fail_("report", stream, usageError, &rc);
        if (name.empty()) return rc;
    }
    struct Session {
        std::string id, started, backend;
        long long ms = -1;
        std::optional<int> exit;
        bool trace = false;
        std::map<std::string, int> programs;
        int execs = 0, commands = 0;
        std::map<std::string, int> perm;
        std::vector<std::string> denied, changed;
    };
    std::map<std::string, Session> sessions;
    std::vector<std::string> order, policy_changes;
    for (auto& e : events_of_(name)) {
        const auto ev = e.value("event", "");
        if (ev == "policy-change" || ev == "policy-reset") {
            policy_changes.push_back(e.value("ts", "") + " " + ev);
            continue;
        }
        const auto id = e.value("session", "");
        if (id.empty() || (!only.empty() && id != only)) continue;
        if (!sessions.contains(id)) order.push_back(id);
        auto& s = sessions[id];
        s.id = id;
        const auto kind = e.value("kind", "");
        if (ev == "session-start") {
            s.started = e.value("ts", "");
            s.backend = e.value("backend", "");
            s.trace = e.value("exec_trace", false);
        } else if (ev == "session-end") {
            s.ms = e.value("ms", -1LL);
            s.exit = e.value("exit", 0);
        } else if (kind == "exec") {
            ++s.execs;
            ++s.programs[e.value("path", "?")];
        } else if (kind == "ops" && ev == "exec") {
            ++s.commands;
        } else if (kind == "perm" && ev == "decision") {
            auto action = e.value("action", "");
            ++s.perm[action];
            if (action == "deny") s.denied.push_back(e.value("program", "") + ": " + e.value("reason", ""));
        } else if (kind == "fs") {
            for (auto& f : e.value("files", nlohmann::json::array()))
                s.changed.push_back(e.value("mount", "") + "/" + f.get<std::string>());
        }
    }
    if (json) {
        nlohmann::json out{{"instance", name}, {"sessions", nlohmann::json::array()},
                           {"policy_changes", policy_changes}};
        for (auto& id : order) {
            auto& s = sessions[id];
            nlohmann::json top = nlohmann::json::object();
            for (auto& [p, n] : s.programs) top[p] = n;
            out["sessions"].push_back({{"id", s.id}, {"started", s.started}, {"backend", s.backend},
                                       {"ms", s.ms}, {"exit", s.exit ? nlohmann::json(*s.exit) : nlohmann::json(nullptr)},
                                       {"exec_traced", s.trace}, {"executions", s.execs}, {"programs", top},
                                       {"commands", s.commands}, {"permissions", s.perm},
                                       {"denied", s.denied}, {"changed", s.changed}});
        }
        std::println(std::cout, "{}", out.dump());
        return 0;
    }
    std::println(std::cout, "subos {}: {} session(s)", name, order.size());
    for (auto& id : order) {
        auto& s = sessions[id];
        std::println(std::cout, "\n  session {}  {}  {}  {}", s.id, s.started, s.backend,
                     s.exit ? std::format("exit {} after {} ms", *s.exit, s.ms) : std::string("running"));
        std::println(std::cout, "    commands joined: {}", s.commands);
        if (s.trace) {
            std::vector<std::pair<int, std::string>> top;
            for (auto& [p, n] : s.programs) top.emplace_back(n, p);
            std::ranges::sort(top, std::greater<>{});
            std::println(std::cout, "    programs executed: {}", s.execs);
            for (std::size_t i = 0; i < std::min<std::size_t>(10, top.size()); ++i)
                std::println(std::cout, "      {:>5}  {}", top[i].first, top[i].second);
        } else {
            std::println(std::cout, "    programs executed: not traced (observe < full)");
        }
        if (!s.perm.empty()) {
            std::string line;
            for (auto& [a, n] : s.perm) line += std::format(" {}={}", a, n);
            std::println(std::cout, "    permission decisions:{}", line);
            for (auto& d : s.denied) std::println(std::cout, "      denied  {}", d);
        }
        if (!s.changed.empty()) {
            std::println(std::cout, "    files changed in rw mounts: {}", s.changed.size());
            for (std::size_t i = 0; i < std::min<std::size_t>(20, s.changed.size()); ++i)
                std::println(std::cout, "      {}", s.changed[i]);
        }
    }
    for (auto& c : policy_changes) std::println(std::cout, "\n  policy  {}", c);
    return 0;
}

// `subos ps`: the running sessions (design §22).
int run_ps_(int argc, char* argv[], EventStream& stream) {
    bool json = false;
    for (int i = 3; i < argc; ++i) if (std::string_view(argv[i]) == "--json") json = true;
    // Every home sessions may run from: this one, and the private domain's
    // (part 2 J40) -- one luban started where its system lives.
    auto sessions = session::list(home_view());
    if (auto domain = xlings::home::prefix_domain::resolve(Config::paths().homeDir); domain && domain->privateHome)
        if (std::error_code dec; fs::is_directory(domain->physicalHome / "run" / "subos", dec))
            for (auto& i : session::list(HomeView{domain->physicalHome})) sessions.push_back(std::move(i));
    if (json) {
        for (auto& i : sessions) std::println(std::cout, "{}", session::to_json(i).dump());
        return 0;
    }
    if (sessions.empty()) {
        log::info("no running sessions");
        return 0;
    }
    nlohmann::json table;
    table["headers"] = {"SUBOS", "SESSION", "BACKEND", "STARTED", "PID", "MODE"};
    table["rows"] = nlohmann::json::array();
    for (auto& i : sessions) {
        table["rows"].push_back({i.instance, i.id, i.backend, i.started,
                                 std::to_string(i.supervisor_pid),
                                 i.detached ? std::format("detached, ttl {}s", i.ttl)
                                            : std::string("attached")});
    }
    stream.emit(DataEvent{"table", table.dump()});
    return 0;
}

// `subos log <name>`: the instance's audit, read from outside the sandbox
// where the supervisor wrote it (design §22).
int run_log_(int argc, char* argv[], EventStream& stream,
             const std::function<void(std::string_view)>& usageError) {
    std::string name;
    std::set<std::string> kinds;
    std::string sessionId;
    std::size_t limit = 50;
    bool json = false, follow = false;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--json") json = true;
        else if (a == "-f" || a == "--follow") follow = true;
        else if (a == "--kind" && i + 1 < argc) kinds.insert(argv[++i]);
        else if (a == "--session" && i + 1 < argc) sessionId = argv[++i];
        else if ((a == "-n" || a == "--lines") && i + 1 < argc) {
            try { limit = static_cast<std::size_t>(std::stoul(argv[++i])); }
            catch (...) { usageError("-n expects a number"); return 1; }
        }
        else if (!a.empty() && a[0] != '-' && name.empty()) name = std::move(a);
        else { usageError("unknown option for `xlings subos log`: " + a); return 1; }
    }
    if (name.empty()) {
        int rc = 0;
        name = pick_subos_or_fail_("log", stream, usageError, &rc);
        if (name.empty()) return rc;
    }
    auto keep = [&](const nlohmann::json& e) {
        if (!kinds.empty() && !kinds.contains(e.value("kind", ""))) return false;
        if (!sessionId.empty() && e.value("session", "") != sessionId) return false;
        return true;
    };
    auto print = [&](const nlohmann::json& e) {
        if (json) { std::println(std::cout, "{}", e.dump()); return; }
        std::string detail;
        for (std::string k : {"program", "path", "exit", "signal", "backend", "error", "count", "ms"}) {
            if (!e.contains(k)) continue;
            detail += std::format(" {}={}", k, e[k].is_string() ? e[k].get<std::string>() : e[k].dump());
        }
        std::println(std::cout, "{} {:<9} {:<14} {}{}", e.value("ts", ""), e.value("kind", ""),
                     e.value("event", ""), e.value("session", ""), detail);
    };
    std::vector<nlohmann::json> shown;
    for (auto& e : events_of_(name)) if (keep(e)) shown.push_back(std::move(e));
    const auto from = shown.size() > limit ? shown.size() - limit : 0;
    for (auto i = from; i < shown.size(); ++i) print(shown[i]);
    if (!follow) return 0;
    // -f: keep reading what the supervisors append, in each home it keeps them.
    std::error_code ec;
    std::vector<std::pair<fs::path, std::uintmax_t>> files;
    for (const auto& [where, domain] : record_homes_(name)) {
        const auto file = where.logs_dir(name) / "events.ndjson";
        files.push_back({file, fs::exists(file, ec) ? fs::file_size(file, ec) : 0});
    }
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        for (auto& [file, offset] : files) {
            auto size = fs::exists(file, ec) ? fs::file_size(file, ec) : 0;
            if (size < offset) offset = 0;      // rotated
            if (size == offset) continue;
            std::ifstream in(file, std::ios::binary);
            in.seekg(static_cast<std::streamoff>(offset));
            std::string line;
            while (std::getline(in, line)) {
                auto e = nlohmann::json::parse(line, nullptr, false);
                if (!e.is_discarded() && e.is_object() && keep(e)) print(e);
            }
            offset = size;
        }
    }
}

}  // namespace xlings::subos
