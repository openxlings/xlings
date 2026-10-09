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
import xlings.observe;
import xlings.core.home;
import xlings.libs.sha256;
import xlings.core.version_order;

namespace xlings::subos {

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
    if (sub == "requests") {
        auto reqs = subos::broker::pending(home, name);
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
    for (auto& e : observe::read(home_view().logs_dir(name) / "events.ndjson")) {
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
    auto sessions = session::list(home_view());
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
    const auto file = home_view().logs_dir(name) / "events.ndjson";
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
    auto events = observe::read(file);
    std::vector<nlohmann::json> shown;
    for (auto& e : events) if (keep(e)) shown.push_back(std::move(e));
    const auto from = shown.size() > limit ? shown.size() - limit : 0;
    for (auto i = from; i < shown.size(); ++i) print(shown[i]);
    if (!follow) return 0;
    // -f: keep reading what the supervisor appends.
    std::error_code ec;
    auto offset = fs::exists(file, ec) ? fs::file_size(file, ec) : 0;
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
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

}  // namespace xlings::subos
