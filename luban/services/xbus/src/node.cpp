module xbus.node;

import std;
import openbus;
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

CallError error_of_(const Frame& f) {
    const auto fields = Fields::decode(f.payload);
    return CallError{fields ? openbus::error_code(fields->value("code")).value_or(openbus::ErrorCode::malformed) : openbus::ErrorCode::malformed,
                     fields ? fields->value("message") : std::string("a malformed error")};
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

    struct Offered { OfferOptions o; CallHandler handler; };
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

    std::uint32_t channel() { return next_channel++ & 0x7fffffffu; }

    void kick() {
        if (wake[1].valid()) (void)native::write(wake[1], "w");
    }

    bool send(const Frame& f) {
        std::lock_guard l(m);
        if (!conn || !welcomed) return false;
        conn->push(f);
        kick();
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
        switch (f.known()) {
        case Kind::welcome: {
            const auto fields = Fields::decode(f.payload);
            std::lock_guard l(m);
            welcomed = true;
            root = fields ? fields->value("root") : std::string{};
            router = fields ? fields->value("router") : std::string{};
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
                    if ((a != absolute.end() ? a->second : given) == path || given == path) { handler = of.handler; break; }
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
            if (handler) handler(std::move(c));
            else c.fail(openbus::ErrorCode::not_found, "this node answers nothing at " + path);
            break;
        }
        case Kind::cancel: {
            std::lock_guard l(m);
            if (auto it = answering.find(f.channel); it != answering.end()) it->second->store(true);
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
                    // What was asked of the router that is gone is answered gone.
                    for (auto& [ch, pnd] : pending) {
                        pnd->error = CallError{openbus::ErrorCode::gone, "the router went away"};
                        pnd->done = true;
                    }
                    pending.clear();
                    answering.clear();
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
            return std::unexpected(CallError{openbus::ErrorCode::deadline, "no answer in time"});
        }
        if (p->error) return std::unexpected(*p->error);
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
    if (auto opened = n->impl_->open(); !opened) return std::unexpected(opened.error());
    n->impl_->io = std::jthread([impl = n->impl_.get()](std::stop_token stop) { impl->loop(stop); });
    std::unique_lock l(n->impl_->m);
    n->impl_->cv.wait_for(l, n->impl_->o.connect_wait, [&] { return n->impl_->welcomed; });
    if (!n->impl_->welcomed) return std::unexpected("the openbus router did not answer its hello");
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

std::expected<void, CallError> Node::offer(std::string path, OfferOptions o, CallHandler handler) {
    const auto ch = impl_->channel() | 0x40000000u;
    auto p = std::make_shared<Impl::Pending>();
    {
        std::lock_guard l(impl_->m);
        impl_->offered[path] = {o, std::move(handler)};
    }
    auto answer = impl_->ask(impl_->offer_frame(path, o, ch), p, std::chrono::seconds(5));
    if (!answer) {
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

std::expected<std::vector<std::string>, CallError> Node::call(std::string path, std::string_view body, std::chrono::milliseconds timeout,
                                                              std::uint16_t codec) {
    std::uint32_t ch;
    { std::lock_guard l(impl_->m); ch = impl_->channel(); }
    auto p = std::make_shared<Impl::Pending>();
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
