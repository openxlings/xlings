// xbus: what is on the bus, seen and used (design 2026-10-22 §8).
//
//   xbus router [--listen ADDR]... [--root PATH] [--log FILE] [--detach] [--idle-exit S]
//   xbus ls [PATTERN] [--interface I] [--json]
//   xbus call PATH [BODY] [--timeout MS]
//   xbus pub PATH BODY
//   xbus sub PATTERN [--count N]
//   xbus monitor [PATTERN] [--count N]
//   xbus top [--interval MS] [--count N]
//   xbus stats [--json] | xbus topology
//   xbus bench call|pubsub|ping [--n N] [--size B] [--subs K] [--json]
//   xbus dump [PATH] [--request R] [--out FILE]
//   xbus profile [PATH] [--seconds S] [--out FILE]
//
// Every command takes --router ADDR (default: $OPENBUS_ROUTER, else local:router).
import std;
import nlohmann.json;
import openbus;
import telemetry;
import xbus.router;
import xbus.node;

namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

struct Args {
    std::vector<std::string> words;
    std::map<std::string, std::vector<std::string>> options;
    std::set<std::string> flags;
    std::optional<std::string> option(std::string_view k) const {
        auto it = options.find(std::string(k));
        return it == options.end() || it->second.empty() ? std::nullopt : std::optional(it->second.back());
    }
    long number(std::string_view k, long fallback) const {
        const auto v = option(k);
        if (!v) return fallback;
        long n = fallback;
        std::from_chars(v->data(), v->data() + v->size(), n);
        return n;
    }
};

Args parse(int argc, char** argv) {
    static const std::set<std::string> kFlags{"--json", "--detach", "--payload", "-h", "--help"};
    Args a;
    for (int k = 2; k < argc; ++k) {
        const std::string w = argv[k];
        if (kFlags.contains(w)) a.flags.insert(w);
        else if (w.starts_with("--") && k + 1 < argc) a.options[w].push_back(argv[++k]);
        else a.words.push_back(w);
    }
    return a;
}

std::optional<xbus::Address> router_of(const Args& a) {
    if (const auto r = a.option("--router")) return xbus::Address::parse(*r);
    return xbus::default_address();
}

std::shared_ptr<xbus::Node> join(const Args& a, std::string name) {
    const auto at = router_of(a);
    if (!at) {
        std::println(std::cerr, "xbus: an address is scheme:where (local:router, unix:/path)");
        return nullptr;
    }
    auto n = xbus::Node::connect({.name = std::move(name), .actor = "program:xbus", .router = *at, .telemetry = false});
    if (!n) {
        std::println(std::cerr, "xbus: {}", n.error());
        std::println(std::cerr, "  start one: xbus router --listen {}", at->text());
        return nullptr;
    }
    return *n;
}

std::string human_ns(std::uint64_t ns) {
    if (ns < 1'000) return std::format("{} ns", ns);
    if (ns < 1'000'000) return std::format("{:.1f} µs", static_cast<double>(ns) / 1e3);
    if (ns < 1'000'000'000) return std::format("{:.1f} ms", static_cast<double>(ns) / 1e6);
    return std::format("{:.2f} s", static_cast<double>(ns) / 1e9);
}

// ── router ──────────────────────────────────────────────────────────

int router(const Args& a) {
    xbus::RouterOptions o;
    o.root = a.option("--root").value_or(xbus::host_root());
    for (const auto& l : a.options.contains("--listen") ? a.options.at("--listen") : std::vector<std::string>{xbus::default_address().text()}) {
        auto at = xbus::Address::parse(l);
        if (!at) { std::println(std::cerr, "xbus router: {} is not an address", l); return 2; }
        o.listen.push_back(*at);
    }
    if (const auto log = a.option("--log")) o.log = *log;
    telemetry::identify("xbus-router");
    if (const auto idle = a.option("--idle-exit")) o.idle_exit = std::chrono::seconds(std::stol(*idle));
    else if (a.flags.contains("--detach")) o.idle_exit = std::chrono::seconds(600);
    xbus::Router r(o);
    if (auto l = r.listen(); !l) {
        std::println(std::cerr, "xbus router: {}", l.error());
        return 1;
    }
    if (!a.flags.contains("--detach")) std::println("xbus router {} at {} ({})", r.id(), o.listen.front().text(), r.root());
    std::atomic<bool> stop{false};
    r.run(stop);
    return 0;
}

// ── seeing ──────────────────────────────────────────────────────────

int ls(const Args& a) {
    auto n = join(a, "xbus-ls");
    if (!n) return 1;
    const auto found = n->resolve(a.words.empty() ? "**" : a.words[0], a.option("--interface").value_or(""));
    if (!found) { std::println(std::cerr, "xbus ls: {}", found.error().message); return 1; }
    if (a.flags.contains("--json")) {
        Json out = Json::array();
        for (const auto& e : *found)
            out.push_back({{"path", e.path}, {"kind", std::string(openbus::name_of(e.kind))}, {"node", e.node}, {"name", e.name},
                           {"via", e.via}, {"interfaces", e.interfaces}});
        std::println("{}", out.dump(1));
        return 0;
    }
    for (const auto& e : *found) {
        std::string ifs;
        for (const auto& i : e.interfaces) ifs += (ifs.empty() ? "" : ",") + i;
        std::println("{:<8} {:<56} {:<16}{}{}", openbus::name_of(e.kind), e.path, e.name, ifs.empty() ? "" : "  " + ifs,
                     e.via.empty() ? "" : std::format("  (via {})", e.via.size()));
    }
    return 0;
}

int call(const Args& a) {
    if (a.words.empty()) { std::println(std::cerr, "usage: xbus call PATH [BODY]"); return 2; }
    auto n = join(a, "xbus-call");
    if (!n) return 1;
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = n->call_each(a.words[0], a.words.size() > 1 ? a.words[1] : std::string{},
                                [](std::string_view part) {
                                    const auto j = Json::parse(part, nullptr, false);
                                    std::println("{}", j.is_discarded() ? std::string(part) : j.dump(1));
                                },
                                std::chrono::milliseconds(a.number("--timeout", 5000)));
    if (!r) {
        std::println(std::cerr, "xbus call: {} ({})", r.error().message, openbus::name_of(r.error().code));
        return 1;
    }
    std::println(std::cerr, "-- {}", human_ns(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - t0).count())));
    return 0;
}

int pub(const Args& a) {
    if (a.words.size() < 2) { std::println(std::cerr, "usage: xbus pub PATH BODY"); return 2; }
    auto n = join(a, "xbus-pub");
    if (!n) return 1;
    const bool ok = n->publish(a.words[0], a.words[1]);
    std::this_thread::sleep_for(100ms);   // written before the node goes
    return ok ? 0 : 1;
}

int sub(const Args& a, bool monitor) {
    auto n = join(a, monitor ? "xbus-monitor" : "xbus-sub");
    if (!n) return 1;
    const auto pattern = monitor ? std::string("bus/trace") : (a.words.empty() ? std::string("**") : a.words[0]);
    const auto only = monitor && !a.words.empty() ? a.words[0] : std::string{};
    const long count = a.number("--count", -1);
    std::atomic<long> seen{0};
    std::mutex out;
    n->subscribe(pattern, [&](const xbus::Message& m) {
        std::lock_guard l(out);
        if (monitor) {
            const auto f = openbus::Fields::decode(m.body);
            if (!f) return;
            if (!only.empty() && !openbus::matches(only, f->value("path", "x"))) return;
            std::println("{:<10} {:<14} {:<16} -> {:<16} {:>7} B  {}", f->value("t").substr(std::max<std::size_t>(f->value("t").size(), 9) - 9),
                         f->value("kind"), f->value("from"), f->value("to"), f->value("size"), f->value("path"));
        } else {
            std::println("{}  {}{}", m.path, m.event ? std::format("event {}", m.value) : m.body, m.from.empty() ? "" : "  (from " + m.from.substr(0, 8) + ")");
        }
        ++seen;
    });
    while (count < 0 || seen.load() < count) std::this_thread::sleep_for(50ms);
    return 0;
}

int stats(const Args& a, std::string_view what) {
    auto n = join(a, "xbus-stats");
    if (!n) return 1;
    const auto r = n->call(std::string("bus/") + std::string(what));
    if (!r || r->empty()) { std::println(std::cerr, "xbus: {}", r ? "no answer" : r.error().message); return 1; }
    const auto j = Json::parse(r->front(), nullptr, false);
    if (a.flags.contains("--json") || what != "stats") {
        std::println("{}", j.dump(1));
        return 0;
    }
    std::println("router {} ({}), {} frames", j.value("router", ""), j.value("root", ""), j.value("frames", 0));
    std::println("{:<56} {:>8} {:>8} {:>8} {:>10} {:>10} {:>6}", "endpoint", "calls", "pubs", "events", "p50", "p99", "drops");
    for (const auto& e : j["endpoints"])
        std::println("{:<56} {:>8} {:>8} {:>8} {:>10} {:>10} {:>6}", e.value("path", ""), e.value("calls", 0), e.value("publishes", 0),
                     e.value("events", 0), human_ns(e["latency_ns"].value("p50", 0ull)), human_ns(e["latency_ns"].value("p99", 0ull)),
                     e.value("drops", 0));
    std::println("{:<24} {:>10} {:>10} {:>12} {:>12} {:>8}", "node", "in", "out", "bytes in", "bytes out", "queued");
    for (const auto& nd : j["nodes"])
        std::println("{:<24} {:>10} {:>10} {:>12} {:>12} {:>8}", nd.value("name", "") + (nd.value("link", false) ? " (link)" : ""),
                     nd.value("frames_in", 0), nd.value("frames_out", 0), nd.value("bytes_in", 0), nd.value("bytes_out", 0), nd.value("queued", 0));
    return 0;
}

int top(const Args& a) {
    const long count = a.number("--count", -1);
    for (long k = 0; count < 0 || k < count; ++k) {
        if (k > 0) std::this_thread::sleep_for(std::chrono::milliseconds(a.number("--interval", 1000)));
        std::print("\x1b[H\x1b[2J");
        if (stats(a, "stats") != 0) return 1;
    }
    return 0;
}

// ── telemetry (SPEC.md §8.3; the telemetry kit) ─────────────────────

// A telemetry endpoint's answer: its replies joined, as JSON.
std::optional<Json> ask_telemetry(xbus::Node& n, const std::string& path, std::string_view request) {
    const auto r = n.call(path, request, std::chrono::seconds(10));
    if (!r) {
        std::println(std::cerr, "xbus: {}: {}", path, r.error().message);
        return std::nullopt;
    }
    std::string text;
    for (const auto& piece : *r) text += piece;
    auto j = Json::parse(text, nullptr, false);
    if (j.is_discarded()) {
        std::println(std::cerr, "xbus: {} did not answer JSON (is it an openbus.telemetry@0.1 endpoint?)", path);
        return std::nullopt;
    }
    return j;
}

bool write_out(const Json& j, const std::optional<std::string>& out) {
    if (!out) {
        std::println("{}", j.dump(1));
        return true;
    }
    std::ofstream f(*out, std::ios::trunc | std::ios::binary);
    f << j.dump() << '\n';
    if (!f) std::println(std::cerr, "xbus: cannot write {}", *out);
    return static_cast<bool>(f);
}

int dump(const Args& a) {
    auto n = join(a, "xbus-dump");
    if (!n) return 1;
    const auto path = a.words.empty() ? std::string("bus/telemetry") : a.words[0];
    const auto j = ask_telemetry(*n, path, a.option("--request").value_or("dump"));
    if (!j) return 1;
    if (j->contains("error")) {
        std::println(std::cerr, "xbus: {}", (*j)["error"].get<std::string>());
        return 1;
    }
    return write_out(*j, a.option("--out")) ? 0 : 1;
}

int profile(const Args& a) {
    auto n = join(a, "xbus-profile");
    if (!n) return 1;
    const auto path = a.words.empty() ? std::string("bus/telemetry") : a.words[0];
    const long seconds = a.number("--seconds", 5);
    if (!ask_telemetry(*n, path, "profile on")) return 1;
    std::println(std::cerr, "profiling {} for {} s", path, seconds);
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    const auto trace = ask_telemetry(*n, path, "trace");
    (void)ask_telemetry(*n, path, "profile off");
    if (!trace) return 1;
    const auto out = a.option("--out").value_or("xbus-profile.json");
    if (!write_out(*trace, out)) return 1;
    std::println("{} events in {} (open it in a trace viewer: Perfetto, chrome://tracing)", (*trace)["traceEvents"].size(), out);
    return 0;
}

// ── bench (design 2026-10-22 §9) ────────────────────────────────────

struct Bench {
    std::string name;
    long n { 0 };
    openbus::Histogram latency;
    double seconds { 0 };
};

void report(const Bench& b, bool json) {
    const double rate = b.seconds > 0 ? static_cast<double>(b.n) / b.seconds : 0;
    if (json) {
        std::println("{}", Json{{"bench", b.name}, {"n", b.n}, {"per_second", rate}, {"p50_ns", b.latency.percentile(0.5)},
                                {"p99_ns", b.latency.percentile(0.99)}, {"p999_ns", b.latency.percentile(0.999)},
                                {"max_ns", b.latency.percentile(1.0)}}.dump());
        return;
    }
    std::println("{:<8} {:>9} ops  {:>12.0f}/s   p50 {:>9}  p99 {:>9}  p999 {:>9}  max {:>9}", b.name, b.n, rate,
                 human_ns(b.latency.percentile(0.5)), human_ns(b.latency.percentile(0.99)), human_ns(b.latency.percentile(0.999)),
                 human_ns(b.latency.percentile(1.0)));
}

int bench(const Args& a) {
    const auto what = a.words.empty() ? std::string("call") : a.words[0];
    const long n = a.number("--n", what == "pubsub" ? 100000 : 10000);
    const auto size = static_cast<std::size_t>(a.number("--size", 64));
    // Its own router, unless one is named: what is measured is XBus, not whoever else is on a bus.
    std::unique_ptr<xbus::Router> own;
    std::atomic<bool> stop{false};
    std::jthread running;
    Args use = a;
    if (!a.option("--router")) {
        std::error_code ec;
        const auto dir = std::filesystem::temp_directory_path(ec) / std::format("xbus-bench-{}", std::random_device{}());
        std::filesystem::create_directories(dir, ec);
        const xbus::Address at{"local", (dir / "router.sock").string()};
        own = std::make_unique<xbus::Router>(xbus::RouterOptions{.root = "host/bench", .listen = {at}});
        if (auto l = own->listen(); !l) { std::println(std::cerr, "xbus bench: {}", l.error()); return 1; }
        running = std::jthread([&] { own->run(stop); });
        use.options["--router"] = {at.text()};
    }
    auto server = join(use, "bench-server");
    auto client = join(use, "bench-client");
    if (!server || !client) return 1;
    const std::string body(size, 'x');
    Bench b{.name = what, .n = n};
    if (what == "call" || what == "ping") {
        (void)server->offer("service/bench/echo", {}, [](xbus::Call c) { c.reply(c.body()); c.end(); });
        for (long k = 0; k < std::min<long>(n / 10, 1000); ++k) (void)client->call("service/bench/echo", body);   // warm
        const auto t0 = std::chrono::steady_clock::now();
        for (long k = 0; k < n; ++k) {
            const auto s = std::chrono::steady_clock::now();
            if (what == "ping") {
                if (!client->ping()) return 1;
            } else if (!client->call("service/bench/echo", body)) {
                return 1;
            }
            b.latency.add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - s).count()));
        }
        b.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    } else if (what == "pubsub") {
        const long subs = a.number("--subs", 4);
        const long window = a.number("--window", 64);
        std::vector<std::shared_ptr<xbus::Node>> nodes;
        std::atomic<long> got{0};
        std::mutex hm;
        for (long k = 0; k < subs; ++k) {
            auto s = join(use, std::format("bench-sub-{}", k));
            if (!s) return 1;
            s->subscribe("topic/bench/*", [&](const xbus::Message& m) {
                std::uint64_t sent = 0;
                if (m.body.size() >= 8) std::memcpy(&sent, m.body.data(), 8);
                const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
                {
                    std::lock_guard l(hm);
                    b.latency.add(now - sent);
                }
                ++got;
            });
            nodes.push_back(std::move(s));
        }
        std::this_thread::sleep_for(200ms);
        std::string msg(std::max<std::size_t>(size, 8), 'x');
        const auto t0 = std::chrono::steady_clock::now();
        for (long k = 0; k < n; ++k) {
            const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
            std::memcpy(msg.data(), &now, 8);
            server->publish("topic/bench/data", msg);
            // Kept within what the subscribers took (--window): a publisher that runs away measures a queue, not the bus.
            while (k - got.load() / subs > window) std::this_thread::yield();
        }
        const auto until = std::chrono::steady_clock::now() + 10s;
        while (got.load() < n * subs && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(1ms);
        b.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        b.n = got.load();
    } else {
        std::println(std::cerr, "xbus bench: call, ping or pubsub");
        return 2;
    }
    report(b, a.flags.contains("--json"));
    server->close();
    client->close();
    stop = true;
    return 0;
}

void usage() {
    std::println(R"(xbus -- what is on the bus (openbus {}), seen and used

  router [--listen ADDR]... [--root PATH] [--log FILE] [--detach]   a router
  ls [PATTERN] [--interface I] [--json]      the endpoints (objects)
  call PATH [BODY]                           call one; its replies
  pub PATH BODY | sub PATTERN [--count N]    publish, subscribe
  monitor [PATTERN] [--count N]              every frame routed, as it is
  top | stats [--json] | topology            what the router carries
  bench call|ping|pubsub [--n N] [--size B] [--subs K] [--window W] [--json]
  dump [PATH] [--request R] [--out FILE]     a process's telemetry (default: the router's)
  profile [PATH] [--seconds S] [--out FILE]  a trace of what it did meanwhile
  ls --interface openbus.telemetry@0.1       every process that can be asked

  --router ADDR   which router (default: $OPENBUS_ROUTER, else local:router))", openbus::kVersion);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::string_view(argv[1]) == "-h" || std::string_view(argv[1]) == "--help") {
        usage();
        return argc < 2 ? 2 : 0;
    }
    const std::string cmd = argv[1];
    const auto a = parse(argc, argv);
    if (cmd == "router") return router(a);
    if (cmd == "ls") return ls(a);
    if (cmd == "call") return call(a);
    if (cmd == "pub") return pub(a);
    if (cmd == "sub") return sub(a, false);
    if (cmd == "monitor") return sub(a, true);
    if (cmd == "top") return top(a);
    if (cmd == "stats") return stats(a, "stats");
    if (cmd == "topology") return stats(a, "topology");
    if (cmd == "verbs") return stats(a, "verbs");
    if (cmd == "bench") return bench(a);
    if (cmd == "dump") return dump(a);
    if (cmd == "profile") return profile(a);
    usage();
    return 2;
}
