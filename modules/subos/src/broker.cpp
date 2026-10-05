module xlings.subos.broker;

import std;
import xlings.libs.json;
import xlings.observe;
import xlings.subos.home_view;
import xlings.subos.policy;
import xlings.platform;

namespace xlings::subos::broker {

Classified classify(std::span<const std::string> argv, std::string_view instance) {
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

    // Another instance is never this sandbox's to change.
    for (std::size_t i = 1; i + 1 < argv.size(); ++i) {
        if (argv[i] == "--subos" && argv[i + 1] != instance) {
            c.route = Route::Owner;
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
        c.route = Route::Owner;
        c.ops.push_back(op("instance_admin", "xlings"));
        return c;
    }
    if (cmd == "install") {
        c.route = Route::Broker;
        if (positional.empty()) c.ops.push_back(op("fetch", "*"));
        for (auto& t : positional) c.ops.push_back(op("fetch", t));
    } else if (cmd == "update") {
        c.route = Route::Broker;
        c.ops.push_back(positional.empty() ? op("index_update", "") : op("fetch", positional[0]));
    } else if (cmd == "remove" || (cmd == "use" && positional.size() >= 2)) {
        // This instance's own state (design §8.1): allowed, but written by
        // the supervisor, because the ledger it updates lives in the home.
        c.route = Route::Broker;
    } else if (cmd == "self") {
        const bool read = argv.size() >= 2 && (argv[1] == "doctor" || argv[1] == "config")
            && std::ranges::find(argv, std::string("--fix")) == argv.end();
        if (!read) { c.route = Route::Owner; c.ops.push_back(op("instance_admin", "self")); }
    } else if (cmd == "subos") {
        static const std::set<std::string> reads{"list", "ls", "info", "i", "status", "ps", "log"};
        const bool read = argv.size() < 2 || reads.contains(argv[1])
            || (argv[1] == "config" && argv.size() <= 3);
        if (!read) { c.route = Route::Owner; c.ops.push_back(op("policy_change", argv[1])); }
    } else if (cmd == "config" && argv.size() > 1) {
        c.route = Route::Owner;
        c.ops.push_back(op("instance_admin", "config"));
    } else if (cmd == "index" && argv.size() > 1 && argv[1] == "use") {
        c.route = Route::Owner;
        c.ops.push_back(op("instance_admin", "index"));
    } else if (cmd == "profile" && argv.size() > 1 && argv[1] != "list") {
        c.route = Route::Broker;
    }
    return c;
}

policy::Decision decide(const policy::Policy& p, const Classified& c) {
    if (c.route == Route::Local) return {policy::Action::Allow, "read-only"};
    policy::Decision worst{policy::Action::Allow, "this instance's own state"};
    for (const auto& op : c.ops) {
        auto d = policy::decide(p, op);
        if (static_cast<int>(d.action) > static_cast<int>(worst.action)) worst = std::move(d);
    }
    return worst;
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

bool available() {
    if constexpr (!platform::is_linux) return false;
    const auto env = platform::environment();
    auto mode = env.find("XLINGS_SUBOS_MODE");
    if (mode == env.end() || mode->second != "sandbox") return false;
    std::error_code ec;
    return fs::exists(socket_path(), ec);
}

int forward(std::span<const std::string> argv) {
    const auto path = socket_path();
    const int fd = platform::unix_connect(path);
    if (fd < 0) {
        std::println(std::cerr, "xlings: the broker is not reachable at {} ({})", path,
                     platform::error_text(platform::last_error()));
        return kExitPermission;
    }
    nlohmann::json req{{"op", "run"}, {"argv", std::vector<std::string>(argv.begin(), argv.end())}};
    const int stdio[3] = {0, 1, 2};
    if (!platform::send_message(fd, req.dump(), stdio)) {
        platform::close_fd(fd);
        return kExitPermission;
    }
    int code = kExitPermission;
    while (auto m = platform::receive_message(fd, 1 << 16)) {
        platform::close_fds(m->fds);
        auto j = nlohmann::json::parse(m->data, nullptr, false);
        if (j.is_discarded() || j.contains("started")) continue;
        code = j.value("exit", kExitPermission);
        if (auto e = j.value("error", ""); !e.empty()) std::println(std::cerr, "[xlings] {}", e);
        if (auto h = j.value("hint", ""); !h.empty()) std::println(std::cerr, "[xlings] {}", h);
        break;
    }
    platform::close_fd(fd);
    return code;
}

}  // namespace xlings::subos::broker
