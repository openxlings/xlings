module xlings.subos.broker;

import std;
import xlings.libs.json;
import xlings.observe;
import xlings.subos.home_view;
import xlings.subos.policy;
import xlings.platform;

namespace xlings::subos::broker {

Classified classify(std::span<const std::string> argv, std::string_view instance) {
    using situation::Touch;
    Classified c;
    if (argv.empty()) return c;
    const auto& cmd = argv[0];
    std::vector<std::string> positional;
    for (std::size_t i = 1; i < argv.size(); ++i)
        if (!argv[i].empty() && argv[i][0] != '-') positional.push_back(argv[i]);
    auto op = [&](std::string kind, std::string target) {
        return policy::Op{ .kind = std::move(kind), .target = std::move(target), .from_inside = true,
                           .instance = std::string(instance) };
    };

    // Another instance is never this system's to change.
    for (std::size_t i = 1; i + 1 < argv.size(); ++i) {
        if (argv[i] == "--subos" && argv[i + 1] != instance) {
            c.touch = Touch::Home;
            c.ops.push_back(op("instance_admin", argv[i + 1]));
            return c;
        }
    }
    // xlings itself is the home's, not the instance's: activating a version
    // of it rewrites the entry every shim in the home runs (AGENTS.md), the
    // owner's included. Only the owner installs, switches or removes it.
    auto names_xlings = [](std::string_view t) {
        if (auto at = t.find('@'); at != std::string_view::npos) t = t.substr(0, at);
        if (auto colon = t.rfind(':'); colon != std::string_view::npos) t = t.substr(colon + 1);
        return t == "xlings";
    };
    if ((cmd == "install" || cmd == "update" || cmd == "remove" || cmd == "use")
        && std::ranges::any_of(positional, names_xlings)
        && !(cmd == "use" && positional.size() < 2)) {
        c.touch = Touch::Home;
        c.ops.push_back(op("instance_admin", "xlings"));
        return c;
    }
    // A user's view adding to the system's store (`sudo xlings __store-add`,
    // design 2026-10-20 §4.4) fetches as an install does.
    if (cmd == "install" || cmd == "__store-add") {
        c.touch = Touch::Fetch;
        if (positional.empty()) c.ops.push_back(op("fetch", "*"));
        for (auto& t : positional) c.ops.push_back(op("fetch", t));
    } else if (cmd == "update") {
        c.touch = positional.empty() ? Touch::Index : Touch::Fetch;
        c.ops.push_back(positional.empty() ? op("index_update", "") : op("fetch", positional[0]));
    } else if (cmd == "remove" || (cmd == "use" && positional.size() >= 2)) {
        // This instance's own state (design §8.1): allowed, but written by
        // the supervisor, because the ledger it updates lives in the home.
        c.touch = Touch::Instance;
    } else if (cmd == "self") {
        const bool read = argv.size() >= 2 && (argv[1] == "doctor" || argv[1] == "config")
            && std::ranges::find(argv, std::string("--fix")) == argv.end();
        if (!read) { c.touch = Touch::Home; c.ops.push_back(op("instance_admin", "self")); }
    } else if (cmd == "subos") {
        static const std::set<std::string> reads{"list", "ls", "info", "i", "status", "ps", "log", "__check"};
        const bool read = argv.size() < 2 || reads.contains(argv[1])
            || (argv[1] == "config" && argv.size() <= 3);
        if (!read) { c.touch = Touch::Home; c.ops.push_back(op("policy_change", argv[1])); }
    } else if (cmd == "config" && argv.size() > 1) {
        c.touch = Touch::Home;
        c.ops.push_back(op("instance_admin", "config"));
    } else if (cmd == "index" && argv.size() > 1 && argv[1] == "use") {
        c.touch = Touch::Home;
        c.ops.push_back(op("instance_admin", "index"));
    } else if (cmd == "profile" && argv.size() > 1 && argv[1] != "list") {
        c.touch = Touch::Instance;
    } else if (cmd == "__clipboard" && argv.size() > 1 && (argv[1] == "copy" || argv[1] == "paste")) {
        // The clipboard bridge (wl-copy & co. inside): the host's clipboard,
        // and nothing else of the display, as the instance's policy grants
        // (`--allow clipboard` / `clipboard-paste`).
        c.touch = Touch::Instance;
        c.ops.push_back(op("grant", argv[1] == "copy" ? "clipboard" : "clipboard-paste"));
    }
    return c;
}

policy::Decision decide(const policy::Policy& p, const Classified& c) {
    if (c.touch == situation::Touch::Read) return {policy::Action::Allow, "read-only"};
    policy::Decision worst{policy::Action::Allow, "this instance's own state"};
    for (const auto& op : c.ops) {
        auto d = policy::decide(p, op);
        if (static_cast<int>(d.action) > static_cast<int>(worst.action)) worst = std::move(d);
    }
    return worst;
}

Verdict judge(const HomeView& home, const policy::Policy& p, std::string_view instance,
              std::span<const std::string> argv, std::string_view where) {
    Verdict v;
    if (argv.empty()) {
        v.error = "malformed request";
        return v;
    }
    // The decision that counts: the same function the client ran, with the
    // policy as the supervisor read it.
    const auto cls = classify(argv, instance);
    auto d = decide(p, cls);
    v.event = {{"event", "decision"}, {"program", argv[0]}, {"argc", argv.size()},
               {"action", std::string(policy::to_string(d.action))}, {"reason", d.reason}};
    if (cls.touch == situation::Touch::Home) d.action = policy::Action::Deny;
    // Deny by default: the broker runs, as the owner, only what changes this
    // instance on its behalf. Anything else -- what the client runs locally,
    // or never heard of -- is refused here, whoever sent it: a process inside
    // can talk to the socket without the client.
    if (cls.touch == situation::Touch::Read) {
        d.action = policy::Action::Deny;
        d.reason = std::format("`{}` is not something the broker runs; it runs inside the system", argv[0]);
    }
    if (d.action == policy::Action::Deny) {
        v.error = "E_PERMISSION: " + d.reason;
        if (!d.owner_command.empty()) v.hint = std::format("{}: {}", where, d.owner_command);
        return v;
    }
    if (d.action == policy::Action::Ask) {
        v.request = enqueue(home, instance, argv, d.reason);
        v.event["request"] = v.request;
        v.exit = kExitPending;
        v.error = "waiting for the owner's approval: request " + v.request;
        v.hint = std::format("{}: xlings subos approve {} {}", where, instance, v.request);
        return v;
    }
    v.run = true;
    v.exit = 0;
    return v;
}

// ── queue ────────────────────────────────────────────────────────────

std::string enqueue(const HomeView& home, std::string_view instance,
                    std::span<const std::string> argv, std::string_view reason) {
    std::random_device rd;
    const auto id = std::format("r{:06x}", rd() & 0xffffff);
    const auto dir = home.requests_dir(instance);
    std::error_code ec;
    fs::create_directories(dir, ec);
    nlohmann::json j{{"id", id}, {"argv", std::vector<std::string>(argv.begin(), argv.end())},
                     {"created", observe::utc_now()}, {"reason", std::string(reason)}};
    std::ofstream(dir / (id + ".json")) << j.dump(2);
    return id;
}

std::vector<Request> pending(const HomeView& home, std::string_view instance) {
    std::vector<Request> out;
    std::error_code ec;
    const auto dir = home.requests_dir(instance);
    if (!fs::is_directory(dir, ec)) return out;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != std::default_sentinel; it.increment(ec)) {
        if (it->path().extension() != ".json") continue;
        std::ifstream in(it->path());
        auto j = nlohmann::json::parse(in, nullptr, false);
        if (j.is_discarded()) continue;
        Request r{ .id = j.value("id", ""), .created = j.value("created", ""), .reason = j.value("reason", "") };
        for (auto& a : j.value("argv", nlohmann::json::array())) r.argv.push_back(a.get<std::string>());
        out.push_back(std::move(r));
    }
    std::ranges::sort(out, {}, &Request::created);
    return out;
}

std::optional<Request> take(const HomeView& home, std::string_view instance, std::string_view id) {
    for (auto& r : pending(home, instance)) {
        if (r.id != id) continue;
        std::error_code ec;
        fs::remove(home.requests_dir(instance) / (std::string(id) + ".json"), ec);
        return r;
    }
    return std::nullopt;
}

// ── client ───────────────────────────────────────────────────────────

namespace {
std::string socket_path() {
    const auto env = platform::environment();
    auto it = env.find(std::string(kSocketEnv));
    return it != env.end() && !it->second.empty() ? it->second : std::string(kSocketInside);
}
}  // namespace

int forward(std::span<const std::string> argv, std::span<const int> stdio_in, bool quiet_refusal) {
    const auto path = socket_path();
    const int fd = platform::unix_connect(path);
    if (fd < 0) {
        std::println(std::cerr, "xlings: the broker is not reachable at {} ({})", path,
                     platform::error_text(platform::last_error()));
        return kExitPermission;
    }
    const int standard[3] = {0, 1, 2};
    const std::span<const int> stdio = stdio_in.size() == 3 ? stdio_in : std::span<const int>(standard);
    // At a terminal the command runs on one of its own, relayed through this
    // terminal: raw here, so every key -- Ctrl-C included -- reaches it.
    const bool tty = platform::is_terminal(stdio[0]);
    // Who asked, for the record: the person behind sudo, else this user.
    const auto env = platform::environment();
    std::string actor;
    for (const auto* k : {"SUDO_USER", "USER", "LOGNAME"})
        if (auto it = env.find(k); it != env.end() && !it->second.empty()) { actor = it->second; break; }
    nlohmann::json req{{"op", "run"}, {"argv", std::vector<std::string>(argv.begin(), argv.end())}, {"tty", tty},
                       {"actor", actor}};
    if (!platform::send_message(fd, req.dump(), stdio)) {
        platform::close_fd(fd);
        return kExitPermission;
    }
    int code = kExitPermission;
    std::optional<std::string> cooked;
    if (tty) cooked = platform::make_terminal_raw(stdio[0]);
    struct Restore {
        int fd; std::optional<std::string>& saved;
        ~Restore() { if (saved) platform::restore_terminal(fd, *saved); }
    } restore{stdio[0], cooked};
    while (auto m = platform::receive_message(fd, 1 << 16)) {
        platform::close_fds(m->fds);
        auto j = nlohmann::json::parse(m->data, nullptr, false);
        if (j.is_discarded() || j.contains("started")) continue;
        code = j.value("exit", kExitPermission);
        if (quiet_refusal && code == kExitPermission) break;
        if (auto e = j.value("error", ""); !e.empty()) std::println(std::cerr, "[xlings] {}", e);
        if (auto h = j.value("hint", ""); !h.empty()) std::println(std::cerr, "[xlings] {}", h);
        break;
    }
    platform::close_fd(fd);
    return code;
}

}  // namespace xlings::subos::broker
