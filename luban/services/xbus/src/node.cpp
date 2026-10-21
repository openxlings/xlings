module xbus.node;

import std;
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

using openbus::Fields;
using openbus::Frame;
using openbus::Kind;

Frame control_(Kind k, std::uint32_t channel, const Fields& f) {
    return Frame{.kind = static_cast<std::uint8_t>(k), .codec = static_cast<std::uint16_t>(openbus::Codec::fields), .channel = channel,
                 .payload = f.encode()};
}

constexpr std::size_t kPiece = 512u << 10;   // an answer larger than a frame comes as several replies
constexpr auto kJson = static_cast<std::uint16_t>(openbus::Codec::json);

CallError error_of_(const Frame& f) {
    const auto fields = Fields::decode(f.payload);
    return CallError{fields ? openbus::error_code(fields->value("code")).value_or(openbus::ErrorCode::malformed) : openbus::ErrorCode::malformed,
                     fields ? fields->value("message") : std::string("a malformed error")};
}

}  // namespace

// ── streams and regions: what a Stream and a Region hold ─────────────

struct RegionState {
    std::uint32_t id { 0 };
    std::size_t size { 0 };
    bool shared { false };
    native::Shared mapped;               // shared: both sides map it
    std::vector<std::byte> local;        // inline: this side's copy
    std::byte* data() { return shared ? mapped.data : local.data(); }
    ~RegionState() { if (shared) native::unmap(mapped); }
};

struct StreamState {
    std::uint32_t channel { 0 };         // on this node's connection
    std::string path, from, body;
    std::uint64_t window { 1u << 20 };
    std::function<bool(Frame, Handle)> send;   // through the node, while it lives
    std::function<void()> forget;              // and out of its streams
    mutable std::mutex m;
    std::condition_variable cv;
    std::uint64_t credit { 0 };          // what this side may still send
    std::uint64_t unacked { 0 };         // read since the last grant
    bool granted { false };              // the other side's first credit came
    std::deque<std::pair<Chunk, std::uint64_t>> inbox;   // and what each cost
    bool ended { false };
    std::string error;
    std::uint32_t next_region { 1 };
    std::map<std::uint32_t, std::shared_ptr<RegionState>> mine, theirs;
    std::map<std::uint32_t, int> answers;                  // a region of mine: 1 mapped, -1 refused

    void end(std::string why = {}) {
        std::lock_guard l(m);
        if (ended) return;
        ended = true;
        error = std::move(why);
        cv.notify_all();
    }
};

namespace {

Frame credit_frame_(std::uint32_t channel, std::uint64_t bytes) {
    return control_(Kind::credit, channel, Fields{{"bytes", std::to_string(bytes)}});
}

std::uint64_t number_(const Fields& f, std::string_view key) {
    const auto v = f.value(key);
    std::uint64_t n = 0;
    std::from_chars(v.data(), v.data() + v.size(), n);
    return n;
}

}  // namespace

struct Node::Impl {
    NodeOptions o;
    std::string id;
    mutable std::mutex m;
    std::condition_variable cv;
    std::unique_ptr<Conn> conn;
    std::array<Handle, 2> wake;
    std::jthread io;
    std::atomic<bool> stopping { false };
    bool welcomed { false };
    std::string root, router;
    std::uint32_t next_channel { 1 };

    struct Offered { OfferOptions o; CallHandler handler; StreamHandler stream; StreamOptions so; };
    // Streams open on this node's connection, by its channel.
    std::map<std::uint32_t, std::shared_ptr<StreamState>> streams;
    telemetry::Counter& m_region_fallbacks = telemetry::metrics().counter("xbus.node.region_fallbacks");
    telemetry::Counter& m_stream_bytes = telemetry::metrics().counter("xbus.node.stream_bytes");
    std::map<std::string, Offered> offered;                     // by the path as given
    std::map<std::string, std::string> absolute;                 // given -> absolute (the router's answer)
    struct Sub { std::string pattern; MessageHandler handler; };
    std::map<std::uint32_t, Sub> subs;
    // A call this node made: its replies, its end.
    struct Pending {
        std::vector<std::string> replies;
        std::function<void(std::string_view)> each;
        std::optional<CallError> error;
        std::vector<Resolved> resolved;
        bool done { false };
        std::uint64_t sent_at { 0 }, done_at { 0 };
    };
    std::map<std::uint32_t, std::shared_ptr<Pending>> pending;
    // Calls it answers, by the router's channel: whether they were cancelled.
    std::map<std::uint32_t, std::shared_ptr<std::atomic<bool>>> answering;
    std::weak_ptr<Node> self;
    Node* raw { nullptr };
    bool lost { false };   // the router went away once: the next welcome is a reconnection

    telemetry::Counter& m_calls = telemetry::metrics().counter("xbus.node.calls");
    telemetry::Counter& m_failed = telemetry::metrics().counter("xbus.node.calls_failed");
    telemetry::Counter& m_answered = telemetry::metrics().counter("xbus.node.answered");
    telemetry::Counter& m_published = telemetry::metrics().counter("xbus.node.published");
    telemetry::Counter& m_delivered = telemetry::metrics().counter("xbus.node.delivered");
    telemetry::Counter& m_reconnects = telemetry::metrics().counter("xbus.node.reconnects");
    telemetry::Histogram& m_call = telemetry::metrics().histogram("xbus.node.call_ns");
    telemetry::Histogram& m_handler = telemetry::metrics().histogram("xbus.node.handler_ns");

    std::uint32_t channel() { return next_channel++ & 0x7fffffffu; }

    void kick() {
        if (wake[1].valid()) (void)native::write(wake[1], "w");
    }

    bool send_with(Frame f, Handle h) {
        std::lock_guard l(m);
        if (!conn || !welcomed) return false;
        const bool idle = !conn->pending();
        if (h.valid()) f.flags |= openbus::flag::descriptors;
        conn->push(std::move(f), std::move(h));
        if (idle) kick();
        return true;
    }

    std::shared_ptr<StreamState> stream_state(std::uint32_t ch, std::string path, std::uint64_t window) {
        auto st = std::make_shared<StreamState>();
        st->channel = ch;
        st->path = std::move(path);
        st->window = window;
        st->send = [w = self](Frame f, Handle h) {
            auto n = w.lock();
            return n && n->impl_->send_with(std::move(f), std::move(h));
        };
        st->forget = [w = self, ch] {
            if (auto n = w.lock()) {
                std::lock_guard l(n->impl_->m);
                n->impl_->streams.erase(ch);
            }
        };
        return st;
    }

    // A frame on a stream's channel (the node's thread).
    void stream_frame(const std::shared_ptr<StreamState>& st, Incoming& in) {
        auto& f = in.frame;
        switch (f.known()) {
        case Kind::credit: {
            const auto fields = Fields::decode(f.payload);
            std::lock_guard l(st->m);
            st->credit += fields ? number_(*fields, "bytes") : 0;
            st->granted = true;
            st->cv.notify_all();
            break;
        }
        case Kind::stream_data: {
            Chunk c;
            const std::uint64_t cost = f.payload.size();
            if (f.flags & openbus::flag::region_ref) {
                c.ref = openbus::decode_region_ref(f.payload);
                if (!c.ref) break;
                std::lock_guard l(st->m);
                // An inline region's bytes come with the reference: into this side's copy.
                if (auto r = st->theirs.find(c.ref->id); r != st->theirs.end() && !r->second->shared) {
                    const auto bytes = std::string_view(f.payload).substr(24);
                    if (c.ref->offset + bytes.size() <= r->second->size)
                        std::memcpy(r->second->data() + c.ref->offset, bytes.data(), bytes.size());
                }
                st->inbox.emplace_back(std::move(c), cost);
            } else {
                c.bytes = std::move(f.payload);
                std::lock_guard l(st->m);
                st->inbox.emplace_back(std::move(c), cost);
            }
            m_stream_bytes.add(cost);
            st->cv.notify_all();
            break;
        }
        case Kind::region_announce: {
            const auto fields = Fields::decode(f.payload);
            if (!fields) break;
            auto r = std::make_shared<RegionState>();
            r->id = static_cast<std::uint32_t>(number_(*fields, "id"));
            r->size = static_cast<std::size_t>(number_(*fields, "size"));
            const auto kind = fields->value("kind");
            bool ok = false;
            if (kind == "inline") {
                r->local.resize(r->size);
                ok = true;
            } else if (kind == "memfd" && in.handle.valid()) {
                if (auto mapped = native::map_shared(std::move(in.handle), r->size)) {
                    r->mapped = std::move(*mapped);
                    r->shared = true;
                    ok = true;
                }
            }
            if (ok) {
                {
                    std::lock_guard l(st->m);
                    st->theirs[r->id] = r;
                }
                (void)send_with(control_(Kind::region_attach, st->channel, Fields{{"id", std::to_string(r->id)}}), {});
            } else {
                // This side cannot map it (it came without its memory): the other side goes inline.
                (void)send_with(control_(Kind::region_release, st->channel,
                                         Fields{{"id", std::to_string(r->id)}, {"error", std::string(openbus::name_of(openbus::ErrorCode::region_kind))}}), {});
            }
            break;
        }
        case Kind::region_attach:
        case Kind::region_release: {
            const auto fields = Fields::decode(f.payload);
            if (!fields) break;
            const auto id = static_cast<std::uint32_t>(number_(*fields, "id"));
            std::lock_guard l(st->m);
            if (f.is(Kind::region_attach)) st->answers[id] = 1;
            else if (fields->get("error")) st->answers[id] = -1;
            else st->theirs.erase(id);   // its owner let it go
            st->cv.notify_all();
            break;
        }
        case Kind::stream_close: {
            st->end();
            std::lock_guard l(m);
            streams.erase(st->channel);
            break;
        }
        case Kind::error: {
            st->end(error_of_(f).message);
            std::lock_guard l(m);
            streams.erase(st->channel);
            break;
        }
        default:
            break;
        }
    }

    bool send(const Frame& f) {
        std::lock_guard l(m);
        if (!conn || !welcomed) return false;
        // The thread is woken when there was nothing to write: while it has
        // something it writes everything, and a knock per frame is a system
        // call per frame.
        const bool idle = !conn->pending();
        conn->push(f);
        if (idle) kick();
        return true;
    }

    std::expected<void, std::string> open() {
        const auto at = o.router.value_or(default_address());
        auto h = native::connect(at.scheme, at.where);
        if (!h && !o.router_program.empty()) {
            native::spawn_detached({o.router_program.string(), "router", "--detach", "--listen", at.text()});
            const auto until = std::chrono::steady_clock::now() + o.connect_wait;
            while (!h && std::chrono::steady_clock::now() < until) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                h = native::connect(at.scheme, at.where);
            }
        }
        if (!h) return std::unexpected("no openbus router answers at " + at.text() + " (" + h.error() + ")");
        auto c = std::make_unique<Conn>(socket_channel(std::move(*h)));
        Fields hello{{"openbus", std::string(openbus::kVersion)}, {"node", id}, {"name", o.name},
                     {"actor", o.actor.empty() ? "program:" + o.name : o.actor}};
        for (const auto* feature : {"descriptors", "regions", "retained"}) hello.add("feature", feature);
        c->push(control_(Kind::hello, 0, hello));
        std::lock_guard l(m);
        conn = std::move(c);
        welcomed = false;
        return {};
    }

    // After a welcome: what this node had, offered and subscribed again.
    void restore() {
        for (const auto& [path, of] : offered) conn->push(offer_frame(path, of.o, 0));
        for (const auto& [ch, s] : subs)
            if (ch != 0) conn->push(Frame{.kind = static_cast<std::uint8_t>(Kind::subscribe), .channel = ch,
                             .payload = openbus::address(Fields{{"pattern", s.pattern}}, {})});
    }

    Frame offer_frame(const std::string& path, const OfferOptions& oo, std::uint32_t ch) {
        Fields f{{"path", path}, {"kind", std::string(openbus::name_of(oo.kind))}};
        if (oo.retained) f.add("retained", "1");
        if (oo.codec) f.add("codec", oo.codec);
        if (!oo.verb.empty()) f.add("verb", oo.verb);
        if (!oo.effect.empty()) f.add("effect", oo.effect);
        if (!oo.approval.empty()) f.add("approval", oo.approval);
        if (!oo.input.empty()) f.add("input", oo.input);
        if (!oo.output.empty()) f.add("output", oo.output);
        for (const auto& i : oo.interfaces) f.add("interface", i);
        for (const auto& [k, v] : oo.properties) f.add(k, v);
        for (const auto& m2 : oo.members) f.add("member", m2);
        if (!oo.policy.empty()) f.add("policy", oo.policy);
        return control_(Kind::offer, ch, f);
    }

    void finish(std::uint32_t ch, std::optional<CallError> err) {
        auto it = pending.find(ch);
        if (it == pending.end()) return;
        it->second->error = std::move(err);
        it->second->done = true;
        it->second->done_at = native::now_ns();
        pending.erase(it);
        cv.notify_all();
    }

    void handle(Incoming in) {
        auto& f = in.frame;
        if (f.is(Kind::credit) || f.is(Kind::stream_data) || f.is(Kind::stream_close) || f.is(Kind::region_announce)
            || f.is(Kind::region_attach) || f.is(Kind::region_release) || f.is(Kind::error)) {
            std::shared_ptr<StreamState> st;
            {
                std::lock_guard l(m);
                if (auto it = streams.find(f.channel); it != streams.end()) st = it->second;
            }
            if (st) {
                stream_frame(st, in);
                return;
            }
        }
        switch (f.known()) {
        case Kind::welcome: {
            const auto fields = Fields::decode(f.payload);
            std::lock_guard l(m);
            welcomed = true;
            root = fields ? fields->value("root") : std::string{};
            router = fields ? fields->value("router") : std::string{};
            if (lost) {
                lost = false;
                m_reconnects.add();
                telemetry::log().info("xbus-reconnected", {{"node", id}, {"name", o.name}, {"router", router}});
            }
            restore();
            cv.notify_all();
            break;
        }
        case Kind::resolved: {
            std::lock_guard l(m);
            if (auto it = pending.find(f.channel); it != pending.end()) {
                const auto fields = Fields::decode(f.payload);
                if (fields) {
                    Resolved r{.path = fields->value("path"),
                               .kind = openbus::endpoint_kind(fields->value("kind")).value_or(openbus::EndpointKind::service),
                               .node = fields->value("node"), .name = fields->value("name")};
                    for (auto v : std::views::split(fields->value("via"), ','))
                        if (std::string_view s(v.begin(), v.end()); !s.empty()) r.via.emplace_back(s);
                    for (const auto i : fields->all("interface")) r.interfaces.emplace_back(i);
                    it->second->resolved.push_back(std::move(r));
                    // An offer's answer is its one resolved frame.
                    if (it->second->each == nullptr && it->second->replies.empty() && f.channel & 0x40000000u) finish(f.channel, std::nullopt);
                }
            }
            break;
        }
        case Kind::reply: {
            std::function<void(std::string_view)> each;
            {
                std::lock_guard l(m);
                auto it = pending.find(f.channel);
                if (it == pending.end()) break;
                if (it->second->each) each = it->second->each;
                else it->second->replies.push_back(f.payload);
            }
            if (each) each(f.payload);
            break;
        }
        case Kind::reply_end: { std::lock_guard l(m); finish(f.channel, std::nullopt); break; }
        case Kind::pong: { std::lock_guard l(m); finish(f.channel, std::nullopt); break; }
        case Kind::error: {
            std::lock_guard l(m);
            if (pending.contains(f.channel)) finish(f.channel, error_of_(f));
            else if (f.channel == 0) {
                // What restore() said again and the router refused (a path taken meanwhile).
                const auto e = error_of_(f);
                telemetry::log().warn("xbus-refused", {{"node", id}, {"name", o.name}, {"code", openbus::name_of(e.code)}, {"message", e.message}});
            }
            break;
        }
        case Kind::call: {
            const auto env = openbus::open_envelope(f.payload);
            if (!env) break;
            const auto path = env->fields.value("path");
            CallHandler handler;
            auto cancelled = std::make_shared<std::atomic<bool>>(false);
            {
                std::lock_guard l(m);
                for (const auto& [given, of] : offered) {
                    const auto a = absolute.find(given);
                    const auto placed = a != absolute.end() ? a->second : given.starts_with("host/") ? given : root + "/" + given;
                    if (placed == path || given == path) { handler = of.handler; break; }
                }
                answering[f.channel] = cancelled;
            }
            Call c;
            c.node_ = self;
            c.raw_ = raw;
            c.channel_ = f.channel;
            c.path_ = path;
            c.from_ = env->fields.value("from");
            c.actor_ = env->fields.value("actor");
            c.body_ = std::string(env->body);
            c.codec_ = f.codec;
            c.cancelled_ = cancelled;
            c.ended_ = std::make_shared<std::atomic<bool>>(false);
            m_answered.add();
            telemetry::Span span("answer", &m_handler, "xbus.node");
            if (handler) handler(std::move(c));
            else c.fail(openbus::ErrorCode::not_found, "this node answers nothing at " + path);
            break;
        }
        case Kind::cancel: {
            std::lock_guard l(m);
            if (auto it = answering.find(f.channel); it != answering.end()) it->second->store(true);
            break;
        }
        case Kind::stream_open: {
            const auto env = openbus::open_envelope(f.payload);
            if (!env) break;
            const auto path = env->fields.value("path");
            StreamHandler handler;
            StreamOptions so;
            {
                std::lock_guard l(m);
                for (const auto& [given, of] : offered) {
                    const auto a = absolute.find(given);
                    const auto placed = a != absolute.end() ? a->second : given.starts_with("host/") ? given : root + "/" + given;
                    if (placed == path || given == path) { handler = of.stream; so = of.so; break; }
                }
            }
            if (!handler) {
                (void)send_with(control_(Kind::error, f.channel, Fields{{"code", std::string(openbus::name_of(openbus::ErrorCode::not_found))},
                                                                       {"message", "this node has no stream at " + path}}), {});
                break;
            }
            auto st = stream_state(f.channel, path, so.window);
            st->from = env->fields.value("from");
            st->body = std::string(env->body);
            st->credit = number_(env->fields, "credit");
            st->granted = true;
            {
                std::lock_guard l(m);
                streams[f.channel] = st;
            }
            (void)send_with(credit_frame_(f.channel, so.window), {});
            handler(Stream(st));
            break;
        }
        case Kind::publish:
        case Kind::event: {
            const auto env = openbus::open_envelope(f.payload);
            if (!env) break;
            MessageHandler handler;
            {
                std::lock_guard l(m);
                if (auto it = subs.find(f.channel); it != subs.end()) handler = it->second.handler;
            }
            if (!handler) break;
            Message msg{.path = env->fields.value("path"), .from = env->fields.value("from"), .actor = env->fields.value("actor"),
                        .body = std::string(env->body), .codec = f.codec, .event = f.is(Kind::event)};
            m_delivered.add();
            if (msg.event && msg.body.size() >= 8)
                for (int k = 7; k >= 0; --k) msg.value = (msg.value << 8) | static_cast<unsigned char>(msg.body[static_cast<std::size_t>(k)]);
            handler(msg);
            break;
        }
        case Kind::broadcast: {
            const auto env = openbus::open_envelope(f.payload);
            if (!env) break;
            MessageHandler handler;
            {
                std::lock_guard l(m);
                if (auto it = subs.find(0); it != subs.end()) handler = it->second.handler;
            }
            if (handler) handler(Message{.path = "broadcast", .from = env->fields.value("from"), .body = std::string(env->body)});
            break;
        }
        default:
            break;
        }
    }

    // The node's thread: read, dispatch, write; when the router goes away,
    // connect again (100 ms, doubling to 2 s) and say everything again.
    void loop(std::stop_token stop) {
        telemetry::name_thread("xbus-node " + o.name);
        auto backoff = std::chrono::milliseconds(100);
        while (!stop.stop_requested() && !stopping.load()) {
            const Handle* h = nullptr;
            bool want_write = false;
            {
                std::lock_guard l(m);
                if (conn) { h = conn->channel().waitable(); want_write = conn->pending(); }
            }
            if (!h) {
                std::this_thread::sleep_for(backoff);
                backoff = std::min(backoff * 2, std::chrono::milliseconds(2000));
                if (open()) backoff = std::chrono::milliseconds(100);
                continue;
            }
            std::array<native::Ready, 2> p{{{.handle = h, .want_write = want_write}, {.handle = &wake[0]}}};
            native::wait(p, 200);
            if (p[1].readable) {
                std::array<char, 256> drain{};
                (void)native::read(wake[0], drain);
            }
            std::vector<Incoming> in;
            bool alive = true;
            {
                std::lock_guard l(m);
                if (p[0].readable || p[0].closed) alive = conn->read(in);
            }
            for (auto& i : in) handle(std::move(i));
            {
                std::lock_guard l(m);
                if (alive && conn->pending()) alive = conn->flush();
                if (!alive) {
                    conn.reset();
                    welcomed = false;
                    lost = true;
                    // What was asked of the router that is gone is answered gone.
                    for (auto& [ch, pnd] : pending) {
                        pnd->error = CallError{openbus::ErrorCode::gone, "the router went away"};
                        pnd->done = true;
                    }
                    pending.clear();
                    answering.clear();
                    for (auto& [ch, st] : streams) st->end("the router went away");
                    streams.clear();
                    cv.notify_all();
                }
            }
        }
    }

    // A request's answer: waits for its end.
    std::expected<std::shared_ptr<Pending>, CallError> ask(const Frame& f, std::shared_ptr<Pending> p, std::chrono::milliseconds timeout) {
        std::unique_lock l(m);
        if (!cv.wait_for(l, std::chrono::milliseconds(o.connect_wait), [&] { return welcomed || stopping.load(); }) || !welcomed)
            return std::unexpected(CallError{openbus::ErrorCode::gone, "not connected to an openbus router"});
        p->sent_at = native::now_ns();
        pending[f.channel] = p;
        conn->push(f);
        kick();
        if (!cv.wait_for(l, timeout, [&] { return p->done; })) {
            pending.erase(f.channel);
            conn->push(Frame{.kind = static_cast<std::uint8_t>(Kind::cancel), .channel = f.channel});
            kick();
            if (f.is(Kind::call)) m_failed.add();
            return std::unexpected(CallError{openbus::ErrorCode::deadline, "no answer in time"});
        }
        if (p->error) {
            if (f.is(Kind::call)) m_failed.add();
            return std::unexpected(*p->error);
        }
        if (f.is(Kind::call)) m_call.record(p->done_at - p->sent_at);
        return p;
    }
};

Node::Node(NodeOptions o) : impl_(std::make_unique<Impl>()) {
    impl_->o = std::move(o);
    std::random_device rd;
    for (int k = 0; k < 16; ++k) impl_->id += std::format("{:02x}", rd() & 0xff);
}

Node::~Node() { close(); }

std::expected<std::shared_ptr<Node>, std::string> Node::connect(NodeOptions o) {
    std::shared_ptr<Node> n(new Node(std::move(o)));
    auto pair = native::pair();
    if (!pair) return std::unexpected("cannot make the node's wake channel");
    n->impl_->wake = std::move(*pair);
    n->impl_->self = n;
    n->impl_->raw = n.get();
    const auto opened = n->impl_->open();
    if (!opened && !n->impl_->o.patient) return std::unexpected(opened.error());
    n->impl_->io = std::jthread([impl = n->impl_.get()](std::stop_token stop) { impl->loop(stop); });
    if (opened) {
        std::unique_lock l(n->impl_->m);
        n->impl_->cv.wait_for(l, n->impl_->o.connect_wait, [&] { return n->impl_->welcomed; });
        if (!n->impl_->welcomed && !n->impl_->o.patient) return std::unexpected("the openbus router did not answer its hello");
    }
    if (n->impl_->o.telemetry) {
        // Everything on the bus is observable the same way (SPEC.md §8.3).
        OfferOptions t{.verb = "telemetry", .effect = "reversible", .interfaces = {"openbus.telemetry@0.1"},
                       .properties = {{"name", n->impl_->o.name}}};
        (void)n->offer("node/" + n->impl_->id + "/telemetry", std::move(t), [](Call c) {
            const auto answer = telemetry::control(c.body()).dump(-1, ' ', false, telemetry::Json::error_handler_t::replace);
            for (std::size_t at = 0; at < answer.size(); at += kPiece) c.reply(std::string_view(answer).substr(at, kPiece), kJson);
            c.end();
        });
    }
    return n;
}

void Node::close() {
    if (!impl_ || impl_->stopping.exchange(true)) return;
    impl_->kick();
    if (impl_->io.joinable()) {
        impl_->io.request_stop();
        impl_->io.join();
    }
    std::lock_guard l(impl_->m);
    if (impl_->conn) (void)impl_->conn->flush();
    impl_->conn.reset();
    impl_->wake[0].reset();
    impl_->wake[1].reset();
    impl_->cv.notify_all();
}

const std::string& Node::id() const { return impl_->id; }
std::string Node::root() const { std::lock_guard l(impl_->m); return impl_->root; }
std::string Node::router() const { std::lock_guard l(impl_->m); return impl_->router; }
bool Node::connected() const { std::lock_guard l(impl_->m); return impl_->welcomed; }

std::expected<void, CallError> Node::offer_stream(std::string path, OfferOptions o, StreamHandler handler, StreamOptions so) {
    o.kind = openbus::EndpointKind::stream;
    return offer_(std::move(path), std::move(o), {}, std::move(handler), so);
}

std::expected<void, CallError> Node::offer(std::string path, OfferOptions o, CallHandler handler) {
    return offer_(std::move(path), std::move(o), std::move(handler), {}, {});
}

std::expected<void, CallError> Node::offer_(std::string path, OfferOptions o, CallHandler handler, StreamHandler stream, StreamOptions so) {
    const auto ch = impl_->channel() | 0x40000000u;
    auto p = std::make_shared<Impl::Pending>();
    {
        std::lock_guard l(impl_->m);
        impl_->offered[path] = {o, std::move(handler), std::move(stream), so};
        // Said when the router answers (restore()).
        if (!impl_->welcomed) return {};
    }
    auto answer = impl_->ask(impl_->offer_frame(path, o, ch), p, std::chrono::seconds(5));
    if (!answer) {
        // The router went away meanwhile: offered again when one answers.
        if (answer.error().code == openbus::ErrorCode::gone) return {};
        std::lock_guard l(impl_->m);
        impl_->offered.erase(path);
        return std::unexpected(answer.error());
    }
    std::lock_guard l(impl_->m);
    if (!(*answer)->resolved.empty()) impl_->absolute[path] = (*answer)->resolved.front().path;
    return {};
}

void Node::withdraw(const std::string& path) {
    {
        std::lock_guard l(impl_->m);
        impl_->offered.erase(path);
    }
    impl_->send(control_(Kind::withdraw, 0, Fields{{"path", path}}));
}

std::expected<Stream, CallError> Node::open(std::string path, std::string_view body, StreamOptions so, std::chrono::milliseconds timeout) {
    std::uint32_t ch;
    { std::lock_guard l(impl_->m); ch = impl_->channel(); }
    auto st = impl_->stream_state(ch, path, so.window);
    st->body = std::string(body);
    {
        std::lock_guard l(impl_->m);
        if (!impl_->welcomed) return std::unexpected(CallError{openbus::ErrorCode::gone, "not connected to an openbus router"});
        impl_->streams[ch] = st;
    }
    Fields env{{"path", path}, {"credit", std::to_string(so.window)}};
    if (!impl_->send_with(Frame{.kind = static_cast<std::uint8_t>(Kind::stream_open), .channel = ch, .payload = openbus::address(env, body)}, {}))
        return std::unexpected(CallError{openbus::ErrorCode::gone, "not connected to an openbus router"});
    // Open once the other side grants its first credit; not, when it answers an error.
    std::unique_lock l(st->m);
    if (!st->cv.wait_for(l, timeout, [&] { return st->granted || st->ended; })) {
        l.unlock();
        Stream(st).close();
        return std::unexpected(CallError{openbus::ErrorCode::deadline, "the stream was not taken in time"});
    }
    if (st->ended) return std::unexpected(CallError{openbus::ErrorCode::not_found, st->error.empty() ? "the stream ended" : st->error});
    return Stream(st);
}

std::expected<std::vector<std::string>, CallError> Node::call(std::string path, std::string_view body, std::chrono::milliseconds timeout,
                                                              std::uint16_t codec) {
    std::uint32_t ch;
    { std::lock_guard l(impl_->m); ch = impl_->channel(); }
    auto p = std::make_shared<Impl::Pending>();
    impl_->m_calls.add();
    auto answer = impl_->ask(Frame{.kind = static_cast<std::uint8_t>(Kind::call), .codec = codec, .channel = ch,
                                   .payload = openbus::address(Fields{{"path", path}}, body)}, p, timeout);
    if (!answer) return std::unexpected(answer.error());
    return std::move((*answer)->replies);
}

std::expected<void, CallError> Node::call_each(std::string path, std::string_view body,
                                               const std::function<void(std::string_view)>& each, std::chrono::milliseconds timeout) {
    std::uint32_t ch;
    { std::lock_guard l(impl_->m); ch = impl_->channel(); }
    auto p = std::make_shared<Impl::Pending>();
    p->each = each;
    auto answer = impl_->ask(Frame{.kind = static_cast<std::uint8_t>(Kind::call), .channel = ch,
                                   .payload = openbus::address(Fields{{"path", path}}, body)}, p, timeout);
    if (!answer) return std::unexpected(answer.error());
    return {};
}

std::uint32_t Node::subscribe(std::string pattern, MessageHandler handler) {
    std::uint32_t ch;
    {
        std::lock_guard l(impl_->m);
        ch = impl_->channel();
        impl_->subs[ch] = {pattern, std::move(handler)};
    }
    impl_->send(Frame{.kind = static_cast<std::uint8_t>(Kind::subscribe), .channel = ch,
                      .payload = openbus::address(Fields{{"pattern", pattern}}, {})});
    return ch;
}

void Node::unsubscribe(std::uint32_t subscription) {
    {
        std::lock_guard l(impl_->m);
        impl_->subs.erase(subscription);
    }
    impl_->send(Frame{.kind = static_cast<std::uint8_t>(Kind::unsubscribe), .channel = subscription});
}

bool Node::publish(std::string path, std::string_view body, std::uint16_t codec, openbus::Reliability r) {
    impl_->m_published.add();
    return impl_->send(Frame{.kind = static_cast<std::uint8_t>(Kind::publish), .flags = openbus::with_reliability(0, r), .codec = codec,
                             .payload = openbus::address(Fields{{"path", path}}, body)});
}

bool Node::event(std::string path, std::uint64_t value) {
    std::string body(8, '\0');
    for (int k = 0; k < 8; ++k) body[static_cast<std::size_t>(k)] = static_cast<char>((value >> (8 * k)) & 0xff);
    return impl_->send(Frame{.kind = static_cast<std::uint8_t>(Kind::event),
                             .flags = openbus::with_reliability(0, openbus::Reliability::latest),
                             .payload = openbus::address(Fields{{"path", path}}, body)});
}

bool Node::broadcast(std::string_view body, openbus::Scope scope) {
    return impl_->send(Frame{.kind = static_cast<std::uint8_t>(Kind::broadcast),
                             .payload = openbus::address(Fields{{"scope", std::string(openbus::name_of(scope))}}, body)});
}

void Node::on_broadcast(MessageHandler handler) {
    std::lock_guard l(impl_->m);
    impl_->subs[0] = {"", std::move(handler)};
}

std::expected<std::vector<Resolved>, CallError> Node::resolve(std::string pattern, std::string interface, std::chrono::milliseconds timeout) {
    std::uint32_t ch;
    { std::lock_guard l(impl_->m); ch = impl_->channel(); }
    auto p = std::make_shared<Impl::Pending>();
    Fields ask{{"pattern", pattern}};
    if (!interface.empty()) ask.add("interface", interface);
    auto answer = impl_->ask(control_(Kind::resolve, ch, ask), p, timeout);
    if (!answer) return std::unexpected(answer.error());
    return std::move((*answer)->resolved);
}

std::expected<std::chrono::nanoseconds, CallError> Node::ping(std::chrono::milliseconds timeout) {
    std::uint32_t ch;
    { std::lock_guard l(impl_->m); ch = impl_->channel(); }
    auto p = std::make_shared<Impl::Pending>();
    auto answer = impl_->ask(Frame{.kind = static_cast<std::uint8_t>(Kind::ping), .channel = ch}, p, timeout);
    if (!answer) return std::unexpected(answer.error());
    return std::chrono::nanoseconds((*answer)->done_at - (*answer)->sent_at);
}

bool Node::send_frame(openbus::Frame f) { return impl_->send(f); }

// ── streams and regions ─────────────────────────────────────────────

std::uint32_t Region::id() const { return s_->id; }
std::size_t Region::size() const { return s_->size; }
std::span<std::byte> Region::bytes() const { return {s_->data(), s_->size}; }
bool Region::shared() const { return s_->shared; }

const std::string& Stream::path() const { return s_->path; }
const std::string& Stream::from() const { return s_->from; }
const std::string& Stream::body() const { return s_->body; }

bool Stream::ended() const {
    std::lock_guard l(s_->m);
    return s_->ended;
}

std::string Stream::error() const {
    std::lock_guard l(s_->m);
    return s_->error;
}

bool Stream::write(std::string_view bytes, std::chrono::milliseconds timeout) {
    constexpr std::size_t kPieceMax = 256u << 10;
    const auto until = std::chrono::steady_clock::now() + timeout;
    while (!bytes.empty()) {
        std::size_t n = 0;
        {
            std::unique_lock l(s_->m);
            if (!s_->cv.wait_until(l, until, [&] { return s_->credit > 0 || s_->ended; }) || s_->ended) return false;
            n = static_cast<std::size_t>(std::min<std::uint64_t>({s_->credit, bytes.size(), kPieceMax}));
            s_->credit -= n;
        }
        if (!s_->send(Frame{.kind = static_cast<std::uint8_t>(Kind::stream_data), .flags = openbus::with_priority(0, openbus::Priority::bulk),
                            .channel = s_->channel, .payload = std::string(bytes.substr(0, n))}, {}))
            return false;
        bytes.remove_prefix(n);
    }
    return true;
}

std::optional<Chunk> Stream::read(std::chrono::milliseconds timeout) {
    std::unique_lock l(s_->m);
    if (!s_->cv.wait_for(l, timeout, [&] { return !s_->inbox.empty() || s_->ended; }) || s_->inbox.empty()) return std::nullopt;
    auto [c, cost] = std::move(s_->inbox.front());
    s_->inbox.pop_front();
    // What was read is granted again, in halves of the window.
    s_->unacked += cost;
    if (s_->unacked >= s_->window / 2 && !s_->ended) {
        const auto grant = std::exchange(s_->unacked, 0);
        l.unlock();
        (void)s_->send(credit_frame_(s_->channel, grant), {});
    }
    return std::move(c);
}

std::expected<Region, std::string> Stream::create_region(std::size_t size, std::chrono::milliseconds wait) {
    auto r = std::make_shared<RegionState>();
    r->size = size;
    {
        std::lock_guard l(s_->m);
        if (s_->ended) return std::unexpected("the stream ended");
        r->id = s_->next_region++;
    }
    const auto announce = [&](bool shared) {
        Fields f{{"id", std::to_string(r->id)}, {"size", std::to_string(size)}, {"kind", shared ? "memfd" : "inline"},
                 {"coherence", "owned"}, {"tier", "hot"}};
        return s_->send(control_(Kind::region_announce, s_->channel, f), shared ? native::duplicate(r->mapped.handle) : Handle{});
    };
    const auto answered = [&]() -> int {
        std::unique_lock l(s_->m);
        s_->cv.wait_for(l, wait, [&] { return s_->answers.contains(r->id) || s_->ended; });
        const auto it = s_->answers.find(r->id);
        const int a = it == s_->answers.end() ? 0 : it->second;
        s_->answers.erase(r->id);
        return a;
    };
    if (auto mapped = native::create_shared("xbus-region", size)) {
        r->mapped = std::move(*mapped);
        r->shared = true;
        if (!announce(true)) return std::unexpected("the stream ended");
        if (const int a = answered(); a == 1) {
            std::lock_guard l(s_->m);
            s_->mine[r->id] = r;
            return Region(r);
        } else if (a == 0) {
            return std::unexpected("the other side did not answer the region in time");
        }
        // Refused (it came without its memory): inline from here, the bytes kept where they are.
        telemetry::metrics().counter("xbus.node.region_fallbacks").add();
        r->local.assign(r->mapped.data, r->mapped.data + size);
        native::unmap(r->mapped);
        r->shared = false;
    } else {
        r->local.resize(size);
    }
    if (!announce(false)) return std::unexpected("the stream ended");
    if (answered() != 1) return std::unexpected("the other side did not take the inline region");
    std::lock_guard l(s_->m);
    s_->mine[r->id] = r;
    return Region(r);
}

std::optional<Region> Stream::region(std::uint32_t id) const {
    std::lock_guard l(s_->m);
    const auto it = s_->theirs.find(id);
    if (it == s_->theirs.end()) return std::nullopt;
    return Region(it->second);
}

bool Stream::send_ref(const Region& r, std::uint64_t offset, std::uint64_t length, std::uint32_t version, std::chrono::milliseconds timeout) {
    if (offset + length > r.size()) return false;
    const auto until = std::chrono::steady_clock::now() + timeout;
    // A shared region's reference is 24 bytes; an inline one's carries the bytes, in pieces.
    constexpr std::uint64_t kInlinePiece = 512u << 10;
    std::uint64_t at = offset;
    const std::uint64_t end = offset + length;
    do {
        const std::uint64_t n = r.shared() ? length : std::min(kInlinePiece, end - at);
        auto payload = openbus::encode(openbus::RegionRef{r.id(), at, n, version});
        if (!r.shared()) payload.append(reinterpret_cast<const char*>(r.bytes().data() + at), static_cast<std::size_t>(n));
        {
            std::unique_lock l(s_->m);
            if (!s_->cv.wait_until(l, until, [&] { return s_->credit >= payload.size() || s_->ended; }) || s_->ended) return false;
            s_->credit -= payload.size();
        }
        if (!s_->send(Frame{.kind = static_cast<std::uint8_t>(Kind::stream_data),
                            .flags = static_cast<std::uint16_t>(openbus::flag::region_ref),
                            .channel = s_->channel, .payload = std::move(payload)}, {}))
            return false;
        at += n;
    } while (!r.shared() && at < end);
    return true;
}

void Stream::close() {
    {
        std::lock_guard l(s_->m);
        if (s_->ended) return;
    }
    (void)s_->send(Frame{.kind = static_cast<std::uint8_t>(Kind::stream_close), .channel = s_->channel}, {});
    s_->end();
    if (s_->forget) s_->forget();
}

// ── answering ───────────────────────────────────────────────────────

bool Call::reply(std::string_view body, std::uint16_t codec) const {
    auto n = node_.lock();
    if (!n || (ended_ && ended_->load())) return false;
    return n->impl_->send(Frame{.kind = static_cast<std::uint8_t>(Kind::reply), .codec = codec, .channel = channel_, .payload = std::string(body)});
}

bool Call::end() const {
    auto n = node_.lock();
    if (!n || !ended_ || ended_->exchange(true)) return false;
    {
        std::lock_guard l(n->impl_->m);
        n->impl_->answering.erase(channel_);
    }
    return n->impl_->send(Frame{.kind = static_cast<std::uint8_t>(Kind::reply_end), .channel = channel_});
}

bool Call::state(openbus::TaskState s, std::string_view detail) const {
    Fields f{{"state", std::string(openbus::name_of(s))}};
    if (!detail.empty()) f.add("detail", std::string(detail));
    return reply(f.encode(), static_cast<std::uint16_t>(openbus::Codec::fields));
}

bool Call::fail(openbus::ErrorCode code, std::string_view message) const {
    auto n = node_.lock();
    if (!n || !ended_ || ended_->exchange(true)) return false;
    {
        std::lock_guard l(n->impl_->m);
        n->impl_->answering.erase(channel_);
    }
    return n->impl_->send(control_(Kind::error, channel_, Fields{{"code", std::string(openbus::name_of(code))}, {"message", std::string(message)}}));
}

}  // namespace xbus
