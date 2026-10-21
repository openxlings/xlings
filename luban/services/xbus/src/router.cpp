module xbus.router;

import std;
import nlohmann.json;
import openbus;
import telemetry;
import xbus.address;
import xbus.detail.backend;
import xbus.detail.conn;

namespace xbus {

namespace {

using namespace detail;
namespace native = detail::backend;
using Handle = detail::backend::Handle;

using Json = nlohmann::json;
using openbus::Fields;
using openbus::Frame;
using openbus::Kind;

constexpr std::uint32_t kRouterChannel = 0x80000000u;
constexpr std::size_t kPiece = 512u << 10;

std::string random_hex_(int bytes) {
    std::random_device rd;
    std::string out;
    for (int k = 0; k < bytes; ++k) out += std::format("{:02x}", rd() & 0xff);
    return out;
}

Frame control_(Kind k, std::uint32_t channel, const Fields& f) {
    return Frame{.kind = static_cast<std::uint8_t>(k), .codec = static_cast<std::uint16_t>(openbus::Codec::fields), .channel = channel,
                 .payload = f.encode()};
}

Frame error_(std::uint32_t channel, openbus::ErrorCode code, std::string_view message) {
    return control_(Kind::error, channel, Fields{{"code", std::string(openbus::name_of(code))}, {"message", std::string(message)}});
}

// One connected side: a node, or another router (a link).
struct Client {
    int id { 0 };
    std::unique_ptr<Conn> conn;
    PeerInfo peer;
    bool hello { false };
    bool initiated_link { false };   // this side said hello on it (link())
    std::uint32_t next_channel { 1 };
    std::uint64_t dropped { 0 };
    bool gone { false };
    std::uint32_t channel() { return kRouterChannel | (next_channel++ & ~kRouterChannel); }
};

struct EndpointStats {
    std::uint64_t calls { 0 }, replies { 0 }, publishes { 0 }, events { 0 }, bytes { 0 }, errors { 0 }, drops { 0 }, fallbacks { 0 };
    openbus::Histogram latency;
};

struct Endpoint {
    int owner { 0 };                 // a client, or 0: the router's own
    openbus::EndpointKind kind { openbus::EndpointKind::service };
    Fields meta;                     // what it was offered with (verb, effect, ...)
    bool retained { false };
    std::optional<std::string> last; // a retained topic's last publication (its payload)
    std::vector<std::string> via;    // the routers it came through
    EndpointStats stats;
};

// A call or a stream: a channel on one client mapped to one on another.
struct Route {
    int other { 0 };
    std::uint32_t other_channel { 0 };
    Kind kind { Kind::call };
    std::string path;
    std::uint64_t started { 0 };
    bool caller { false };           // this side opened it
};

struct Subscription {
    int client { 0 };
    std::uint32_t channel { 0 };
    std::string pattern;             // absolute
};

}  // namespace

struct Router::Impl {
    RouterOptions o;
    std::string id = random_hex_(8);
    std::map<int, Client> clients;
    int next_client { 1 };
    std::map<std::string, Endpoint> endpoints;
    std::map<std::pair<int, std::uint32_t>, Route> routes;
    std::vector<Subscription> subscriptions;
    // A subscription forwarded over a link: (link, its channel there) -> (subscriber, its channel).
    std::map<std::pair<int, std::uint32_t>, std::pair<int, std::uint32_t>> forwarded;
    std::uint64_t last_node_at = native::now_ns();
    std::uint64_t frames { 0 };
    // Its log (the file is RouterOptions::log), and what it measures of itself
    // (telemetry: bus/telemetry, a dump's "xbus.router.<id>").
    telemetry::Log log;
    telemetry::Counter& m_frames = telemetry::metrics().counter("xbus.router.frames");
    telemetry::Counter& m_drops = telemetry::metrics().counter("xbus.router.drops");
    telemetry::Counter& m_fallbacks = telemetry::metrics().counter("xbus.router.fallbacks");
    telemetry::Histogram& m_round = telemetry::metrics().histogram("xbus.router.round_ns");
    telemetry::Histogram& m_frame = telemetry::metrics().histogram("xbus.router.frame_ns");
    telemetry::Gauge& m_queued = telemetry::metrics().gauge("xbus.router.queued_bytes");
    telemetry::Gauge& m_clients = telemetry::metrics().gauge("xbus.router.clients");
    // What another thread may read of it (a dump's state): set each round.
    std::atomic<std::size_t> seen_nodes { 0 }, seen_links { 0 }, seen_endpoints { 0 };

    std::string abs(std::string_view p) const {
        return p.starts_with("host/") || p == "host" ? std::string(p) : o.root + "/" + std::string(p);
    }
    // A pattern that begins with a wildcard asks everywhere (SPEC.md §5.2).
    std::string abs_pattern(std::string_view p) const {
        return p.starts_with("*") ? std::string(p) : abs(p);
    }

    void note(Json fields) {
        const auto event = fields.value("event", std::string("event"));
        fields.erase("event");
        fields["router"] = id;
        const bool trouble = event == "denied" || event == "slow-consumer" || event == "fallback";
        log.write(trouble ? telemetry::Level::warn : telemetry::Level::info, event, std::move(fields));
    }

    bool allowed(const Client& c, std::string_view op, std::string_view path) {
        if (!o.allow || o.allow(c.peer, op, path)) return true;
        note({{"event", "denied"}, {"node", c.peer.node}, {"actor", c.peer.actor}, {"op", op}, {"path", path}});
        return false;
    }

    Client* client(int id) {
        auto it = clients.find(id);
        return it == clients.end() || it->second.gone ? nullptr : &it->second;
    }

    // Queued to a client; a reliable frame past its queue's limit ends it, a
    // latest or best-effort one is dropped and counted.
    bool send(Client& c, const Frame& f, Handle h = {}) {
        if (c.gone) return false;
        if (c.conn->queued_bytes() > o.queue_limit) {
            if (openbus::reliability_of(f.flags) != openbus::Reliability::reliable) {
                ++c.dropped;
                m_drops.add();
                return false;
            }
            note({{"event", "slow-consumer"}, {"node", c.peer.node}, {"name", c.peer.name}, {"queued", c.conn->queued_bytes()}});
            c.gone = true;
            return false;
        }
        if (!c.conn->push(f, std::move(h))) {
            m_fallbacks.add();
            note({{"event", "fallback"}, {"node", c.peer.node}, {"why", "this channel carries no handles"}});
        }
        return true;
    }

    // ── tracing (bus/trace) ──────────────────────────────────────────

    void trace(const Frame& f, int from, int to, std::string_view path) {
        const auto topic = o.root + "/bus/trace";
        if (path == topic) return;
        bool any = false;
        for (const auto& s : subscriptions) any = any || openbus::matches(s.pattern, topic);
        if (!any) return;
        const auto* a = client(from);
        const auto* b = client(to);
        Fields rec{{"t", std::to_string(native::now_ns())}, {"kind", std::string(openbus::name_of(f.known()))},
                   {"path", std::string(path)}, {"size", std::to_string(f.payload.size())}, {"channel", std::to_string(f.channel)},
                   {"from", a ? a->peer.name : std::string("router")}, {"to", b ? b->peer.name : std::string("router")},
                   {"trace", std::to_string(f.trace)}};
        deliver(Frame{.kind = static_cast<std::uint8_t>(Kind::publish),
                      .flags = openbus::with_reliability(openbus::with_priority(0, openbus::Priority::bulk), openbus::Reliability::best_effort),
                      .codec = static_cast<std::uint16_t>(openbus::Codec::fields)},
                Fields{{"path", topic}, {"from", id}}, rec.encode(), 0, false);
    }

    // ── endpoints ────────────────────────────────────────────────────

    Fields offer_fields(const std::string& path, const Endpoint& e) const {
        Fields f{{"path", path}, {"kind", std::string(openbus::name_of(e.kind))}};
        if (e.retained) f.add("retained", "1");
        for (const auto& [k, v] : e.meta.items())
            if (k != "path" && k != "kind" && k != "retained" && k != "via") f.add(k, v);
        std::string via;
        for (const auto& v : e.via) via += (via.empty() ? "" : ",") + v;
        via += (via.empty() ? "" : ",") + id;
        f.add("via", via);
        return f;
    }

    void propagate_offer(const std::string& path, const Endpoint& e) {
        for (auto& [cid, c] : clients) {
            if (!c.peer.link || c.gone || cid == e.owner || !c.hello) continue;
            if (std::ranges::contains(e.via, c.peer.node)) continue;
            send(c, control_(Kind::offer, 0, offer_fields(path, e)));
        }
    }

    void withdraw(const std::string& path) {
        const auto it = endpoints.find(path);
        if (it == endpoints.end()) return;
        const int owner = it->second.owner;
        endpoints.erase(it);
        for (auto& [cid, c] : clients)
            if (c.peer.link && c.hello && !c.gone && cid != owner) send(c, control_(Kind::withdraw, 0, Fields{{"path", path}}));
        publish_topology();
    }

    void publish_topology() {
        const auto topic = o.root + "/bus/topology";
        for (const auto& s : subscriptions)
            if (openbus::matches(s.pattern, topic)) {
                deliver(Frame{.kind = static_cast<std::uint8_t>(Kind::publish), .codec = static_cast<std::uint16_t>(openbus::Codec::json)},
                        Fields{{"path", topic}, {"from", id}}, topology(), 0, false);
                break;
            }
    }

    void offer(Client& c, const Frame& f) {
        auto fields = Fields::decode(f.payload);
        const auto path = fields ? abs(fields->value("path")) : std::string{};
        const auto kind = fields ? openbus::endpoint_kind(fields->value("kind", "service")) : std::nullopt;
        if (!fields || !openbus::valid_path(path) || !kind) {
            if (!c.peer.link) send(c, error_(f.channel, openbus::ErrorCode::malformed, "an offer names a path and a kind"));
            return;
        }
        if (!allowed(c, "offer", path)) {
            if (!c.peer.link) send(c, error_(f.channel, openbus::ErrorCode::denied, "not this node's to offer"));
            return;
        }
        if (const auto it = endpoints.find(path); it != endpoints.end() && it->second.owner != c.id) {
            if (!c.peer.link) send(c, error_(f.channel, openbus::ErrorCode::taken, path + " is offered by another node"));
            return;
        }
        Endpoint e{.owner = c.id, .kind = *kind, .meta = *fields, .retained = fields->value("retained") == "1"};
        if (c.peer.link)
            for (auto v : std::views::split(fields->value("via"), ','))
                if (std::string_view s(v.begin(), v.end()); !s.empty()) e.via.emplace_back(s);
        if (const auto it = endpoints.find(path); it != endpoints.end()) e.last = it->second.last;
        auto& stored = endpoints[path] = std::move(e);
        if (!c.peer.link) send(c, control_(Kind::resolved, f.channel, Fields{{"path", path}, {"node", c.peer.node}}));
        propagate_offer(path, stored);
        note({{"event", "offer"}, {"path", path}, {"node", c.peer.node}, {"name", c.peer.name}, {"link", c.peer.link}});
        publish_topology();
    }

    void resolve(Client& c, const Frame& f) {
        const auto fields = Fields::decode(f.payload);
        const auto pattern = abs_pattern(fields ? fields->value("pattern", "**") : std::string("**"));
        const auto wanted = fields ? fields->value("interface") : std::string{};
        for (const auto& [path, e] : endpoints) {
            if (!openbus::matches(pattern, path)) continue;
            // By what it does (SPEC.md §2.7): an interface it names.
            if (!wanted.empty() && !std::ranges::contains(e.meta.all("interface"), std::string_view(wanted))) continue;
            const auto* owner = client(e.owner);
            Fields r{{"path", path}, {"kind", std::string(openbus::name_of(e.kind))},
                     {"node", owner ? owner->peer.node : id}, {"name", owner ? owner->peer.name : std::string("router")}};
            std::string via;
            for (const auto& v : e.via) via += (via.empty() ? "" : ",") + v;
            if (!via.empty()) r.add("via", via);
            for (const auto i : e.meta.all("interface")) r.add("interface", std::string(i));
            send(c, control_(Kind::resolved, f.channel, r));
        }
        send(c, Frame{.kind = static_cast<std::uint8_t>(Kind::reply_end), .channel = f.channel});
    }

    // ── calls and streams ────────────────────────────────────────────

    // The envelope as the next hop reads it: absolute path, who sent it.
    std::string rewrite(const Client& from, const openbus::Envelope& env, const std::string& path) {
        Fields out;
        for (const auto& [k, v] : env.fields.items())
            if (k != "path" && k != "from" && k != "actor" && k != "via" && k != "pattern") out.add(k, v);
        out.add("path", path);
        if (from.peer.link) {
            out.add("from", env.fields.value("from"));
            out.add("actor", env.fields.value("actor"));
            out.add("via", env.fields.value("via").empty() ? from.peer.node : env.fields.value("via") + "," + from.peer.node);
        } else {
            out.add("from", from.peer.node);
            out.add("actor", from.peer.actor);
        }
        return openbus::address(out, env.body);
    }

    void open_route(Client& c, Frame f, Handle h) {
        auto env = openbus::open_envelope(f.payload);
        const auto path = env ? abs(env->fields.value("path")) : std::string{};
        const bool stream = f.is(Kind::stream_open);
        if (!env || !openbus::valid_path(path)) {
            send(c, error_(f.channel, openbus::ErrorCode::malformed, "a call names its path in its envelope"));
            return;
        }
        if (path.starts_with(o.root + "/bus/") && !stream) {
            own(c, f, path);
            return;
        }
        const auto it = endpoints.find(path);
        Client* owner = it == endpoints.end() ? nullptr : client(it->second.owner);
        if (!owner) {
            send(c, error_(f.channel, openbus::ErrorCode::not_found, "nothing is offered at " + path));
            return;
        }
        if (!allowed(c, stream ? "open" : "call", path)) {
            ++it->second.stats.errors;
            send(c, error_(f.channel, openbus::ErrorCode::denied, "not allowed to " + std::string(stream ? "open " : "call ") + path));
            return;
        }
        const auto ch = owner->channel();
        const auto now = native::now_ns();
        routes[{c.id, f.channel}] = Route{owner->id, ch, stream ? Kind::stream_open : Kind::call, path, now, true};
        routes[{owner->id, ch}] = Route{c.id, f.channel, stream ? Kind::stream_open : Kind::call, path, now, false};
        ++it->second.stats.calls;
        it->second.stats.bytes += f.payload.size();
        f.payload = rewrite(c, *env, path);
        f.channel = ch;
        trace(f, c.id, owner->id, path);
        send(*owner, f, std::move(h));
    }

    // A frame on a mapped channel: to the other side, on its channel.
    bool follow(Client& c, Frame f, Handle h) {
        const auto it = routes.find({c.id, f.channel});
        if (it == routes.end()) return false;
        const auto r = it->second;
        Client* other = client(r.other);
        const auto ep = endpoints.find(r.path);
        if (ep != endpoints.end()) {
            ep->second.stats.bytes += f.payload.size();
            if (f.is(Kind::reply)) ++ep->second.stats.replies;
            if (f.is(Kind::error)) ++ep->second.stats.errors;
        }
        const bool ends = f.is(Kind::reply_end) || f.is(Kind::stream_close) || (f.is(Kind::error) && !r.caller);
        if (ends) {
            if (ep != endpoints.end() && !r.caller) ep->second.stats.latency.add(native::now_ns() - r.started);
            routes.erase({c.id, f.channel});
            routes.erase({r.other, r.other_channel});
        }
        trace(f, c.id, r.other, r.path);
        f.channel = r.other_channel;
        if (other) send(*other, f, std::move(h));
        return true;
    }

    // ── publish, subscribe, events, broadcasts ───────────────────────

    // To every subscription matching `env`'s path -- but a link's own
    // deliveries come back only to the subscription that asked for them.
    void deliver(Frame f, const Fields& env, std::string_view body, int origin, bool count) {
        const auto path = env.value("path");
        const auto payload = openbus::address(env, body);
        std::size_t to = 0;
        for (const auto& s : subscriptions) {
            if (!openbus::matches(s.pattern, path)) continue;
            Client* c = client(s.client);
            if (!c || (c->peer.link && s.client == origin)) continue;
            Frame out = f;
            out.channel = s.channel;
            out.payload = payload;
            if (!send(*c, out)) {
                if (auto ep = endpoints.find(path); ep != endpoints.end()) ++ep->second.stats.drops;
            }
            ++to;
        }
        if (count)
            if (auto ep = endpoints.find(path); ep != endpoints.end()) {
                if (f.is(Kind::event)) ++ep->second.stats.events;
                else ++ep->second.stats.publishes;
                ep->second.stats.bytes += body.size();
            }
    }

    void publish(Client& c, const Frame& f) {
        auto env = openbus::open_envelope(f.payload);
        // A link's delivery to a subscription forwarded to it.
        if (c.peer.link)
            if (const auto fw = forwarded.find({c.id, f.channel}); fw != forwarded.end()) {
                if (Client* to = client(fw->second.first)) {
                    Frame out = f;
                    out.channel = fw->second.second;
                    send(*to, out);
                }
                return;
            }
        const auto path = env ? abs(env->fields.value("path")) : std::string{};
        if (!env || !openbus::valid_path(path)) {
            send(c, error_(f.channel, openbus::ErrorCode::malformed, "a publication names its path"));
            return;
        }
        if (!allowed(c, "publish", path)) {
            send(c, error_(f.channel, openbus::ErrorCode::denied, "not allowed to publish " + path));
            return;
        }
        Fields out;
        for (const auto& [k, v] : env->fields.items())
            if (k != "path" && k != "from" && k != "actor") out.add(k, v);
        out.add("path", path);
        out.add("from", c.peer.node);
        if (f.is(Kind::publish))
            if (auto ep = endpoints.find(path); ep != endpoints.end() && ep->second.retained)
                ep->second.last = openbus::address(out, env->body);
        trace(f, c.id, 0, path);
        deliver(f, out, env->body, c.id, true);
    }

    void subscribe(Client& c, const Frame& f) {
        auto env = openbus::open_envelope(f.payload);
        const auto pattern = env ? abs_pattern(env->fields.value("pattern")) : std::string{};
        if (!env || !openbus::valid_pattern(pattern)) {
            send(c, error_(f.channel, openbus::ErrorCode::malformed, "a subscription names its pattern"));
            return;
        }
        if (!allowed(c, "subscribe", pattern)) {
            send(c, error_(f.channel, openbus::ErrorCode::denied, "not allowed to subscribe to " + pattern));
            return;
        }
        subscriptions.push_back({c.id, f.channel, pattern});
        // The retained topics' last word, at once.
        for (const auto& [path, e] : endpoints)
            if (e.last && openbus::matches(pattern, path))
                send(c, Frame{.kind = static_cast<std::uint8_t>(Kind::publish), .channel = f.channel, .payload = *e.last});
        // Over every other link, for what is published beyond it.
        for (auto& [lid, l] : clients) {
            if (!l.peer.link || l.gone || !l.hello || lid == c.id) continue;
            const auto ch = l.channel();
            forwarded[{lid, ch}] = {c.id, f.channel};
            send(l, Frame{.kind = static_cast<std::uint8_t>(Kind::subscribe), .channel = ch,
                          .payload = openbus::address(Fields{{"pattern", pattern}}, {})});
        }
    }

    void unsubscribe(Client& c, std::uint32_t channel) {
        std::erase_if(subscriptions, [&](const Subscription& s) { return s.client == c.id && s.channel == channel; });
        for (auto it = forwarded.begin(); it != forwarded.end();) {
            if (it->second == std::pair{c.id, channel}) {
                if (Client* l = client(it->first.first))
                    send(*l, Frame{.kind = static_cast<std::uint8_t>(Kind::unsubscribe), .channel = it->first.second});
                it = forwarded.erase(it);
            } else {
                ++it;
            }
        }
    }

    void broadcast(Client& c, const Frame& f) {
        auto env = openbus::open_envelope(f.payload);
        if (!env) return;
        const auto scope = openbus::scope(env->fields.value("scope", "host")).value_or(openbus::Scope::host);
        Fields out{{"scope", std::string(openbus::name_of(scope))}};
        out.add("from", c.peer.link ? env->fields.value("from") : c.peer.node);
        for (auto& [cid, other] : clients) {
            if (cid == c.id || other.gone || !other.hello) continue;
            if (other.peer.link && scope < openbus::Scope::link) continue;
            send(other, Frame{.kind = f.kind, .flags = f.flags, .codec = f.codec, .payload = openbus::address(out, env->body)});
        }
        trace(f, c.id, 0, "broadcast");
    }

    // ── the router's own endpoints (SPEC.md §8) ──────────────────────

    std::string topology() const {
        Json nodes = Json::array(), eps = Json::array(), links = Json::array();
        for (const auto& [cid, c] : clients) {
            if (c.gone || !c.hello) continue;
            Json n{{"node", c.peer.node}, {"name", c.peer.name}, {"actor", c.peer.actor}, {"identity", c.peer.identity},
                   {"transport", c.conn->channel().transport()}, {"handles", c.conn->channel().carries_handles()}};
            if (c.peer.link) links.push_back({{"router", c.peer.node}, {"root", c.peer.root}, {"transport", c.conn->channel().transport()},
                                              {"handles", c.conn->channel().carries_handles()}});
            else nodes.push_back(std::move(n));
        }
        for (const auto& [path, e] : endpoints) {
            const auto it = clients.find(e.owner);
            Json one{{"path", path}, {"kind", std::string(openbus::name_of(e.kind))},
                     {"node", it == clients.end() ? id : it->second.peer.node},
                     {"name", it == clients.end() ? std::string("router") : it->second.peer.name}};
            if (!e.via.empty()) one["via"] = e.via;
            eps.push_back(std::move(one));
        }
        return Json{{"router", id}, {"root", o.root}, {"openbus", std::string(openbus::kVersion)}, {"nodes", nodes},
                    {"endpoints", eps}, {"links", links}}.dump();
    }

    std::string stats() const {
        Json eps = Json::array(), nodes = Json::array();
        for (const auto& [path, e] : endpoints) {
            const auto& s = e.stats;
            eps.push_back({{"path", path}, {"calls", s.calls}, {"replies", s.replies}, {"publishes", s.publishes}, {"events", s.events},
                           {"bytes", s.bytes}, {"errors", s.errors}, {"drops", s.drops}, {"fallbacks", s.fallbacks},
                           {"latency_ns", {{"count", s.latency.count()}, {"p50", s.latency.percentile(0.5)},
                                           {"p99", s.latency.percentile(0.99)}, {"max", s.latency.percentile(1.0)},
                                           {"buckets", s.latency.encode()}}}});
        }
        for (const auto& [cid, c] : clients) {
            if (c.gone || !c.hello) continue;
            nodes.push_back({{"node", c.peer.node}, {"name", c.peer.name}, {"link", c.peer.link},
                             {"frames_in", c.conn->frames_in}, {"frames_out", c.conn->frames_out},
                             {"bytes_in", c.conn->bytes_in}, {"bytes_out", c.conn->bytes_out},
                             {"queued", c.conn->queued_bytes()}, {"dropped", c.dropped}, {"fallbacks", c.conn->fallbacks}});
        }
        return Json{{"router", id}, {"root", o.root}, {"frames", frames}, {"endpoints", eps}, {"nodes", nodes}}.dump();
    }

    std::string verbs() const {
        Json out = Json::array();
        for (const auto& [path, e] : endpoints) {
            if (!e.meta.get("verb")) continue;
            Json v{{"path", path}, {"verb", e.meta.value("verb")}, {"effect", e.meta.value("effect", "read")},
                   {"approval", e.meta.value("approval", "never")}};
            if (const auto in = e.meta.get("input")) v["input"] = std::string(*in);
            if (const auto out2 = e.meta.get("output")) v["output"] = std::string(*out2);
            out.push_back(std::move(v));
        }
        return out.dump();
    }

    // The router is an object on its own bus (SPEC.md §8.2): it is managed
    // the way everything on the bus is used.
    void own(Client& c, const Frame& f, const std::string& path) {
        const auto what = path.substr(o.root.size() + 5);
        const auto env = openbus::open_envelope(f.payload);
        const std::string body_in = env ? std::string(env->body) : std::string{};
        std::string body;
        if (what == "stats") body = stats();
        else if (what == "topology") body = topology();
        else if (what == "verbs") body = verbs();
        else if (what == "links") body = links_json();
        else if (what == "telemetry") {
            // The process's telemetry; a dump also carries what the router is.
            auto answer = telemetry::control(body_in);
            if (answer.contains("telemetry") && answer.contains("state"))
                answer["state"]["xbus.router." + id] = {{"topology", Json::parse(topology())}, {"stats", Json::parse(stats())}};
            body = answer.dump(-1, ' ', false, Json::error_handler_t::replace);
        }
        else if (what == "link" || what == "unlink") {
            if (c.peer.link || !allowed(c, what, path)) {
                send(c, error_(f.channel, openbus::ErrorCode::denied, "not allowed to " + what + " this bus"));
                return;
            }
            if (what == "link") {
                const auto to = Address::parse(body_in);
                if (!to) {
                    send(c, error_(f.channel, openbus::ErrorCode::malformed, "bus/link takes an address (local:..., unix:...)"));
                    return;
                }
                if (auto linked = connect_link(*to); !linked) {
                    send(c, error_(f.channel, openbus::ErrorCode::gone, linked.error()));
                    return;
                }
                note({{"event", "link"}, {"to", to->text()}, {"by", c.peer.node}, {"actor", c.peer.actor}});
                body = Json{{"linked", to->text()}}.dump();
            } else {
                int gone = 0;
                for (auto& [cid, l] : clients)
                    if (l.peer.link && (l.peer.node == body_in || l.peer.root == body_in)) { l.gone = true; ++gone; }
                note({{"event", "unlink"}, {"which", body_in}, {"by", c.peer.node}, {"links", gone}});
                body = Json{{"unlinked", gone}}.dump();
            }
        } else {
            send(c, error_(f.channel, openbus::ErrorCode::not_found,
                           "the router offers bus/stats, bus/topology, bus/verbs, bus/links, bus/link, bus/unlink, bus/telemetry, bus/trace"));
            return;
        }
        // An answer larger than a frame comes as several replies: their concatenation (SPEC.md §8.3).
        for (std::size_t at = 0; at < body.size() || at == 0; at += kPiece) {
            send(c, Frame{.kind = static_cast<std::uint8_t>(Kind::reply), .codec = static_cast<std::uint16_t>(openbus::Codec::json),
                          .channel = f.channel, .payload = body.substr(at, kPiece)});
            if (body.empty()) break;
        }
        send(c, Frame{.kind = static_cast<std::uint8_t>(Kind::reply_end), .channel = f.channel});
    }

    std::string links_json() const {
        Json out = Json::array();
        for (const auto& [cid, l] : clients)
            if (l.peer.link && l.hello && !l.gone)
                out.push_back({{"router", l.peer.node}, {"root", l.peer.root}, {"transport", l.conn->channel().transport()},
                               {"frames_in", l.conn->frames_in}, {"frames_out", l.conn->frames_out}});
        return out.dump();
    }

    std::expected<void, std::string> connect_link(const Address& to);

    // ── connections ──────────────────────────────────────────────────

    Fields hello_fields(bool as_link) const {
        Fields f{{"openbus", std::string(openbus::kVersion)}, {"node", id}, {"name", "router"}, {"actor", "service:xbus-router"},
                 {"root", o.root}, {"router", id}};
        for (const auto* feature : {"descriptors", "regions", "retained"}) f.add("feature", feature);
        if (as_link) f.add("feature", "link");
        return f;
    }

    // A link that is up: everything known is offered to it, every
    // subscription forwarded over it.
    void link_up(Client& l) {
        for (const auto& [path, e] : endpoints)
            if (e.owner != l.id && !std::ranges::contains(e.via, l.peer.node)) send(l, control_(Kind::offer, 0, offer_fields(path, e)));
        for (const auto& s : subscriptions) {
            if (s.client == l.id) continue;
            const auto ch = l.channel();
            forwarded[{l.id, ch}] = {s.client, s.channel};
            send(l, Frame{.kind = static_cast<std::uint8_t>(Kind::subscribe), .channel = ch,
                          .payload = openbus::address(Fields{{"pattern", s.pattern}}, {})});
        }
        note({{"event", "link-up"}, {"router", l.peer.node}, {"root", l.peer.root}});
    }

    void hello(Client& c, const Frame& f) {
        const auto fields = Fields::decode(f.payload);
        if (!fields) { c.gone = true; return; }
        c.peer.node = fields->value("node", random_hex_(16));
        c.peer.name = fields->value("name", "node");
        c.peer.actor = fields->value("actor", "program");
        c.peer.link = std::ranges::contains(fields->all("feature"), "link");
        c.peer.root = fields->value("root");
        c.hello = true;
        Fields w = hello_fields(false);
        w.add("scope", c.peer.link ? "link" : "host");
        w.add("limit-payload", static_cast<std::int64_t>(openbus::kMaxPayload));
        w.add("limit-queue", static_cast<std::int64_t>(o.queue_limit));
        send(c, control_(Kind::welcome, f.channel, w));
        note({{"event", "connect"}, {"node", c.peer.node}, {"name", c.peer.name}, {"actor", c.peer.actor}, {"identity", c.peer.identity},
              {"link", c.peer.link}});
        if (c.peer.link) link_up(c);
        else last_node_at = native::now_ns();
    }

    void welcome(Client& c, const Frame& f) {
        // A link this side opened: the other router said who it is.
        if (!c.initiated_link) return;
        const auto fields = Fields::decode(f.payload);
        if (!fields) return;
        c.peer.node = fields->value("router");
        c.peer.name = "router";
        c.peer.root = fields->value("root");
        c.peer.link = true;
        c.hello = true;
        link_up(c);
    }

    void handle(Client& c, Incoming in) {
        auto& f = in.frame;
        ++frames;
        m_frames.add();
        telemetry::Span span(openbus::name_of(f.known()), &m_frame, "xbus.router");
        if (!c.hello && !f.is(Kind::hello) && !(c.initiated_link && f.is(Kind::welcome))) {
            send(c, error_(f.channel, openbus::ErrorCode::malformed, "the first frame is hello"));
            c.gone = true;
            return;
        }
        if (f.kind == 0 || f.kind > openbus::kLastKind) {
            send(c, error_(f.channel, openbus::ErrorCode::unknown_kind, std::format("kind {} is not openbus {}", f.kind, openbus::kVersion)));
            return;
        }
        switch (f.known()) {
        case Kind::hello: hello(c, f); break;
        case Kind::welcome: welcome(c, f); break;
        case Kind::offer: offer(c, f); break;
        case Kind::withdraw: {
            const auto fields = Fields::decode(f.payload);
            const auto path = fields ? abs(fields->value("path")) : std::string{};
            if (auto it = endpoints.find(path); it != endpoints.end() && it->second.owner == c.id) withdraw(path);
            break;
        }
        case Kind::resolve: resolve(c, f); break;
        case Kind::call:
        case Kind::stream_open: {
            // Over a link, a delivery on a forwarded channel is not a call.
            open_route(c, std::move(f), std::move(in.handle));
            break;
        }
        case Kind::reply: case Kind::reply_end: case Kind::cancel: case Kind::stream_data: case Kind::stream_close:
        case Kind::credit: case Kind::region_announce: case Kind::region_attach: case Kind::region_release: case Kind::error:
            (void)follow(c, std::move(f), std::move(in.handle));
            break;
        case Kind::subscribe: subscribe(c, f); break;
        case Kind::unsubscribe: unsubscribe(c, f.channel); break;
        case Kind::publish: case Kind::event: publish(c, f); break;
        case Kind::broadcast: broadcast(c, f); break;
        case Kind::ping: send(c, Frame{.kind = static_cast<std::uint8_t>(Kind::pong), .channel = f.channel, .payload = f.payload}); break;
        case Kind::pong: case Kind::resolved: break;
        }
    }

    void drop(Client& c) {
        // Its endpoints withdrawn; calls to it answered gone; its calls cancelled.
        std::vector<std::string> owned;
        for (const auto& [path, e] : endpoints)
            if (e.owner == c.id) owned.push_back(path);
        for (const auto& p : owned) withdraw(p);
        for (auto it = routes.begin(); it != routes.end();) {
            if (it->first.first != c.id) { ++it; continue; }
            const auto r = it->second;
            if (Client* other = client(r.other)) {
                if (r.caller) send(*other, Frame{.kind = static_cast<std::uint8_t>(r.kind == Kind::call ? Kind::cancel : Kind::stream_close),
                                                 .channel = r.other_channel});
                else if (r.kind == Kind::call) send(*other, error_(r.other_channel, openbus::ErrorCode::gone, r.path + " is gone"));
                else send(*other, Frame{.kind = static_cast<std::uint8_t>(Kind::stream_close), .channel = r.other_channel});
            }
            routes.erase({r.other, r.other_channel});
            it = routes.erase(it);
        }
        std::vector<std::uint32_t> mine;
        for (const auto& s : subscriptions)
            if (s.client == c.id) mine.push_back(s.channel);
        for (const auto ch : mine) unsubscribe(c, ch);
        std::erase_if(forwarded, [&](const auto& kv) { return kv.first.first == c.id; });
        note({{"event", "disconnect"}, {"node", c.peer.node}, {"name", c.peer.name}, {"link", c.peer.link}});
        if (!c.peer.link) last_node_at = native::now_ns();
    }

    // ── connections ──────────────────────────────────────────────────

    struct Listening {
        Address address;
        Handle handle;
    };
    std::vector<Listening> listeners;
    std::array<Handle, 2> wake;              // a pipe's feeding thread knocks here
    std::vector<std::shared_ptr<PipeEnd>> pipe_ends;

    void knock() {
        if (wake[1].valid()) (void)native::write(wake[1], "w");
    }

    Client& add(std::unique_ptr<Channel> ch, bool initiated_link) {
        auto& c = clients[next_client];
        c.id = next_client++;
        c.peer.identity = ch->identity();
        c.conn = std::make_unique<Conn>(std::move(ch));
        c.initiated_link = initiated_link;
        return c;
    }

    std::shared_ptr<PipeEnd> add_pipe(Pipe p, bool initiated_link) {
        std::shared_ptr<PipeEnd> end;
        auto ch = pipe_channel(std::move(p), [this] { knock(); }, &end);
        auto& c = add(std::move(ch), initiated_link);
        if (initiated_link) c.conn->push(control_(Kind::hello, 0, hello_fields(true)));
        knock();
        return end;
    }

    void round(int timeout_ms) {
        std::vector<native::Ready> set;
        std::vector<int> ids;   // 0 the wake, -k a listener, else a client
        if (wake[0].valid()) { set.push_back({.handle = &wake[0]}); ids.push_back(0); }
        for (std::size_t k = 0; k < listeners.size(); ++k) {
            set.push_back({.handle = &listeners[k].handle});
            ids.push_back(-static_cast<int>(k) - 1);
        }
        bool fed = false;
        for (auto& [cid, c] : clients) {
            if (const auto* h = c.conn->channel().waitable()) {
                set.push_back({.handle = h, .want_write = c.conn->pending()});
                ids.push_back(cid);
            } else {
                fed = true;
            }
        }
        native::wait(set, timeout_ms);
        // What the round did, apart from waiting: its busy time.
        telemetry::Span busy("round", &m_round, "xbus.router");
        bool worked = fed;
        std::vector<int> ready;
        for (std::size_t k = 0; k < set.size(); ++k) {
            if (ids[k] == 0) {
                if (set[k].readable) {
                    std::array<char, 256> drain{};
                    (void)native::read(wake[0], drain);
                }
                continue;
            }
            if (ids[k] < 0) {
                if (!set[k].readable) continue;
                auto& l = listeners[static_cast<std::size_t>(-ids[k] - 1)];
                for (auto h = native::accept(l.handle); h.valid(); h = native::accept(l.handle)) add(socket_channel(std::move(h)), false);
                worked = true;
                continue;
            }
            if (set[k].readable || set[k].closed) ready.push_back(ids[k]);
        }
        // A pipe is read every round: what was fed knocked first.
        if (fed)
            for (auto& [cid, c] : clients)
                if (!c.conn->channel().waitable()) ready.push_back(cid);
        for (const int id : ready) {
            Client* c = client(id);
            if (!c) continue;
            std::vector<Incoming> in;
            std::string why;
            const bool alive = c->conn->read(in, &why);
            for (auto& i : in) {
                if (c->gone) continue;
                handle(*c, std::move(i));
            }
            if (!alive) {
                if (why.find("larger") != std::string::npos) {
                    c->conn->push(error_(0, openbus::ErrorCode::too_large, "a frame larger than 1 MiB"));
                    (void)c->conn->flush();
                }
                c->gone = true;
            }
        }
        // Everything queued this round, written; the gone ones dropped.
        for (auto& [cid, c] : clients)
            if (!c.gone && c.conn->pending() && !c.conn->flush()) c.gone = true;
        for (auto it = clients.begin(); it != clients.end();) {
            if (!it->second.gone) { ++it; continue; }
            drop(it->second);
            it = clients.erase(it);
            worked = true;
        }
        if (!worked && ready.empty()) {
            busy.discard();
            return;
        }
        std::int64_t queued = 0;
        std::size_t n = 0, l = 0;
        for (const auto& [cid, c] : clients) {
            queued += static_cast<std::int64_t>(c.conn->queued_bytes());
            if (c.hello) ++(c.peer.link ? l : n);
        }
        m_queued.set(queued);
        m_clients.set(static_cast<std::int64_t>(clients.size()));
        seen_nodes.store(n);
        seen_links.store(l);
        seen_endpoints.store(endpoints.size());
    }
};

std::expected<void, std::string> Router::Impl::connect_link(const Address& to) {
    auto h = native::connect(to.scheme, to.where);
    if (!h) return std::unexpected(h.error());
    auto& c = add(socket_channel(std::move(*h)), true);
    c.conn->push(control_(Kind::hello, 0, hello_fields(true)));
    knock();
    return {};
}

Router::Router(RouterOptions o) : impl_(std::make_unique<Impl>()) {
    impl_->o = std::move(o);
    if (auto w = native::pair()) impl_->wake = std::move(*w);
    if (impl_->o.root.empty()) impl_->o.root = host_root();
    impl_->log.open({.file = impl_->o.log, .component = "xbus-router"});
    telemetry::provide("xbus.router." + impl_->id, [impl = impl_.get()] {
        return Json{{"root", impl->o.root}, {"nodes", impl->seen_nodes.load()}, {"links", impl->seen_links.load()},
                    {"endpoints", impl->seen_endpoints.load()}};
    });
    // The router's own endpoints: it is an object of its bus, openbus.router.
    const auto router_object = [](std::string verb, std::string effect) {
        return Fields{{"verb", std::move(verb)}, {"effect", std::move(effect)}, {"interface", "openbus.router@0.1"}};
    };
    for (const auto* what : {"stats", "topology", "verbs", "links"})
        impl_->endpoints[impl_->o.root + "/bus/" + what] = Endpoint{.owner = 0, .kind = openbus::EndpointKind::service,
                                                                    .meta = router_object(std::string("bus-") + what, "read")};
    for (const auto* what : {"link", "unlink"})
        impl_->endpoints[impl_->o.root + "/bus/" + what] = Endpoint{.owner = 0, .kind = openbus::EndpointKind::service,
                                                                    .meta = router_object(std::string("bus-") + what, "external")};
    impl_->endpoints[impl_->o.root + "/bus/trace"] = Endpoint{.owner = 0, .kind = openbus::EndpointKind::topic,
                                                              .meta = Fields{{"interface", "openbus.router@0.1"}}};
    auto told = router_object("telemetry", "reversible");
    told.add("interface", "openbus.telemetry@0.1");
    impl_->endpoints[impl_->o.root + "/bus/telemetry"] = Endpoint{.owner = 0, .kind = openbus::EndpointKind::service, .meta = std::move(told)};
}

Router::~Router() {
    telemetry::withdraw("xbus.router." + impl_->id);
    for (const auto& l : impl_->listeners) native::forget(l.address.scheme, l.address.where);
}

std::expected<void, std::string> Router::listen() {
    for (const auto& a : impl_->o.listen) {
        auto h = native::listen(a.scheme, a.where);
        if (!h) return std::unexpected(h.error());
        impl_->note({{"event", "listen"}, {"address", a.text()}, {"at", native::location(a.scheme, a.where)}, {"root", impl_->o.root}});
        impl_->listeners.push_back({a, std::move(*h)});
    }
    return {};
}

std::shared_ptr<PipeEnd> Router::adopt(Pipe p) { return impl_->add_pipe(std::move(p), false); }

std::expected<void, std::string> Router::link(const Address& to) { return impl_->connect_link(to); }

std::shared_ptr<PipeEnd> Router::link(Pipe p) { return impl_->add_pipe(std::move(p), true); }

void Router::run_once(int timeout_ms) { impl_->round(timeout_ms); }

void Router::run(const std::atomic<bool>& stop) {
    while (!stop.load()) {
        impl_->round(200);
        if (impl_->o.idle_exit && nodes() == 0
            && native::now_ns() - impl_->last_node_at > static_cast<std::uint64_t>(impl_->o.idle_exit->count()) * 1'000'000'000ull)
            return;
    }
}

const std::string& Router::root() const { return impl_->o.root; }
const std::string& Router::id() const { return impl_->id; }
std::size_t Router::nodes() const {
    return static_cast<std::size_t>(std::ranges::count_if(impl_->clients, [](const auto& kv) { return kv.second.hello && !kv.second.peer.link; }));
}
std::size_t Router::links() const {
    return static_cast<std::size_t>(std::ranges::count_if(impl_->clients, [](const auto& kv) { return kv.second.hello && kv.second.peer.link; }));
}
std::size_t Router::endpoints() const { return impl_->endpoints.size(); }
std::string Router::topology_json() const { return impl_->topology(); }
std::string Router::stats_json() const { return impl_->stats(); }

}  // namespace xbus
